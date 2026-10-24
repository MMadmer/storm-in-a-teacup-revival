#include <Windows.h>
#include <TlHelp32.h>

#include <array>
#include <cstdio>
#include <cwchar>
#include <string>


namespace
{
class UniqueHandle
{
public:
    explicit UniqueHandle(HANDLE handle = nullptr) : handle_(handle) {}
    ~UniqueHandle()
    {
        if (handle_ && handle_ != INVALID_HANDLE_VALUE)
        {
            CloseHandle(handle_);
        }
    }

    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;

    [[nodiscard]] HANDLE get() const { return handle_; }
    [[nodiscard]] explicit operator bool() const
    {
        return handle_ && handle_ != INVALID_HANDLE_VALUE;
    }

private:
    HANDLE handle_{};
};

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

DWORD FindProcessId(const wchar_t* executableName)
{
    UniqueHandle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    if (!snapshot)
    {
        return 0;
    }

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (!Process32FirstW(snapshot.get(), &entry))
    {
        return 0;
    }

    do
    {
        if (_wcsicmp(entry.szExeFile, executableName) == 0)
        {
            return entry.th32ProcessID;
        }
    } while (Process32NextW(snapshot.get(), &entry));
    return 0;
}

std::uintptr_t FindRemoteModuleBase(DWORD pid, const wchar_t* moduleName)
{
    for (int attempt = 0; attempt < 20; ++attempt)
    {
        UniqueHandle snapshot(CreateToolhelp32Snapshot(
            TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32,
            pid
        ));
        if (!snapshot)
        {
            Sleep(10);
            continue;
        }

        MODULEENTRY32W entry{};
        entry.dwSize = sizeof(entry);
        if (!Module32FirstW(snapshot.get(), &entry))
        {
            Sleep(10);
            continue;
        }

        do
        {
            if (_wcsicmp(entry.szModule, moduleName) == 0)
            {
                return reinterpret_cast<std::uintptr_t>(entry.modBaseAddr);
            }
        } while (Module32NextW(snapshot.get(), &entry));
        Sleep(10);
    }
    return 0;
}

bool WaitForLoaderReady(DWORD pid)
{
    wchar_t eventName[64]{};
    _snwprintf_s(eventName, _TRUNCATE, L"Local\\StormModLoaderReady_%lu", pid);
    for (int attempt = 0; attempt < 500; ++attempt)
    {
        UniqueHandle readyEvent(OpenEventW(SYNCHRONIZE, FALSE, eventName));
        if (readyEvent)
        {
            return WaitForSingleObject(readyEvent.get(), 30000) == WAIT_OBJECT_0;
        }
        Sleep(10);
    }
    return false;
}

bool InjectLoader(DWORD pid, const std::wstring& loaderPath)
{
    UniqueHandle process(OpenProcess(
        PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION |
            PROCESS_VM_WRITE | PROCESS_VM_READ,
        FALSE,
        pid
    ));
    if (!process)
    {
        std::wprintf(L"OpenProcess failed: %lu\n", GetLastError());
        return false;
    }

    const auto localKernel32 = GetModuleHandleW(L"kernel32.dll");
    const auto localLoadLibrary = GetProcAddress(localKernel32, "LoadLibraryW");
    HMODULE localOwner{};
    if (!localKernel32 || !localLoadLibrary || !GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(localLoadLibrary),
            &localOwner
        ))
    {
        std::wprintf(L"Could not resolve the local LoadLibraryW owner\n");
        return false;
    }

    std::array<wchar_t, MAX_PATH> ownerPath{};
    if (!GetModuleFileNameW(localOwner, ownerPath.data(), static_cast<DWORD>(ownerPath.size())))
    {
        std::wprintf(L"Could not resolve the LoadLibraryW module name\n");
        return false;
    }
    const auto ownerName = std::wstring(ownerPath.data()).substr(
        std::wstring(ownerPath.data()).find_last_of(L"\\/") + 1
    );
    const auto remoteOwner = FindRemoteModuleBase(pid, ownerName.c_str());
    if (!remoteOwner)
    {
        std::wprintf(L"Could not find remote %s\n", ownerName.c_str());
        return false;
    }

    const auto loadLibraryOffset = reinterpret_cast<std::uintptr_t>(localLoadLibrary) -
        reinterpret_cast<std::uintptr_t>(localOwner);
    const auto remoteLoadLibrary = reinterpret_cast<LPTHREAD_START_ROUTINE>(
        remoteOwner + loadLibraryOffset
    );
    const auto pathBytes = (loaderPath.size() + 1) * sizeof(wchar_t);
    const auto remotePath = VirtualAllocEx(
        process.get(),
        nullptr,
        pathBytes,
        MEM_COMMIT | MEM_RESERVE,
        PAGE_READWRITE
    );
    if (!remotePath)
    {
        std::wprintf(L"VirtualAllocEx failed: %lu\n", GetLastError());
        return false;
    }

