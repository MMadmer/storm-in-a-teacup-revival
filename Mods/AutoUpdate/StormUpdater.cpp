#include "UpdateCommon.h"

#include <shellapi.h>

#include <array>
#include <exception>
#include <optional>
#include <ranges>
#include <string>
#include <vector>


namespace
{
struct Options
{
    std::filesystem::path game;
    std::filesystem::path staging;
    DWORD pid{};
    bool elevated{};
};

enum class Result
{
    Updated,
    RolledBack,
    // The rollback itself failed: the installation may be mixed until the next update repairs it.
    Damaged,
};

std::optional<Options> ParseOptions()
{
    auto count = 0;
    auto** arguments = CommandLineToArgvW(GetCommandLineW(), &count);
    if (!arguments)
    {
        return std::nullopt;
    }
    Options options;
    for (auto index = 1; index < count; ++index)
    {
        const std::wstring_view name = arguments[index];
        const auto hasValue = index + 1 < count;
        if (name == L"--elevated")
        {
            options.elevated = true;
        }
        else if (name == L"--game" && hasValue)
        {
            options.game = arguments[++index];
        }
        else if (name == L"--staging" && hasValue)
        {
            options.staging = arguments[++index];
        }
        else if (name == L"--pid" && hasValue)
        {
            options.pid = static_cast<DWORD>(std::wcstoul(arguments[++index], nullptr, 10));
        }
    }
    LocalFree(arguments);
    return !options.game.empty() && !options.staging.empty() ? std::optional(options) : std::nullopt;
}

std::wstring Text(const wchar_t* russian, const wchar_t* english)
{
    return Update::IsRussianUi() ? russian : english;
}

std::filesystem::path AutoUpdateDirectory(const Options& options)
{
    return options.game / L"Mods" / L"AutoUpdate";
}

std::filesystem::path LogPath(const Options& options)
{
    return AutoUpdateDirectory(options) / L"update.log";
}

void WaitForGameExit(DWORD pid)
{
    if (const auto process = pid ? OpenProcess(SYNCHRONIZE, FALSE, pid) : nullptr)
    {
        WaitForSingleObject(process, 60000);
        CloseHandle(process);
    }
}

// An antivirus scan can hold a freshly written file for a moment, so file operations are retried for ~10 s.
template <typename Operation>
bool Retry(Operation operation)
{
    for (auto attempt = 0; attempt < 20; ++attempt)
    {
        if (operation())
        {
            return true;
        }
        const auto error = GetLastError();
        if (error != ERROR_SHARING_VIOLATION && error != ERROR_LOCK_VIOLATION && error != ERROR_ACCESS_DENIED)
        {
            return false;
        }
        Sleep(500);
    }
    return false;
}

// Copies next to the target first, so the replacement itself is a rename on the same volume. A running
// executable (this applier updating itself) cannot be replaced but can be renamed out of the way.
bool InstallFile(const std::filesystem::path& source, const std::filesystem::path& target)
{
    std::error_code error;
    std::filesystem::create_directories(target.parent_path(), error);
    auto temporary = target;
    temporary += L".update";
    SetFileAttributesW(target.c_str(), FILE_ATTRIBUTE_NORMAL);
    const auto replace = [&]
    {
        return MoveFileExW(temporary.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
    };
    auto installed = Retry([&] { return CopyFileW(source.c_str(), temporary.c_str(), FALSE) != 0; }) && Retry(replace);
    if (!installed && std::filesystem::exists(temporary, error))
    {
        auto old = target;
        old += L".old";
        installed = MoveFileExW(target.c_str(), old.c_str(), MOVEFILE_REPLACE_EXISTING) && replace();
    }
    if (!installed)
    {
        DeleteFileW(temporary.c_str());
    }
    return installed;
}

bool CanWrite(const std::filesystem::path& directory)
{
    const auto probe = directory / L".storm-update-probe";
    const auto file = CreateFileW(probe.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
        FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (file == INVALID_HANDLE_VALUE)
    {
        return GetLastError() != ERROR_ACCESS_DENIED;
    }
    CloseHandle(file);
    return true;
}

std::optional<std::string> HashFile(const std::filesystem::path& file, std::uint64_t expectedSize)
{
    std::uint64_t size{};
    auto hash = Update::Sha256File(file, size);
    return hash && size == expectedSize ? hash : std::nullopt;
}

struct Change
{
    std::filesystem::path target;
    std::optional<std::filesystem::path> backup;
};

class Installation
{
public:
    explicit Installation(const Options& options)
        : options_(options),
          work_(AutoUpdateDirectory(options) / L".update"),
          incoming_(work_ / L"new"),
          backupRoot_(work_ / (L"backup-" + std::to_wstring(GetTickCount64())))
    {
    }

    Result Run()
    {
        try
        {
            return Apply();
        }
        catch (const std::exception& exception)
        {
            return Fail(L"Unexpected error: " + Update::ToWide(exception.what()));
        }
    }

private:
    // The download sits in a user-writable folder, so it is copied into the game folder first and every check and
    // installation works from that copy. An elevated applier never reads user-writable files after this step.
    Result Apply()
    {
        std::error_code error;
        std::filesystem::remove_all(incoming_, error);
        for (const auto& old : std::filesystem::directory_iterator(AutoUpdateDirectory(options_), error))
        {
            if (old.path().extension() == L".old")
            {
                DeleteFileW(old.path().c_str());
            }
        }

        const auto stagedManifest = Update::ToNativePath(options_.staging, Update::ManifestArchivePath);
        const auto manifestCopy = Update::ToNativePath(incoming_, Update::ManifestArchivePath);
        std::filesystem::create_directories(manifestCopy.parent_path());
        if (!CopyFileW(stagedManifest.c_str(), manifestCopy.c_str(), FALSE))
        {
            return Fail(L"The staged update has no manifest");
        }
        const auto incoming = Update::LoadManifest(manifestCopy);
        if (!incoming)
        {
            return Fail(L"The staged manifest is not valid");
        }
        for (const auto& file : incoming->files)
        {
            const auto staged = Update::ToNativePath(options_.staging, file.path);
            const auto copy = Update::ToNativePath(incoming_, file.path);
            std::filesystem::create_directories(copy.parent_path());
            if (std::filesystem::exists(staged, error) && !CopyFileW(staged.c_str(), copy.c_str(), FALSE))
            {
                return Fail(L"Could not read the staged " + Update::ToWide(file.path));
            }
        }

        // Every file of the new release must end up present and verified: either downloaded or already installed.
        std::vector<const Update::ManifestFile*> toInstall;
        for (const auto& file : incoming->files)
        {
            const auto copy = Update::ToNativePath(incoming_, file.path);
            const auto target = Update::ToNativePath(options_.game, file.path);
            if (std::filesystem::exists(copy, error))
            {
                if (HashFile(copy, file.size) != file.sha256)
                {
                    return Fail(L"The downloaded " + Update::ToWide(file.path) + L" is damaged");
                }
                if (!file.config || !std::filesystem::exists(target, error))
                {
                    toInstall.push_back(&file);
                }
            }
            else if (!file.config && HashFile(target, file.size) != file.sha256)
            {
                return Fail(L"The update is incomplete: " + Update::ToWide(file.path) + L" is missing");
            }
        }

        for (const auto* file : toInstall)
        {
            if (!Replace(file->path, Update::ToNativePath(incoming_, file->path)))
            {
                return Fail(L"Could not install " + Update::ToWide(file->path));
            }
        }

        // Files the previous release installed and this one no longer ships are removed; settings stay.
        if (const auto installed = Update::LoadManifest(options_.game / Update::ManifestPath))
        {
            for (const auto& file : installed->files)
            {
                if (!file.config && !incoming->Find(file.path) && !Remove(file.path))
                {
                    return Fail(L"Could not remove " + Update::ToWide(file.path));
                }
            }
        }

        if (!Replace(std::string(Update::ManifestArchivePath), manifestCopy))
        {
            return Fail(L"Could not install the update manifest");
        }
        std::filesystem::remove_all(work_, error);
        Update::AppendLog(LogPath(options_), L"Updated to " + Update::ToWide(incoming->version) + L": " +
            std::to_wstring(changes_.size()) + L" file(s) changed");
        return Result::Updated;
    }

    std::optional<std::filesystem::path> Backup(const std::string& path, const std::filesystem::path& target)
    {
        const auto backup = Update::ToNativePath(backupRoot_, path);
        std::filesystem::create_directories(backup.parent_path());
        return CopyFileW(target.c_str(), backup.c_str(), FALSE) ? std::optional(backup) : std::nullopt;
    }

    bool Replace(const std::string& path, const std::filesystem::path& source)
    {
        const auto target = Update::ToNativePath(options_.game, path);
        std::optional<std::filesystem::path> backup;
        if (std::filesystem::exists(target))
        {
            backup = Backup(path, target);
            if (!backup)
            {
                return false;
            }
        }
        changes_.push_back({target, std::move(backup)});
        return InstallFile(source, target);
    }

    bool Remove(const std::string& path)
    {
        const auto target = Update::ToNativePath(options_.game, path);
        if (!std::filesystem::exists(target))
        {
            return true;
        }
        auto backup = Backup(path, target);
        if (!backup)
        {
            return false;
        }
        changes_.push_back({target, std::move(backup)});
        SetFileAttributesW(target.c_str(), FILE_ATTRIBUTE_NORMAL);
        return Retry([&] { return DeleteFileW(target.c_str()) != 0; });
    }

    // Puts every touched file back the way it was. The backups are kept when that does not fully succeed.
    Result Fail(const std::wstring& reason)
    {
        Update::AppendLog(LogPath(options_), L"FAILED: " + reason + L"; restoring " +
            std::to_wstring(changes_.size()) + L" file(s)");
        auto restored = true;
        for (const auto& change : changes_ | std::views::reverse)
        {
            const auto done = change.backup
                ? InstallFile(*change.backup, change.target)
                : Retry([&]
                  {
                      return DeleteFileW(change.target.c_str()) != 0 || GetLastError() == ERROR_FILE_NOT_FOUND;
                  });
            if (!done)
            {
                restored = false;
                Update::AppendLog(LogPath(options_), L"Could not restore " + change.target.wstring());
            }
        }
        std::error_code error;
        std::filesystem::remove_all(incoming_, error);
        if (restored)
        {
            std::filesystem::remove_all(backupRoot_, error);
        }
        return restored ? Result::RolledBack : Result::Damaged;
    }

    const Options& options_;
    std::filesystem::path work_;
    std::filesystem::path incoming_;
    std::filesystem::path backupRoot_;
    std::vector<Change> changes_;
};

// A game installed under Program Files needs administrator rights. The elevated run uses the installed applier
// from the game folder, which only administrators can change, never the copy in the user's profile.
std::optional<Result> RunElevated(const Options& options)
{
    const auto applier = AutoUpdateDirectory(options) / L"StormUpdater.exe";
    const auto parameters = L"--elevated --game \"" + options.game.wstring() + L"\" --staging \"" +
        options.staging.wstring() + L"\"";
    SHELLEXECUTEINFOW execute{sizeof(execute)};
    execute.fMask = SEE_MASK_NOCLOSEPROCESS;
    execute.lpVerb = L"runas";
    execute.lpFile = applier.c_str();
    execute.lpParameters = parameters.c_str();
    execute.nShow = SW_HIDE;
    if (!ShellExecuteExW(&execute) || !execute.hProcess)
    {
        return std::nullopt;
    }
    WaitForSingleObject(execute.hProcess, INFINITE);
    DWORD exitCode = static_cast<DWORD>(Result::Damaged);
    GetExitCodeProcess(execute.hProcess, &exitCode);
    CloseHandle(execute.hProcess);
    return static_cast<Result>(exitCode);
}

void StartGame(const std::filesystem::path& game)
{
    const auto executable = game / L"Storm.exe";
    std::wstring commandLine = L"\"" + executable.wstring() + L"\"";
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION process{};
    if (CreateProcessW(executable.c_str(), commandLine.data(), nullptr, nullptr, FALSE, 0, nullptr, game.c_str(),
            &startup, &process))
    {
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
    }
}

void ReportFailure(const Options& options, std::optional<Result> result)
{
    std::wstring message;
    if (!result)
    {
        message = Text(L"Для установки обновления требуются права администратора. Обновление отменено.",
            L"Administrator rights are required to install the update. The update has been cancelled.");
    }
    else if (*result == Result::RolledBack)
    {
        message = Text(L"Не удалось установить обновление. Установленная версия восстановлена.",
            L"The update could not be installed. The installed version has been restored.");
    }
    else
    {
        message = Text(L"Не удалось установить обновление и полностью восстановить прежнюю версию. "
                       L"Восстановление будет предложено при следующем запуске игры.",
            L"The update could not be installed and the previous version could not be fully restored. "
            L"A repair will be offered the next time the game starts.");
    }
    message += Text(L"\n\nЖурнал: ", L"\n\nLog: ") + LogPath(options).wstring();
    MessageBoxW(nullptr, message.c_str(), L"Storm in a Teacup Revival", MB_OK | MB_ICONWARNING | MB_SETFOREGROUND);
}
}


int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    const auto options = ParseOptions();
    if (!options)
    {
        return static_cast<int>(Result::RolledBack);
    }
    if (options->elevated)
    {
        return static_cast<int>(Installation(*options).Run());
    }

    const auto mutex = CreateMutexW(nullptr, TRUE, Update::UpdateMutexName.data());
    WaitForGameExit(options->pid);
    const auto result = CanWrite(AutoUpdateDirectory(*options))
        ? std::optional(Installation(*options).Run())
        : RunElevated(*options);
    if (result != Result::Updated)
    {
        ReportFailure(*options, result);
    }
    if (mutex)
    {
        ReleaseMutex(mutex);
        CloseHandle(mutex);
    }
    StartGame(options->game);
    return static_cast<int>(result.value_or(Result::RolledBack));
}
