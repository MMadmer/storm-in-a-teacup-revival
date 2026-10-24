#pragma once

#include <Windows.h>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>


// Shared by the AutoUpdate mod and StormUpdater.exe.
namespace Update
{
// Installed and published next to the mod; every release archive carries its own copy at this path.
inline constexpr std::wstring_view ManifestPath = L"Mods\\AutoUpdate\\update-manifest.txt";
inline constexpr std::string_view ManifestArchivePath = "Mods/AutoUpdate/update-manifest.txt";
inline constexpr std::string_view ManifestHeader = "storm-revival-manifest 1";

struct ManifestFile
{
    std::string path;
    std::uint64_t size{};
    std::string sha256;
    // A user-editable file: installed when missing, never overwritten or removed.
    bool config{};
};

struct Manifest
{
    std::string version;
    std::vector<ManifestFile> files;

    [[nodiscard]] const ManifestFile* Find(std::string_view path) const;
};

std::optional<Manifest> ParseManifest(std::string_view text);
std::optional<Manifest> LoadManifest(const std::filesystem::path& file);

// Rejects absolute paths, drive letters and parent traversal.
bool IsSafeRelativePath(std::string_view path);
std::filesystem::path ToNativePath(const std::filesystem::path& root, std::string_view archivePath);

std::string Sha256(std::string_view data);
std::optional<std::string> Sha256File(const std::filesystem::path& file, std::uint64_t& size);

// Compares dotted numeric versions: "1.10.0" > "1.9.2".
int CompareVersions(std::string_view left, std::string_view right);

std::wstring ToWide(std::string_view text);
std::string ToUtf8(std::wstring_view text);

bool IsRussianUi();

// Held by StormUpdater.exe while it waits for the game and installs; the mod leaves the update alone meanwhile.
inline constexpr std::wstring_view UpdateMutexName = L"Local\\StormInATeacupRevivalUpdate";

// %LOCALAPPDATA%\StormInATeacupRevival: the downloaded update and the applier copy.
std::filesystem::path DataDirectory();
void AppendLog(const std::filesystem::path& log, const std::wstring& message);
}
