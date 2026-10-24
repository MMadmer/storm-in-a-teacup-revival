#include "StormModApi.h"
#include "GamePatches.h"

#include <Windows.h>
#include <d3d9.h>

#include <algorithm>
#include <array>
#include <cwchar>
#include <string>


namespace
{
using CreateDeviceFn = HRESULT(WINAPI*)(
    IDirect3D9*,
    UINT,
    D3DDEVTYPE,
    HWND,
    DWORD,
    D3DPRESENT_PARAMETERS*,
    IDirect3DDevice9**
);

struct Settings
{
    bool highPerformanceGpu{true};
    GamePatches::Options patches{true, true, true, true};
};

const StormModHostApi* g_host{};
HMODULE g_module{};
Settings g_settings{};
CreateDeviceFn g_originalCreateDevice{};

void __cdecl Log(const wchar_t* message)
{
    if (g_host && g_host->log)
    {
        g_host->log(L"PerformanceOptimizer", message);
    }
}

std::wstring ModuleDirectory()
{
    std::array<wchar_t, MAX_PATH> path{};
    const auto length = GetModuleFileNameW(g_module, path.data(), static_cast<DWORD>(path.size()));
    std::wstring result(path.data(), length);
    result.resize(result.find_last_of(L"\\/") + 1);
    return result;
}

// Benchmark runs disable single optimizations through STORM_OPTIMIZER, e.g. "nomath,noaudio".
bool HasDevFlag(const wchar_t* flag)
{
    std::array<wchar_t, 256> value{};
    const auto length = GetEnvironmentVariableW(L"STORM_OPTIMIZER", value.data(), static_cast<DWORD>(value.size()));
    if (!length || length >= value.size())
    {
        return false;
    }
    const auto flags = L"," + std::wstring(value.data(), length) + L",";
    return flags.find(L"," + std::wstring(flag) + L",") != std::wstring::npos;
}

void LoadSettings()
{
    const auto path = ModuleDirectory() + L"PerformanceOptimizer.ini";
    const auto read = [&path](const wchar_t* key, const wchar_t* devFlag)
    {
        return GetPrivateProfileIntW(L"Optimizer", key, 1, path.c_str()) != 0 && !HasDevFlag(devFlag);
    };
    g_settings.highPerformanceGpu = read(L"HighPerformanceGpu", L"nogpu");
    g_settings.patches.fastParticles = read(L"FastParticles", L"noparticles");
    g_settings.patches.physicsGrid = read(L"PhysicsGridFix", L"nogrid");
    g_settings.patches.fastMath = read(L"FastMath", L"nomath");
    g_settings.patches.fastAudio = read(L"FastAudio", L"noaudio");
}

// Adds GpuPreference=2 to Storm.exe's per-app graphics settings, the same entry Windows Settings > Display >
// Graphics writes. A GPU choice already made there is kept, and so are the other settings in the same value.
void RequestHighPerformanceGpu()
{
    std::array<wchar_t, MAX_PATH> executablePath{};
    const auto length = GetModuleFileNameW(nullptr, executablePath.data(), static_cast<DWORD>(executablePath.size()));
    if (!length || length == executablePath.size())
    {
        return;
    }

    HKEY key{};
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\DirectX\\UserGpuPreferences", 0, nullptr, 0,
            KEY_QUERY_VALUE | KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS)
    {
        Log(L"Unable to open the Windows GPU preference registry key");
        return;
    }

    std::wstring stored;
    DWORD type{};
    DWORD size{};
    const auto status = RegQueryValueExW(key, executablePath.data(), nullptr, &type, nullptr, &size);
    auto readable = status == ERROR_FILE_NOT_FOUND;
    if (status == ERROR_SUCCESS && type == REG_SZ)
    {
        // The extra zeroed element terminates a stored string that lacks its own terminator.
        stored.resize(size / sizeof(wchar_t) + 1);
        size = static_cast<DWORD>(stored.size() * sizeof(wchar_t));
        if (RegQueryValueExW(key, executablePath.data(), nullptr, nullptr, reinterpret_cast<BYTE*>(stored.data()),
                &size) == ERROR_SUCCESS)
        {
            stored.resize(std::wcslen(stored.c_str()));
            readable = true;
        }
    }

    // Windows stores "Key=value;" pairs. Tokens without "=" are dropped: an earlier build of this mod saved a
    // truncated "Gp" there.
    std::wstring settings;
    auto hasPreference = false;
    for (std::size_t begin = 0; readable && begin < stored.size();)
    {
        const auto end = (std::min)(stored.find(L';', begin), stored.size());
        const auto token = stored.substr(begin, end - begin);
        if (token.find(L'=') != std::wstring::npos)
        {
            settings += token + L';';
            hasPreference = hasPreference || token.starts_with(L"GpuPreference=");
        }
        begin = end + 1;
    }
    if (!hasPreference)
    {
        settings += L"GpuPreference=2;";
    }
    if (!readable || settings == stored)
    {
        RegCloseKey(key);
        return;
    }
    const auto result = RegSetValueExW(key, executablePath.data(), 0, REG_SZ,
        reinterpret_cast<const BYTE*>(settings.c_str()), static_cast<DWORD>((settings.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(key);
    Log(result == ERROR_SUCCESS
        ? L"Saved the Windows high-performance GPU preference"
        : L"Unable to save the Windows GPU preference");
}

// The game's code is decrypted by now, so the code patches can find their signatures.
HRESULT WINAPI HookedCreateDevice(
    IDirect3D9* direct3D,
    UINT adapter,
    D3DDEVTYPE deviceType,
    HWND focusWindow,
    DWORD behaviorFlags,
    D3DPRESENT_PARAMETERS* parameters,
    IDirect3DDevice9** returnedDevice
)
{
    GamePatches::Install(g_host->gameModule, g_settings.patches, Log);
    return g_originalCreateDevice(direct3D, adapter, deviceType, focusWindow, behaviorFlags, parameters,
        returnedDevice);
}

template <typename Function>
Function PatchVtableEntry(void** vtable, std::size_t index, Function replacement)
{
    DWORD oldProtection{};
    if (!VirtualProtect(&vtable[index], sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtection))
    {
        return {};
    }
    const auto original = reinterpret_cast<Function>(
        InterlockedExchangePointer(&vtable[index], reinterpret_cast<void*>(replacement))
    );
    DWORD ignored{};
    VirtualProtect(&vtable[index], sizeof(void*), oldProtection, &ignored);
    return original;
}
}


extern "C" __declspec(dllexport) BOOL __cdecl StormMod_Initialize(const StormModHostApi* host)
{
    if (!host || host->version != StormModApiVersion)
    {
        return FALSE;
    }

    g_host = host;
    LoadSettings();
    if (g_settings.highPerformanceGpu)
    {
        RequestHighPerformanceGpu();
    }
    return TRUE;
}

extern "C" __declspec(dllexport) void __cdecl StormMod_OnDirect3D9Created(void* instance)
{
    auto* direct3D = static_cast<IDirect3D9*>(instance);
    if (!direct3D || g_originalCreateDevice)
    {
        return;
    }

    auto** vtable = *reinterpret_cast<void***>(direct3D);
    g_originalCreateDevice = PatchVtableEntry(vtable, 16, HookedCreateDevice);
    if (!g_originalCreateDevice)
    {
        Log(L"Device creation hook installation failed");
    }
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, void*)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        g_module = instance;
        DisableThreadLibraryCalls(instance);
    }
    return TRUE;
}
