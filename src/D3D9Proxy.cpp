#include <Windows.h>
#include <d3d9.h>

#include <array>
#include <string>


namespace
{
using Direct3DCreate9Fn = IDirect3D9*(WINAPI*)(UINT);
using NotifyDirect3D9CreatedFn = void(__cdecl*)(void*);

INIT_ONCE g_initializeOnce = INIT_ONCE_STATIC_INIT;
HMODULE g_realD3D9{};
HMODULE g_loader{};
Direct3DCreate9Fn g_realDirect3DCreate9{};
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
            const auto result = WaitForSingleObject(readyEvent, 30000);
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

    // A missing or stalled loader must not prevent the game from creating a native device.
    if (g_loader && WaitForLoaderReady())
    {
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

    auto* direct3D = g_realDirect3DCreate9(sdkVersion);
    if (direct3D && g_notifyDirect3D9Created)
    {
        g_notifyDirect3D9Created(direct3D);
    }
    return direct3D;
}

namespace
{
// Storm.exe has no DPI manifest, so on a scaled display Windows renders it at the logical resolution and
// stretches the result: a blurry image, small intro windows and a mouse confined to part of the screen.
// The game imports d3d9.dll statically, so this runs before it creates any window.
void EnableDpiAwareness()
{
    const auto user32 = GetModuleHandleW(L"user32.dll");
    if (!user32)
    {
        return;
    }
    using SetContextFn = BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT);
    using SetAwareFn = BOOL(WINAPI*)();
    if (const auto setContext = reinterpret_cast<SetContextFn>(
            GetProcAddress(user32, "SetProcessDpiAwarenessContext"));
        setContext && setContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2))
    {
        return;
    }
    if (const auto setAware = reinterpret_cast<SetAwareFn>(GetProcAddress(user32, "SetProcessDPIAware")))
    {
        setAware();
    }
}
}


BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, void*)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(instance);
        EnableDpiAwareness();
    }
    return TRUE;
}
