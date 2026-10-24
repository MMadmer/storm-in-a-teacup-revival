#include "UpdateCommon.h"

#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cstring>
#include <cstdio>
#include <fstream>
#include <ranges>
#include <sstream>


namespace Update
{
const ManifestFile* Manifest::Find(std::string_view path) const
{
    const auto found = std::ranges::find_if(files, [path](const ManifestFile& file)
    {
        return _stricmp(file.path.c_str(), std::string(path).c_str()) == 0;
    });
    return found != files.end() ? &*found : nullptr;
}

// Format: a header line, "version X.Y.Z", then "file <sha256> <size> <-|config> <path>" per file.
std::optional<Manifest> ParseManifest(std::string_view text)
{
    Manifest manifest;
    auto headerSeen = false;
    for (auto line : text | std::views::split('\n'))
    {
        std::string_view row(line.begin(), line.end());
        if (!row.empty() && row.back() == '\r')
        {
            row.remove_suffix(1);
        }
        if (row.empty())
        {
            continue;
        }
        if (!headerSeen)
        {
            if (row != ManifestHeader)
            {
                return std::nullopt;
            }
            headerSeen = true;
            continue;
        }
        if (row.starts_with("version "))
        {
            manifest.version = row.substr(8);
            continue;
        }
        if (!row.starts_with("file "))
        {
            return std::nullopt;
        }

        std::array<std::string_view, 3> fields{};
        row.remove_prefix(5);
        for (auto& field : fields)
        {
            const auto space = row.find(' ');
            if (space == std::string_view::npos)
            {
                return std::nullopt;
            }
            field = row.substr(0, space);
            row.remove_prefix(space + 1);
        }
        ManifestFile file{std::string(row), 0, std::string(fields[0]), fields[2] == "config"};
        const auto [end, error] = std::from_chars(fields[1].data(), fields[1].data() + fields[1].size(), file.size);
        if (error != std::errc{} || end != fields[1].data() + fields[1].size() || file.sha256.size() != 64 ||
            !IsSafeRelativePath(file.path) || manifest.Find(file.path))
        {
            return std::nullopt;
        }
        manifest.files.push_back(std::move(file));
    }
    if (!headerSeen || manifest.version.empty())
    {
        return std::nullopt;
    }
    return manifest;
}

std::optional<Manifest> LoadManifest(const std::filesystem::path& file)
{
    std::ifstream stream(file, std::ios::binary);
    if (!stream)
    {
        return std::nullopt;
    }
    std::ostringstream text;
    text << stream.rdbuf();
    return ParseManifest(text.str());
}

// Windows treats "x.dll." as "x.dll" and reserves device names, so such names could alias another entry.
bool IsSafeRelativePath(std::string_view path)
{
    if (path.empty() || path.front() == '/')
    {
        return false;
    }
    constexpr std::array<std::string_view, 4> devices{"CON", "PRN", "AUX", "NUL"};
    constexpr std::array<std::string_view, 2> numberedDevices{"COM", "LPT"};
    for (auto part : path | std::views::split('/'))
    {
        const std::string_view name(part.begin(), part.end());
        if (name.empty() || name.back() == '.' || name.back() == ' ' ||
            std::ranges::any_of(name, [](char character)
            {
                return static_cast<unsigned char>(character) < 0x20 || std::strchr("\\:*?\"<>|", character);
            }))
        {
            return false;
        }
        const auto stem = name.substr(0, name.find('.'));
        const auto isDevice = [stem](std::string_view device)
        {
            return _strnicmp(stem.data(), device.data(), device.size()) == 0;
        };
        if ((stem.size() == 3 && std::ranges::any_of(devices, isDevice)) ||
            (stem.size() == 4 && std::isdigit(static_cast<unsigned char>(stem[3])) &&
                std::ranges::any_of(numberedDevices, isDevice)))
        {
            return false;
        }
    }
    return true;
}

std::filesystem::path ToNativePath(const std::filesystem::path& root, std::string_view archivePath)
{
    auto result = root;
    for (auto part : archivePath | std::views::split('/'))
    {
        result /= ToWide(std::string_view(part.begin(), part.end()));
    }
    return result;
}

namespace
{
class Sha256Hasher
{
public:
    Sha256Hasher()
    {
        BCryptOpenAlgorithmProvider(&algorithm_, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
        if (algorithm_)
        {
            BCryptCreateHash(algorithm_, &hash_, nullptr, 0, nullptr, 0, 0);
        }
    }
    ~Sha256Hasher()
    {
        if (hash_)
        {
            BCryptDestroyHash(hash_);
        }
        if (algorithm_)
        {
            BCryptCloseAlgorithmProvider(algorithm_, 0);
        }
    }
    Sha256Hasher(const Sha256Hasher&) = delete;
    Sha256Hasher& operator=(const Sha256Hasher&) = delete;

    void Add(const void* data, std::size_t size)
    {
        if (hash_)
        {
            BCryptHashData(hash_, static_cast<PUCHAR>(const_cast<void*>(data)), static_cast<ULONG>(size), 0);
        }
    }

    std::string Finish()
    {
        std::array<UCHAR, 32> digest{};
        if (!hash_ || BCryptFinishHash(hash_, digest.data(), static_cast<ULONG>(digest.size()), 0) < 0)
        {
            return {};
        }
        std::string text;
        for (const auto byte : digest)
        {
            std::array<char, 3> pair{};
            std::snprintf(pair.data(), pair.size(), "%02x", byte);
            text += pair.data();
        }
        return text;
    }

private:
    BCRYPT_ALG_HANDLE algorithm_{};
    BCRYPT_HASH_HANDLE hash_{};
};
}

std::string Sha256(std::string_view data)
{
    Sha256Hasher hasher;
    hasher.Add(data.data(), data.size());
    return hasher.Finish();
}

std::optional<std::string> Sha256File(const std::filesystem::path& file, std::uint64_t& size)
{
    std::ifstream stream(file, std::ios::binary);
    if (!stream)
    {
        return std::nullopt;
    }
    Sha256Hasher hasher;
    std::vector<char> buffer(1 << 16);
    size = 0;
    while (stream)
    {
        stream.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto count = static_cast<std::size_t>(stream.gcount());
        hasher.Add(buffer.data(), count);
        size += count;
    }
    return hasher.Finish();
}

int CompareVersions(std::string_view left, std::string_view right)
{
    const auto numbers = [](std::string_view text)
    {
        std::vector<std::uint32_t> result;
        for (auto part : text | std::views::split('.'))
        {
            std::uint32_t value{};
            std::from_chars(part.data(), part.data() + part.size(), value);
            result.push_back(value);
        }
        return result;
    };
    auto a = numbers(left);
    auto b = numbers(right);
    const auto count = (std::max)(a.size(), b.size());
    a.resize(count);
    b.resize(count);
    return a < b ? -1 : a > b ? 1 : 0;
}

std::wstring ToWide(std::string_view text)
{
    const auto length = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring result(static_cast<std::size_t>((std::max)(length, 0)), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), length);
    return result;
}

std::string ToUtf8(std::wstring_view text)
{
    const auto length = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0,
        nullptr, nullptr);
    std::string result(static_cast<std::size_t>((std::max)(length, 0)), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), length, nullptr,
        nullptr);
    return result;
}

bool IsRussianUi()
{
    return PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_RUSSIAN;
}

std::filesystem::path DataDirectory()
{
    std::array<wchar_t, MAX_PATH> base{};
    const auto length = GetEnvironmentVariableW(L"LOCALAPPDATA", base.data(), static_cast<DWORD>(base.size()));
    const std::filesystem::path root = length && length < base.size()
        ? std::filesystem::path(base.data())
        : std::filesystem::temp_directory_path();
    return root / L"StormInATeacupRevival";
}

void AppendLog(const std::filesystem::path& log, const std::wstring& message)
{
    std::error_code error;
    std::filesystem::create_directories(log.parent_path(), error);
    SYSTEMTIME time{};
    GetLocalTime(&time);
    std::array<char, 32> stamp{};
    std::snprintf(stamp.data(), stamp.size(), "%04u-%02u-%02u %02u:%02u:%02u ", time.wYear, time.wMonth, time.wDay,
        time.wHour, time.wMinute, time.wSecond);
    std::ofstream stream(log, std::ios::binary | std::ios::app);
    stream << stamp.data() << ToUtf8(message) << "\r\n";
}
}