    SIZE_T written{};
    const auto wrotePath = WriteProcessMemory(
        process.get(),
        remotePath,
        loaderPath.c_str(),
        pathBytes,
        &written
    );
    if (!wrotePath || written != pathBytes)
    {
        std::wprintf(L"WriteProcessMemory failed: %lu\n", GetLastError());
        VirtualFreeEx(process.get(), remotePath, 0, MEM_RELEASE);
        return false;
    }

    UniqueHandle thread(CreateRemoteThread(
        process.get(),
        nullptr,
        0,
        remoteLoadLibrary,
        remotePath,
        0,
        nullptr
    ));
    if (!thread)
    {
        std::wprintf(L"CreateRemoteThread failed: %lu\n", GetLastError());
        VirtualFreeEx(process.get(), remotePath, 0, MEM_RELEASE);
        return false;
    }

    const auto waitResult = WaitForSingleObject(thread.get(), 15000);
    DWORD remoteModule{};
    GetExitCodeThread(thread.get(), &remoteModule);
    VirtualFreeEx(process.get(), remotePath, 0, MEM_RELEASE);
    if (waitResult != WAIT_OBJECT_0 || !remoteModule)
    {
        std::wprintf(L"Remote LoadLibraryW failed\n");
        return false;
    }
    if (!WaitForLoaderReady(pid))
    {
        std::wprintf(L"StormModLoader did not report ready state\n");
        return false;
    }
    return true;
}
}


int wmain(int argc, wchar_t** argv)
{
    const auto gameDirectory = GetExecutableDirectory();
    const auto loaderPath = gameDirectory + L"\\StormModLoader.dll";
    const auto gamePath = gameDirectory + L"\\Storm.exe";
    const auto proxyPath = gameDirectory + L"\\d3d9.dll";
    if (GetFileAttributesW(loaderPath.c_str()) == INVALID_FILE_ATTRIBUTES)
    {
        std::wprintf(L"StormModLoader.dll not found next to the launcher\n");
        return 1;
    }

    if (argc >= 2 && _wcsicmp(argv[1], L"--attach") == 0)
    {
        const auto pid = argc >= 3 ? wcstoul(argv[2], nullptr, 10) : FindProcessId(L"Storm.exe");
        if (!pid)
        {
            std::wprintf(L"Storm.exe is not running\n");
            return 1;
        }
        return InjectLoader(pid, loaderPath) ? 0 : 1;
    }

    if (GetFileAttributesW(proxyPath.c_str()) != INVALID_FILE_ATTRIBUTES)
    {
        if (FindProcessId(L"Storm.exe"))
        {
            return 0;
        }

        STARTUPINFOW proxyStartupInfo{};
        proxyStartupInfo.cb = sizeof(proxyStartupInfo);
        PROCESS_INFORMATION proxyProcessInfo{};
        std::wstring proxyCommandLine = L"\"" + gamePath + L"\"";
        if (!CreateProcessW(
                gamePath.c_str(),
                proxyCommandLine.data(),
                nullptr,
                nullptr,
                FALSE,
                0,
                nullptr,
                gameDirectory.c_str(),
                &proxyStartupInfo,
                &proxyProcessInfo
            ))
        {
            std::wprintf(L"CreateProcessW failed: %lu\n", GetLastError());
            return 1;
        }
        CloseHandle(proxyProcessInfo.hThread);
        CloseHandle(proxyProcessInfo.hProcess);
        return 0;
    }

    if (const auto runningPid = FindProcessId(L"Storm.exe"))
    {
        return InjectLoader(runningPid, loaderPath) ? 0 : 1;
    }

    STARTUPINFOW startupInfo{};
    startupInfo.cb = sizeof(startupInfo);
    PROCESS_INFORMATION processInfo{};
    std::wstring commandLine = L"\"" + gamePath + L"\"";
    if (!CreateProcessW(
            gamePath.c_str(),
            commandLine.data(),
            nullptr,
            nullptr,
            FALSE,
            CREATE_SUSPENDED,
            nullptr,
            gameDirectory.c_str(),
            &startupInfo,
            &processInfo
        ))
    {
        std::wprintf(L"CreateProcessW failed: %lu\n", GetLastError());
        return 1;
    }

    UniqueHandle process(processInfo.hProcess);
    UniqueHandle mainThread(processInfo.hThread);

    // Toolhelp cannot enumerate imported modules before the initial loader pass.
    ResumeThread(mainThread.get());
    if (!FindRemoteModuleBase(processInfo.dwProcessId, L"kernel32.dll"))
    {
        TerminateProcess(process.get(), 1);
        std::wprintf(L"The game loader did not initialize imported modules\n");
        return 1;
    }
    SuspendThread(mainThread.get());

    if (!InjectLoader(processInfo.dwProcessId, loaderPath))
    {
        TerminateProcess(process.get(), 1);
        return 1;
    }

    ResumeThread(mainThread.get());
    return 0;
}
