#include <Windows.h>
#include <d3d9.h>

#include <array>
#include <string>


namespace
{
using Direct3DCreate9Fn = IDirect3D9*(WINAPI*)(UINT);
using CreateDirect3D9OverrideFn = void*(__cdecl*)(UINT);
using NotifyDirect3D9CreatedFn = void(__cdecl*)(void*);

INIT_ONCE g_initializeOnce = INIT_ONCE_STATIC_INIT;
HMODULE g_realD3D9{};
HMODULE g_loader{};
Direct3DCreate9Fn g_realDirect3DCreate9{};
CreateDirect3D9OverrideFn g_createDirect3D9Override{};
NotifyDirect3D9CreatedFn g_notifyDirect3D9Created{};

std::wstring GetExecutableDirectory()
{
    std::array<wchar_t, 32768> path{};
    const auto length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (!length || length == path.size())
    {
        return {};
    }

    std::wstring result(path.data(), length);
    result.resize(result.find_last_of(L"\\/"));
    return result;
}

bool WaitForLoaderReady()
{
    wchar_t eventName[64]{};
    _snwprintf_s(
        eventName,
        _TRUNCATE,
        L"Local\\StormModLoaderReady_%lu",
        GetCurrentProcessId()
    );
    for (int attempt = 0; attempt < 500; ++attempt)
    {
        const auto readyEvent = OpenEventW(SYNCHRONIZE, FALSE, eventName);
        if (readyEvent)
        {
            const auto result = WaitForSingleObject(readyEvent, 5000);
            CloseHandle(readyEvent);
            return result == WAIT_OBJECT_0;
        }
        Sleep(10);
    }
    return false;
}

BOOL CALLBACK InitializeProxy(PINIT_ONCE, void*, void**)
{
    std::array<wchar_t, MAX_PATH> systemDirectory{};
    const auto length = GetSystemDirectoryW(
        systemDirectory.data(),
        static_cast<UINT>(systemDirectory.size())
    );
    if (!length || length >= systemDirectory.size())
    {
        return FALSE;
    }

    const auto realPath = std::wstring(systemDirectory.data(), length) + L"\\d3d9.dll";
    g_realD3D9 = LoadLibraryW(realPath.c_str());
    if (!g_realD3D9)
    {
        return FALSE;
    }
    g_realDirect3DCreate9 = reinterpret_cast<Direct3DCreate9Fn>(
        GetProcAddress(g_realD3D9, "Direct3DCreate9")
    );
    if (!g_realDirect3DCreate9)
    {
        return FALSE;
    }

    const auto loaderPath = GetExecutableDirectory() + L"\\StormModLoader.dll";
    g_loader = LoadLibraryW(loaderPath.c_str());
    if (!g_loader)
    {
        g_loader = GetModuleHandleW(L"StormModLoader.dll");
    }
    if (!WaitForLoaderReady())
    {
        return FALSE;
    }
    if (!g_loader)
    {
        g_loader = GetModuleHandleW(L"StormModLoader.dll");
    }
    if (g_loader)
    {
        g_createDirect3D9Override = reinterpret_cast<CreateDirect3D9OverrideFn>(
            GetProcAddress(g_loader, "StormModLoader_CreateDirect3D9")
        );
        g_notifyDirect3D9Created = reinterpret_cast<NotifyDirect3D9CreatedFn>(
            GetProcAddress(g_loader, "StormModLoader_NotifyDirect3D9Created")
        );
    }
    return TRUE;
}
}


extern "C" IDirect3D9* WINAPI Direct3DCreate9(UINT sdkVersion)
{
    if (!InitOnceExecuteOnce(&g_initializeOnce, InitializeProxy, nullptr, nullptr))
    {
        return nullptr;
    }

    auto* direct3D = g_createDirect3D9Override
        ? static_cast<IDirect3D9*>(g_createDirect3D9Override(sdkVersion))
        : nullptr;
    if (!direct3D)
    {
        direct3D = g_realDirect3DCreate9(sdkVersion);
    }
    if (direct3D && g_notifyDirect3D9Created)
    {
        g_notifyDirect3D9Created(direct3D);
    }
    return direct3D;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, void*)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(instance);
    }
    return TRUE;
}
