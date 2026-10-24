#include "StormModApi.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <string>


namespace
{
const StormModHostApi* g_host{};
void* g_workerLoop{};

// rk_theora.dll v1.0.2.0: the video worker thread procedure and the decode loop it calls with "this" in esi.
constexpr std::uintptr_t WorkerThreadRva = 0x92C0;
constexpr std::uintptr_t WorkerLoopRva = 0x9890;

// push esi; mov esi, [esp+8]; call WorkerLoop; pop esi; ret 4
constexpr std::array<std::uint8_t, 14> OriginalWorkerThread{
    0x56, 0x8B, 0x74, 0x24, 0x08, 0xE8, 0xC6, 0x05, 0x00, 0x00, 0x5E, 0xC2, 0x04, 0x00
};

void Log(const wchar_t* message)
{
    if (g_host && g_host->log)
    {
        g_host->log(L"VideoCrashFix", message);
    }
}

// The player's destructor sets the stop flag (+88h) and deletes the critical section the worker locks first.
// A worker that started after that point locked freed memory, which is what killed the game a few seconds
// into the final video. Such a worker now leaves before touching the player.
__declspec(naked) void GuardedWorkerThread()
{
    __asm
    {
        push esi
        mov esi, [esp + 8]
        test esi, esi
        jz done
        cmp byte ptr [esi + 88h], 0
        jnz done
        call dword ptr [g_workerLoop]
    done:
        pop esi
        ret 4
    }
}

enum class PatchResult
{
    Patched,
    AlreadyFixed,
    Failed,
};

PatchResult PatchWorkerThread(std::uint8_t* base)
{
    auto* site = base + WorkerThreadRva;
    if (std::memcmp(site, OriginalWorkerThread.data(), OriginalWorkerThread.size()) != 0)
    {
        // A copy of the DLL patched on disk with the same guard starts with a jump into its code cave.
        if (site[0] == 0xE9)
        {
            Log(L"rk_theora.dll is already patched on disk; nothing to do");
            return PatchResult::AlreadyFixed;
        }
        Log(L"Unsupported rk_theora.dll: worker thread bytes differ");
        return PatchResult::Failed;
    }

    DWORD oldProtection{};
    if (!VirtualProtect(site, OriginalWorkerThread.size(), PAGE_EXECUTE_READWRITE, &oldProtection))
    {
        Log(L"Unable to unprotect the video worker thread");
        return PatchResult::Failed;
    }
    g_workerLoop = base + WorkerLoopRva;
    const auto displacement = reinterpret_cast<std::intptr_t>(&GuardedWorkerThread) -
        reinterpret_cast<std::intptr_t>(site + 5);
    std::memset(site, 0x90, OriginalWorkerThread.size());
    site[0] = 0xE9;
    std::memcpy(site + 1, &displacement, sizeof(std::int32_t));
    DWORD ignored{};
    VirtualProtect(site, OriginalWorkerThread.size(), oldProtection, &ignored);
    FlushInstructionCache(GetCurrentProcess(), site, OriginalWorkerThread.size());
    return PatchResult::Patched;
}
}


extern "C" __declspec(dllexport) BOOL __cdecl StormMod_Initialize(const StormModHostApi* host)
{
    if (!host || host->version != StormModApiVersion)
    {
        return FALSE;
    }
    g_host = host;

    // The game loads the decoder by name only when its video player starts; loading it now from the game folder
    // gives the game this same, already patched module later. The reference is kept for the whole session.
    const auto path = std::wstring(host->gameDirectory) + L"\\rk_theora.dll";
    const auto theora = LoadLibraryW(path.c_str());
    if (!theora)
    {
        Log(L"rk_theora.dll not found");
        return FALSE;
    }
    const auto result = PatchWorkerThread(reinterpret_cast<std::uint8_t*>(theora));
    if (result != PatchResult::Patched)
    {
        FreeLibrary(theora);
        return result == PatchResult::AlreadyFixed;
    }
    Log(L"Video worker race fixed: the final video no longer crashes the game");
    return TRUE;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, void*)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(instance);
    }
    return TRUE;
}
