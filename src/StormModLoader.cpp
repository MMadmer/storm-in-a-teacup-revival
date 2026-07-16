#include "StormModApi.h"

#include <Windows.h>

#include <array>
#include <cwctype>
#include <cwchar>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>


namespace
{
struct LoadedMod
{
    HMODULE module{};
    StormModShutdown shutdown{};
};

HMODULE g_loaderModule{};
HANDLE g_readyEvent{};
std::wstring g_gameDirectory;
std::wstring g_logPath;
std::wstring g_modConfigPath;
std::mutex g_logMutex;
std::vector<LoadedMod> g_loadedMods;

std::wstring GetModuleDirectory(HMODULE module)
{
    std::array<wchar_t, 32768> path{};
    const auto length = GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
    if (!length || length == path.size())
    {
        return {};
    }

    std::wstring result(path.data(), length);
    if (const auto separator = result.find_last_of(L"\\/"); separator != std::wstring::npos)
    {
        result.resize(separator);
    }
    return result;
}

void __cdecl Log(const wchar_t* modName, const wchar_t* message)
{
    std::scoped_lock lock(g_logMutex);
    const auto file = CreateFileW(
        g_logPath.c_str(),
        FILE_APPEND_DATA,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr
    );
    if (!file || file == INVALID_HANDLE_VALUE)
    {
        return;
    }

    SYSTEMTIME time{};
    GetLocalTime(&time);
    wchar_t line[2048]{};
    _snwprintf_s(
        line,
        _TRUNCATE,
        L"[%02u:%02u:%02u.%03u] [%s] %s\r\n",
        time.wHour,
        time.wMinute,
        time.wSecond,
        time.wMilliseconds,
        modName ? modName : L"Loader",
        message ? message : L""
    );

    const auto byteCount = static_cast<DWORD>(std::wcslen(line) * sizeof(wchar_t));
    DWORD written{};
    WriteFile(file, line, byteCount, &written, nullptr);
    CloseHandle(file);
}

std::wstring GetModId(const std::wstring& path)
{
    const auto fileSeparator = path.find_last_of(L"\\/");
    const auto parentSeparator = path.find_last_of(L"\\/", fileSeparator - 1);
    const auto parent = path.substr(parentSeparator + 1, fileSeparator - parentSeparator - 1);
    if (_wcsicmp(parent.c_str(), L"Mods") != 0)
    {
        return parent;
    }

    const auto fileName = path.substr(fileSeparator + 1);
    if (const auto extension = fileName.find_last_of(L'.'); extension != std::wstring::npos)
    {
        return fileName.substr(0, extension);
    }
    return fileName;
}

bool IsModEnabled(const std::wstring& modId)
{
    return GetPrivateProfileIntW(L"Mods", modId.c_str(), 1, g_modConfigPath.c_str()) != 0;
}

void TryLoadMod(const std::wstring& path, std::unordered_set<std::wstring>& loadedPaths)
{
    const auto modId = GetModId(path);
    if (!IsModEnabled(modId))
    {
        Log(L"Loader", (L"Skipped disabled mod: " + modId).c_str());
        return;
    }

    auto normalized = path;
    for (auto& character : normalized)
    {
        character = static_cast<wchar_t>(std::towlower(character));
    }
    if (!loadedPaths.emplace(normalized).second)
    {
        return;
    }

    const auto module = LoadLibraryW(path.c_str());
    if (!module)
    {
        Log(L"Loader", (L"LoadLibraryW failed for " + path).c_str());
        return;
    }

    const auto initialize = reinterpret_cast<StormModInitialize>(
        GetProcAddress(module, "StormMod_Initialize")
    );
    if (!initialize)
    {
        Log(L"Loader", (L"Missing StormMod_Initialize in " + path).c_str());
        FreeLibrary(module);
        return;
    }

    static StormModHostApi host{};
    host = {
        sizeof(StormModHostApi),
        StormModApiVersion,
        GetModuleHandleW(nullptr),
        g_gameDirectory.c_str(),
        Log,
    };
    if (!initialize(&host))
    {
        Log(L"Loader", (L"Initialization failed for " + path).c_str());
        FreeLibrary(module);
        return;
    }

    const auto shutdown = reinterpret_cast<StormModShutdown>(
        GetProcAddress(module, "StormMod_Shutdown")
    );
    g_loadedMods.push_back({module, shutdown});
}

void EnumeratePattern(
    const std::wstring& pattern,
    std::unordered_set<std::wstring>& loadedPaths
)
{
    WIN32_FIND_DATAW findData{};
    const auto search = FindFirstFileW(pattern.c_str(), &findData);
    if (!search || search == INVALID_HANDLE_VALUE)
    {
        return;
    }

    const auto separator = pattern.find_last_of(L"\\/");
    const auto directory = pattern.substr(0, separator);
    do
    {
        if (!(findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
        {
            TryLoadMod(directory + L"\\" + findData.cFileName, loadedPaths);
        }
    } while (FindNextFileW(search, &findData));
    FindClose(search);
}

DWORD WINAPI LoadModsThread(void*)
{
    wchar_t readyEventName[64]{};
    _snwprintf_s(
        readyEventName,
        _TRUNCATE,
        L"Local\\StormModLoaderReady_%lu",
        GetCurrentProcessId()
    );
    g_readyEvent = CreateEventW(nullptr, TRUE, FALSE, readyEventName);

    g_gameDirectory = GetModuleDirectory(nullptr);
    g_logPath = g_gameDirectory + L"\\Mods\\StormModLoader.log";
    g_modConfigPath = g_gameDirectory + L"\\Mods\\Mods.ini";
    CreateDirectoryW((g_gameDirectory + L"\\Mods").c_str(), nullptr);
    DeleteFileW(g_logPath.c_str());

    std::unordered_set<std::wstring> loadedPaths;
    EnumeratePattern(g_gameDirectory + L"\\Mods\\*.dll", loadedPaths);

    WIN32_FIND_DATAW directoryData{};
    const auto directorySearch = FindFirstFileW(
        (g_gameDirectory + L"\\Mods\\*").c_str(),
        &directoryData
    );
    if (directorySearch && directorySearch != INVALID_HANDLE_VALUE)
    {
        do
        {
            const auto isDirectory = directoryData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY;
            const auto isDot = directoryData.cFileName[0] == L'.';
            if (isDirectory && !isDot)
            {
                EnumeratePattern(
                    g_gameDirectory + L"\\Mods\\" + directoryData.cFileName + L"\\*.dll",
                    loadedPaths
                );
            }
        } while (FindNextFileW(directorySearch, &directoryData));
        FindClose(directorySearch);
    }

    wchar_t summary[64]{};
    _snwprintf_s(
        summary,
        _TRUNCATE,
        L"Loaded %zu mod(s)",
        g_loadedMods.size()
    );
    Log(L"Loader", summary);
    if (g_readyEvent)
    {
        SetEvent(g_readyEvent);
    }
    return 0;
}
}


extern "C" __declspec(dllexport) void __cdecl StormModLoader_NotifyDirect3D9Created(
    void* direct3D
)
{
    for (const auto& mod : g_loadedMods)
    {
        const auto notify = reinterpret_cast<void(__cdecl*)(void*)>(
            GetProcAddress(mod.module, "StormMod_OnDirect3D9Created")
        );
        if (notify)
        {
            notify(direct3D);
        }
    }
}

extern "C" __declspec(dllexport) void* __cdecl StormModLoader_CreateDirect3D9(
    unsigned int sdkVersion
)
{
    for (const auto& mod : g_loadedMods)
    {
        const auto create = reinterpret_cast<void*(__cdecl*)(unsigned int)>(
            GetProcAddress(mod.module, "StormMod_CreateDirect3D9")
        );
        if (create)
        {
            if (const auto direct3D = create(sdkVersion))
            {
                return direct3D;
            }
        }
    }
    return nullptr;
}


BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, void*)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        g_loaderModule = instance;
        DisableThreadLibraryCalls(instance);
        const auto thread = CreateThread(nullptr, 0, LoadModsThread, nullptr, 0, nullptr);
        if (thread)
        {
            CloseHandle(thread);
        }
    }
    return TRUE;
}
