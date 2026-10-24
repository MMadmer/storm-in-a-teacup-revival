#include "StormModApi.h"
#include "UpdateCommon.h"

#include <d3d9.h>
#include <winhttp.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <variant>
#include <vector>


namespace
{
using CreateDeviceFn = HRESULT(WINAPI*)(IDirect3D9*, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS*,
    IDirect3DDevice9**);

constexpr std::wstring_view LatestReleaseUrl =
    L"https://api.github.com/repos/MMadmer/storm-in-a-teacup-revival/releases/latest";
constexpr std::string_view AssetPrefix = "storm-in-a-teacup-revival-";
constexpr std::wstring_view UserAgent = L"StormInATeacupRevival-Updater";
constexpr std::uint64_t MaxDownload = 32ull << 20;

const StormModHostApi* g_host{};
std::filesystem::path g_gameDirectory;
CreateDeviceFn g_originalCreateDevice{};
HANDLE g_checkDone{};
std::atomic_bool g_offerShown{};

void Log(const std::wstring& message)
{
    if (g_host && g_host->log)
    {
        g_host->log(L"AutoUpdate", message.c_str());
    }
}

std::wstring Text(const wchar_t* russian, const wchar_t* english)
{
    return Update::IsRussianUi() ? russian : english;
}

// --- HTTP ---------------------------------------------------------------------------------------------------------

struct InternetHandle
{
    HINTERNET handle{};
    explicit InternetHandle(HINTERNET value = nullptr) : handle(value) {}
    ~InternetHandle()
    {
        if (handle)
        {
            WinHttpCloseHandle(handle);
        }
    }
    InternetHandle(const InternetHandle&) = delete;
    InternetHandle& operator=(const InternetHandle&) = delete;
    explicit operator bool() const { return handle != nullptr; }
};

struct HttpResponse
{
    DWORD status{};
    std::wstring contentRange;
    std::string body;
};

class HttpClient
{
public:
    HttpClient()
        : session_(WinHttpOpen(UserAgent.data(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
              WINHTTP_NO_PROXY_BYPASS, 0))
    {
        if (!session_)
        {
            session_.handle = WinHttpOpen(UserAgent.data(), WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        }
        if (session_)
        {
            WinHttpSetTimeouts(session_.handle, 5000, 5000, 10000, 20000);
        }
    }

    // A range of "first-last" or "-suffix" bytes; an empty range requests the whole resource.
    std::optional<HttpResponse> Get(const std::wstring& url, const std::wstring& range = {},
        const wchar_t* accept = nullptr) const
    {
        if (!session_)
        {
            return std::nullopt;
        }
        URL_COMPONENTS parts{sizeof(parts)};
        std::array<wchar_t, 256> host{};
        std::wstring path(url.size() + 1, L'\0');
        parts.lpszHostName = host.data();
        parts.dwHostNameLength = static_cast<DWORD>(host.size());
        parts.lpszUrlPath = path.data();
        parts.dwUrlPathLength = static_cast<DWORD>(path.size());
        if (!WinHttpCrackUrl(url.c_str(), 0, 0, &parts))
        {
            return std::nullopt;
        }
        const auto secure = parts.nScheme == INTERNET_SCHEME_HTTPS;
        InternetHandle connection(WinHttpConnect(session_.handle, host.data(), parts.nPort, 0));
        InternetHandle request(connection
            ? WinHttpOpenRequest(connection.handle, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
                  WINHTTP_DEFAULT_ACCEPT_TYPES, secure ? WINHTTP_FLAG_SECURE : 0)
            : nullptr);
        if (!request)
        {
            return std::nullopt;
        }

        std::wstring headers;
        if (accept)
        {
            headers += L"Accept: " + std::wstring(accept) + L"\r\n";
        }
        if (!range.empty())
        {
            headers += L"Range: bytes=" + range + L"\r\n";
        }
        if (!WinHttpSendRequest(request.handle, headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(),
                headers.empty() ? 0 : static_cast<DWORD>(-1L), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
            !WinHttpReceiveResponse(request.handle, nullptr))
        {
            return std::nullopt;
        }

        HttpResponse response;
        DWORD size = sizeof(response.status);
        WinHttpQueryHeaders(request.handle, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &response.status, &size, WINHTTP_NO_HEADER_INDEX);
        std::array<wchar_t, 128> contentRange{};
        size = static_cast<DWORD>(contentRange.size() * sizeof(wchar_t));
        if (WinHttpQueryHeaders(request.handle, WINHTTP_QUERY_CONTENT_RANGE, WINHTTP_HEADER_NAME_BY_INDEX,
                contentRange.data(), &size, WINHTTP_NO_HEADER_INDEX))
        {
            response.contentRange = contentRange.data();
        }
        for (DWORD available = 0; WinHttpQueryDataAvailable(request.handle, &available) && available;)
        {
            const auto offset = response.body.size();
            if (offset + available > MaxDownload)
            {
                return std::nullopt;
            }
            response.body.resize(offset + available);
            DWORD read = 0;
            if (!WinHttpReadData(request.handle, response.body.data() + offset, available, &read))
            {
                return std::nullopt;
            }
            response.body.resize(offset + read);
        }
        return response;
    }

private:
    InternetHandle session_;
};

// --- JSON (just enough for the GitHub release API) --------------------------------------------------------------

struct JsonValue;
using JsonObject = std::map<std::string, JsonValue, std::less<>>;
using JsonArray = std::vector<JsonValue>;

struct JsonValue
{
    std::variant<std::nullptr_t, bool, double, std::string, std::shared_ptr<JsonArray>, std::shared_ptr<JsonObject>>
        value;

    [[nodiscard]] const JsonValue* Get(std::string_view key) const
    {
        const auto* object = std::get_if<std::shared_ptr<JsonObject>>(&value);
        if (!object)
        {
            return nullptr;
        }
        const auto found = (*object)->find(key);
        return found != (*object)->end() ? &found->second : nullptr;
    }
    [[nodiscard]] std::string String(std::string_view key) const
    {
        const auto* member = Get(key);
        const auto* text = member ? std::get_if<std::string>(&member->value) : nullptr;
        return text ? *text : std::string{};
    }
    [[nodiscard]] double Number(std::string_view key) const
    {
        const auto* member = Get(key);
        const auto* number = member ? std::get_if<double>(&member->value) : nullptr;
        return number ? *number : 0.0;
    }
    [[nodiscard]] const JsonArray* Array(std::string_view key) const
    {
        const auto* member = Get(key);
        const auto* array = member ? std::get_if<std::shared_ptr<JsonArray>>(&member->value) : nullptr;
        return array ? array->get() : nullptr;
    }
};

class JsonParser
{
public:
    explicit JsonParser(std::string_view text) : text_(text) {}

    std::optional<JsonValue> Parse()
    {
        auto value = ParseValue(0);
        SkipSpace();
        return value && position_ == text_.size() ? value : std::nullopt;
    }

private:
    static bool IsSpace(char character)
    {
        return character == ' ' || character == '\t' || character == '\r' || character == '\n';
    }

    void SkipSpace()
    {
        while (position_ < text_.size() && IsSpace(text_[position_]))
        {
            ++position_;
        }
    }

    bool Consume(char expected)
    {
        SkipSpace();
        if (position_ < text_.size() && text_[position_] == expected)
        {
            ++position_;
            return true;
        }
        return false;
    }

    std::optional<JsonValue> ParseValue(int depth)
    {
        SkipSpace();
        if (depth > 32 || position_ >= text_.size())
        {
            return std::nullopt;
        }
        switch (text_[position_])
        {
        case '{':
            return ParseObject(depth);
        case '[':
            return ParseArray(depth);
        case '"':
            if (auto text = ParseString())
            {
                return JsonValue{std::move(*text)};
            }
            return std::nullopt;
        default:
            return ParseLiteral();
        }
    }

    std::optional<JsonValue> ParseObject(int depth)
    {
        ++position_;
        auto object = std::make_shared<JsonObject>();
        if (Consume('}'))
        {
            return JsonValue{object};
        }
        do
        {
            SkipSpace();
            auto key = ParseString();
            if (!key || !Consume(':'))
            {
                return std::nullopt;
            }
            auto member = ParseValue(depth + 1);
            if (!member)
            {
                return std::nullopt;
            }
            object->insert_or_assign(std::move(*key), std::move(*member));
        }
        while (Consume(','));
        return Consume('}') ? std::optional<JsonValue>(JsonValue{object}) : std::nullopt;
    }

    std::optional<JsonValue> ParseArray(int depth)
    {
        ++position_;
        auto array = std::make_shared<JsonArray>();
        if (Consume(']'))
        {
            return JsonValue{array};
        }
        do
        {
            auto element = ParseValue(depth + 1);
            if (!element)
            {
                return std::nullopt;
            }
            array->push_back(std::move(*element));
        }
        while (Consume(','));
        return Consume(']') ? std::optional<JsonValue>(JsonValue{array}) : std::nullopt;
    }

    static void AppendUtf8(std::string& out, std::uint32_t code)
    {
        if (code < 0x80)
        {
            out += static_cast<char>(code);
        }
        else if (code < 0x800)
        {
            out += static_cast<char>(0xC0 | code >> 6);
            out += static_cast<char>(0x80 | (code & 0x3F));
        }
        else if (code < 0x10000)
        {
            out += static_cast<char>(0xE0 | code >> 12);
            out += static_cast<char>(0x80 | (code >> 6 & 0x3F));
            out += static_cast<char>(0x80 | (code & 0x3F));
        }
        else
        {
            out += static_cast<char>(0xF0 | code >> 18);
            out += static_cast<char>(0x80 | (code >> 12 & 0x3F));
            out += static_cast<char>(0x80 | (code >> 6 & 0x3F));
            out += static_cast<char>(0x80 | (code & 0x3F));
        }
    }

    std::optional<std::uint32_t> ParseHex4()
    {
        if (position_ + 4 > text_.size())
        {
            return std::nullopt;
        }
        std::uint32_t code{};
        for (auto index = 0; index < 4; ++index)
        {
            const auto digit = text_[position_++];
            code <<= 4;
            if (digit >= '0' && digit <= '9')
            {
                code |= digit - '0';
            }
            else if (digit >= 'a' && digit <= 'f')
            {
                code |= digit - 'a' + 10;
            }
            else if (digit >= 'A' && digit <= 'F')
            {
                code |= digit - 'A' + 10;
            }
            else
            {
                return std::nullopt;
            }
        }
        return code;
    }

    std::optional<std::string> ParseString()
    {
        if (position_ >= text_.size() || text_[position_] != '"')
        {
            return std::nullopt;
        }
        ++position_;
        std::string result;
        while (position_ < text_.size())
        {
            const auto character = text_[position_++];
            if (character == '"')
            {
                return result;
            }
            if (character != '\\')
            {
                result += character;
                continue;
            }
            if (position_ >= text_.size())
            {
                return std::nullopt;
            }
            switch (const auto escape = text_[position_++])
            {
            case 'n':
                result += '\n';
                break;
            case 'r':
                result += '\r';
                break;
            case 't':
                result += '\t';
                break;
            case 'b':
                result += '\b';
                break;
            case 'f':
                result += '\f';
                break;
            case 'u':
            {
                auto code = ParseHex4();
                if (!code)
                {
                    return std::nullopt;
                }
                // A high surrogate is followed by "\uDC00".."\uDFFF".
                if (*code >= 0xD800 && *code < 0xDC00 && position_ + 1 < text_.size() && text_[position_] == '\\' &&
                    text_[position_ + 1] == 'u')
                {
                    position_ += 2;
                    const auto low = ParseHex4();
                    if (!low || *low < 0xDC00 || *low > 0xDFFF)
                    {
                        return std::nullopt;
                    }
                    *code = 0x10000 + ((*code - 0xD800) << 10) + (*low - 0xDC00);
                }
                AppendUtf8(result, *code);
                break;
            }
            default:
                result += escape;
                break;
            }
        }
        return std::nullopt;
    }

    std::optional<JsonValue> ParseLiteral()
    {
        const auto start = position_;
        while (position_ < text_.size() && !IsSpace(text_[position_]) && text_[position_] != ',' &&
            text_[position_] != ']' && text_[position_] != '}')
        {
            ++position_;
        }
        const auto token = text_.substr(start, position_ - start);
        if (token == "true" || token == "false")
        {
            return JsonValue{token == "true"};
        }
        if (token == "null")
        {
            return JsonValue{nullptr};
        }
        const std::string number(token);
        char* end{};
        const auto value = std::strtod(number.c_str(), &end);
        return !number.empty() && end == number.c_str() + number.size()
            ? std::optional<JsonValue>(JsonValue{value})
            : std::nullopt;
    }

    std::string_view text_;
    std::size_t position_{};
};

// --- Release archive read through HTTP ranges --------------------------------------------------------------------

std::uint32_t Crc32(std::string_view data)
{
    static const auto table = []
    {
        std::array<std::uint32_t, 256> values{};
        for (std::uint32_t index = 0; index < values.size(); ++index)
        {
            auto value = index;
            for (auto bit = 0; bit < 8; ++bit)
            {
                value = value & 1 ? 0xEDB88320u ^ (value >> 1) : value >> 1;
            }
            values[index] = value;
        }
        return values;
    }();
    auto crc = 0xFFFFFFFFu;
    for (const auto byte : data)
    {
        crc = table[(crc ^ static_cast<std::uint8_t>(byte)) & 0xFF] ^ (crc >> 8);
    }
    return ~crc;
}

template <typename Integer>
Integer ReadLittle(std::string_view data, std::size_t offset)
{
    Integer value{};
    std::memcpy(&value, data.data() + offset, sizeof(value));
    return value;
}

struct ArchiveEntry
{
    std::string name;
    std::uint32_t crc{};
    std::uint64_t size{};
    std::uint64_t recordOffset{};
    // The next record or the central directory: the whole local record lies before it.
    std::uint64_t recordEnd{};
};

// Release archives are stored without compression, so a file's bytes are a plain slice of its local record.
class RemoteArchive
{
public:
    RemoteArchive(std::wstring url, std::uint64_t size, std::string sha256)
        : url_(std::move(url)), size_(size), sha256_(std::move(sha256))
    {
    }

    // True once a server that ignores ranges has sent the whole archive.
    [[nodiscard]] bool IsWhole() const { return !whole_.empty(); }
    [[nodiscard]] std::uint64_t Size() const { return size_; }

    bool ReadDirectory()
    {
        const auto tailSize = (std::min<std::uint64_t>)(size_, 65557);
        std::string tail;
        if (!Read(size_ - tailSize, tailSize, tail))
        {
            return false;
        }
        const auto record = tail.rfind(std::string_view("PK\x05\x06", 4));
        if (record == std::string::npos || record + 22 > tail.size())
        {
            return false;
        }
        const auto count = ReadLittle<std::uint16_t>(tail, record + 10);
        const auto directorySize = ReadLittle<std::uint32_t>(tail, record + 12);
        const auto directoryOffset = ReadLittle<std::uint32_t>(tail, record + 16);
        std::string directory;
        if (static_cast<std::uint64_t>(directoryOffset) + directorySize > size_ ||
            !Read(directoryOffset, directorySize, directory))
        {
            return false;
        }

        for (std::size_t offset = 0, index = 0; index < count; ++index)
        {
            if (offset + 46 > directory.size() || ReadLittle<std::uint32_t>(directory, offset) != 0x02014B50)
            {
                return false;
            }
            const auto method = ReadLittle<std::uint16_t>(directory, offset + 10);
            const auto compressed = ReadLittle<std::uint32_t>(directory, offset + 20);
            const auto uncompressed = ReadLittle<std::uint32_t>(directory, offset + 24);
            const auto nameLength = ReadLittle<std::uint16_t>(directory, offset + 28);
            const auto extraLength = ReadLittle<std::uint16_t>(directory, offset + 30);
            const auto commentLength = ReadLittle<std::uint16_t>(directory, offset + 32);
            if (method != 0 || compressed != uncompressed || offset + 46 + nameLength > directory.size())
            {
                return false;
            }
            entries_.push_back({directory.substr(offset + 46, nameLength),
                ReadLittle<std::uint32_t>(directory, offset + 16), uncompressed,
                ReadLittle<std::uint32_t>(directory, offset + 42), 0});
            offset += 46 + nameLength + extraLength + commentLength;
        }

        std::ranges::sort(entries_, {}, &ArchiveEntry::recordOffset);
        for (std::size_t index = 0; index < entries_.size(); ++index)
        {
            entries_[index].recordEnd = index + 1 < entries_.size() ? entries_[index + 1].recordOffset : directoryOffset;
        }
        return true;
    }

    [[nodiscard]] const ArchiveEntry* Find(std::string_view name) const
    {
        const auto found = std::ranges::find(entries_, name, &ArchiveEntry::name);
        return found != entries_.end() ? &*found : nullptr;
    }

    std::optional<std::string> Extract(const ArchiveEntry& entry)
    {
        std::string record;
        if (entry.recordEnd < entry.recordOffset + 30 ||
            !Read(entry.recordOffset, entry.recordEnd - entry.recordOffset, record) ||
            ReadLittle<std::uint32_t>(record, 0) != 0x04034B50)
        {
            return std::nullopt;
        }
        const auto dataOffset = 30ull + ReadLittle<std::uint16_t>(record, 26) + ReadLittle<std::uint16_t>(record, 28);
        if (dataOffset + entry.size > record.size())
        {
            return std::nullopt;
        }
        auto data = record.substr(static_cast<std::size_t>(dataOffset), static_cast<std::size_t>(entry.size));
        return Crc32(data) == entry.crc ? std::optional(std::move(data)) : std::nullopt;
    }

private:
    // A server that ignores ranges sends the whole archive once; it is checked against the release digest and
    // every later read is served from it.
    bool Read(std::uint64_t offset, std::uint64_t length, std::string& out)
    {
        if (!whole_.empty())
        {
            if (offset > whole_.size() || length > whole_.size() - offset)
            {
                return false;
            }
            out = whole_.substr(static_cast<std::size_t>(offset), static_cast<std::size_t>(length));
            return true;
        }
        if (!length)
        {
            out.clear();
            return true;
        }
        const auto range = std::to_wstring(offset) + L"-" + std::to_wstring(offset + length - 1);
        auto response = http_.Get(url_, range);
        if (!response)
        {
            return false;
        }
        if (response->status == 206)
        {
            if (response->body.size() != length || !response->contentRange.starts_with(L"bytes " + range + L"/"))
            {
                return false;
            }
            out = std::move(response->body);
            return true;
        }
        if (response->status != 200 || response->body.size() != size_ || sha256_.empty() ||
            Update::Sha256(response->body) != sha256_)
        {
            return false;
        }
        whole_ = std::move(response->body);
        return Read(offset, length, out);
    }

    HttpClient http_;
    std::wstring url_;
    std::uint64_t size_;
    std::string sha256_;
    std::string whole_;
    std::vector<ArchiveEntry> entries_;
};

// --- Update check -------------------------------------------------------------------------------------------------

struct UpdatePlan
{
    std::string version;
    std::vector<std::string> changes;
    // Its directory is read during the check and reused for the download.
    std::shared_ptr<RemoteArchive> archive;
    Update::Manifest manifest;
    std::vector<Update::ManifestFile> files;
    std::uint64_t downloadSize{};
    // The installed version is the latest one, but some of its files are missing or damaged.
    bool repair{};
};

std::optional<UpdatePlan> g_plan;

// "* item" lines of "## Изменения" inside "## RU", or of "## Changes" inside "## EN".
std::vector<std::string> ParseChanges(std::string_view body)
{
    const auto russian = Update::IsRussianUi();
    const std::string_view language = russian ? "## RU" : "## EN";
    const std::string_view changes = russian ? "## Изменения" : "## Changes";
    std::vector<std::string> result;
    auto inLanguage = false;
    auto inChanges = false;
    for (auto part : body | std::views::split('\n'))
    {
        std::string_view line(part.begin(), part.end());
        while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
        {
            line.remove_suffix(1);
        }
        if (line.starts_with("## "))
        {
            if (line == "## RU" || line == "## EN")
            {
                inLanguage = line == language;
                inChanges = false;
            }
            else
            {
                inChanges = inLanguage && line == changes;
            }
            continue;
        }
        if (inChanges && (line.starts_with("* ") || line.starts_with("- ")))
        {
            std::string item(line.substr(2));
            std::erase(item, '*');
            std::erase(item, '`');
            result.push_back(std::move(item));
        }
    }
    return result;
}

// An interrupted update or a quarantined file leaves the latest version partly installed, which only the release
// can fix. Installations without a manifest (development builds) are left alone.
bool IsInstallationIntact()
{
    const auto installed = Update::LoadManifest(g_gameDirectory / Update::ManifestPath);
    if (!installed)
    {
        return true;
    }
    return installed->version == STORM_REVIVAL_VERSION &&
        std::ranges::all_of(installed->files, [](const Update::ManifestFile& file)
        {
            std::uint64_t size{};
            return file.config ||
                (Update::Sha256File(Update::ToNativePath(g_gameDirectory, file.path), size) == file.sha256 &&
                    size == file.size);
        });
}

std::optional<UpdatePlan> CheckForUpdate()
{
    HttpClient http;
    std::array<wchar_t, 1024> overrideUrl{};
    const auto overrideLength = GetEnvironmentVariableW(L"STORM_UPDATE_URL", overrideUrl.data(),
        static_cast<DWORD>(overrideUrl.size()));
    const std::wstring apiUrl = overrideLength && overrideLength < overrideUrl.size()
        ? std::wstring(overrideUrl.data())
        : std::wstring(LatestReleaseUrl);
    const auto response = http.Get(apiUrl, {}, L"application/vnd.github+json");
    if (!response || response->status != 200)
    {
        return std::nullopt;
    }
    const auto release = JsonParser(response->body).Parse();
    if (!release)
    {
        return std::nullopt;
    }

    UpdatePlan plan;
    plan.version = release->String("tag_name");
    if (plan.version.starts_with('v') || plan.version.starts_with('V'))
    {
        plan.version.erase(0, 1);
    }
    const auto order = plan.version.empty() ? -1 : Update::CompareVersions(plan.version, STORM_REVIVAL_VERSION);
    if (order < 0 || (order == 0 && IsInstallationIntact()))
    {
        return std::nullopt;
    }
    plan.repair = order == 0;
    const auto* assets = release->Array("assets");
    if (!assets)
    {
        return std::nullopt;
    }
    const auto assetName = std::string(AssetPrefix) + plan.version + ".zip";
    const auto asset = std::ranges::find(*assets, assetName, [](const JsonValue& value)
    {
        return value.String("name");
    });
    if (asset == assets->end())
    {
        return std::nullopt;
    }
    const auto digest = asset->String("digest");
    const auto archiveSize = asset->Number("size");
    if (!(archiveSize > 0.0 && archiveSize < static_cast<double>(1ull << 40)))
    {
        return std::nullopt;
    }
    plan.archive = std::make_shared<RemoteArchive>(Update::ToWide(asset->String("browser_download_url")),
        static_cast<std::uint64_t>(archiveSize), digest.starts_with("sha256:") ? digest.substr(7) : "");
    plan.changes = ParseChanges(release->String("body"));

    // Only the files whose installed copy differs from the release are taken from the archive.
    auto& archive = *plan.archive;
    const auto* manifestEntry = archive.ReadDirectory() ? archive.Find(Update::ManifestArchivePath) : nullptr;
    const auto manifestText = manifestEntry ? archive.Extract(*manifestEntry) : std::nullopt;
    auto manifest = manifestText ? Update::ParseManifest(*manifestText) : std::nullopt;
    if (!manifest || manifest->version != plan.version)
    {
        Log(L"The release archive has no valid update manifest");
        return std::nullopt;
    }
    plan.manifest = std::move(*manifest);
    plan.downloadSize = manifestEntry->recordEnd - manifestEntry->recordOffset;
    for (const auto& file : plan.manifest.files)
    {
        const auto target = Update::ToNativePath(g_gameDirectory, file.path);
        std::uint64_t size{};
        const auto installed = Update::Sha256File(target, size);
        if (installed ? file.config || (*installed == file.sha256 && size == file.size) : false)
        {
            continue;
        }
        const auto* entry = archive.Find(file.path);
        if (!entry || entry->size != file.size)
        {
            Log(L"The release archive does not match its manifest");
            return std::nullopt;
        }
        plan.files.push_back(file);
        plan.downloadSize += entry->recordEnd - entry->recordOffset;
    }
    if (plan.repair && plan.files.empty())
    {
        return std::nullopt;
    }
    if (archive.IsWhole())
    {
        plan.downloadSize = archive.Size();
    }
    return plan;
}

DWORD WINAPI CheckThread(void*)
{
    try
    {
        g_plan = CheckForUpdate();
    }
    catch (const std::exception& exception)
    {
        Log(L"Update check failed: " + Update::ToWide(exception.what()));
    }
    if (g_plan)
    {
        Log((g_plan->repair ? L"Repair available: " : L"Update available: ") + Update::ToWide(g_plan->version));
    }
    SetEvent(g_checkDone);
    return 0;
}

// --- Offer and installation ---------------------------------------------------------------------------------------

std::wstring FormatSize(std::uint64_t bytes)
{
    std::array<wchar_t, 64> text{};
    if (bytes < 1024 * 1024)
    {
        _snwprintf_s(text.data(), text.size(), _TRUNCATE, Text(L"%.0f КБ", L"%.0f KB").c_str(),
            (std::max)(1.0, bytes / 1024.0));
    }
    else
    {
        _snwprintf_s(text.data(), text.size(), _TRUNCATE, Text(L"%.1f МБ", L"%.1f MB").c_str(),
            bytes / (1024.0 * 1024.0));
    }
    return text.data();
}

bool AskToInstall(HWND owner, const UpdatePlan& plan)
{
    if (plan.repair)
    {
        const auto message = Text(L"Часть файлов Storm in a Teacup Revival отсутствует или повреждена.\n\n",
            L"Some Storm in a Teacup Revival files are missing or damaged.\n\n") +
            Text(L"Размер загрузки: ", L"Download size: ") + FormatSize(plan.downloadSize) +
            Text(L"\n\nВосстановить файлы сейчас? Игра будет перезапущена.",
                L"\n\nRestore the files now? The game will restart.");
        return MessageBoxW(owner, message.c_str(), L"Storm in a Teacup Revival",
            MB_YESNO | MB_ICONWARNING | MB_SETFOREGROUND | MB_TOPMOST) == IDYES;
    }
    auto message = Text(L"Доступна новая версия Storm in a Teacup Revival.\n\n",
        L"A new version of Storm in a Teacup Revival is available.\n\n");
    message += Text(L"Установлена: ", L"Installed: ") + Update::ToWide(STORM_REVIVAL_VERSION) + L"\n";
    message += Text(L"Доступна: ", L"Available: ") + Update::ToWide(plan.version) + L"\n";
    message += Text(L"Размер загрузки: ", L"Download size: ") + FormatSize(plan.downloadSize) + L"\n";
    if (!plan.changes.empty())
    {
        message += Text(L"\nИзменения:\n", L"\nChanges:\n");
        for (const auto& change : plan.changes | std::views::take(12))
        {
            message += L"• " + Update::ToWide(change) + L"\n";
        }
    }
    message += Text(L"\nУстановить обновление сейчас? Игра будет перезапущена.",
        L"\nInstall the update now? The game will restart.");
    return MessageBoxW(owner, message.c_str(), L"Storm in a Teacup Revival",
        MB_YESNO | MB_ICONINFORMATION | MB_SETFOREGROUND | MB_TOPMOST) == IDYES;
}

bool SaveFile(const std::filesystem::path& path, std::string_view data)
{
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream.write(data.data(), static_cast<std::streamsize>(data.size()));
    return static_cast<bool>(stream);
}

// Downloads the changed files into the staging folder and starts the applier from a copy outside the game.
bool StageAndStartApplier(const UpdatePlan& plan)
{
    const auto updateDirectory = Update::DataDirectory() / L"update";
    const auto staging = updateDirectory / L"new";
    std::error_code error;
    std::filesystem::remove_all(updateDirectory, error);

    auto& archive = *plan.archive;
    for (const auto& file : plan.files)
    {
        const auto* entry = archive.Find(file.path);
        const auto data = entry ? archive.Extract(*entry) : std::nullopt;
        if (!data || data->size() != file.size || Update::Sha256(*data) != file.sha256 ||
            !SaveFile(Update::ToNativePath(staging, file.path), *data))
        {
            Log(L"Download failed: " + Update::ToWide(file.path));
            return false;
        }
    }
    const auto* manifestEntry = archive.Find(Update::ManifestArchivePath);
    const auto manifest = manifestEntry ? archive.Extract(*manifestEntry) : std::nullopt;
    if (!manifest || !SaveFile(Update::ToNativePath(staging, Update::ManifestArchivePath), *manifest))
    {
        return false;
    }

    const auto applier = updateDirectory / L"applier" / L"StormUpdater.exe";
    std::filesystem::create_directories(applier.parent_path(), error);
    if (!CopyFileW((g_gameDirectory / L"Mods" / L"AutoUpdate" / L"StormUpdater.exe").c_str(), applier.c_str(),
            FALSE))
    {
        return false;
    }
    auto commandLine = L"\"" + applier.wstring() + L"\" --game \"" + g_gameDirectory.wstring() + L"\" --staging \"" +
        staging.wstring() + L"\" --pid " + std::to_wstring(GetCurrentProcessId());
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(applier.c_str(), commandLine.data(), nullptr, nullptr, FALSE, DETACHED_PROCESS, nullptr,
            applier.parent_path().c_str(), &startup, &process))
    {
        return false;
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    Log(L"Staged " + std::to_wstring(plan.files.size()) + L" file(s) for " + Update::ToWide(plan.version));
    return true;
}

// The offer appears before the device exists, so exclusive fullscreen has not taken the screen yet.
HRESULT WINAPI HookedCreateDevice(IDirect3D9* direct3D, UINT adapter, D3DDEVTYPE deviceType, HWND focusWindow,
    DWORD behaviorFlags, D3DPRESENT_PARAMETERS* parameters, IDirect3DDevice9** returnedDevice)
{
    if (!g_offerShown.exchange(true) && WaitForSingleObject(g_checkDone, 2000) == WAIT_OBJECT_0 && g_plan &&
        AskToInstall(focusWindow, *g_plan))
    {
        auto started = false;
        try
        {
            started = StageAndStartApplier(*g_plan);
        }
        catch (const std::exception& exception)
        {
            Log(L"Update download failed: " + Update::ToWide(exception.what()));
        }
        if (started)
        {
            TerminateProcess(GetCurrentProcess(), 0);
        }
        MessageBoxW(focusWindow,
            Text(L"Не удалось загрузить обновление. Игра продолжит работу с текущей версией.",
                L"The update could not be downloaded. The game continues with the current version.").c_str(),
            L"Storm in a Teacup Revival", MB_OK | MB_ICONWARNING | MB_SETFOREGROUND | MB_TOPMOST);
    }
    return g_originalCreateDevice(direct3D, adapter, deviceType, focusWindow, behaviorFlags, parameters,
        returnedDevice);
}
}


extern "C" __declspec(dllexport) BOOL __cdecl StormMod_Initialize(const StormModHostApi* host)
{
    if (!host || host->version != StormModApiVersion)
    {
        return FALSE;
    }
    g_host = host;
    g_gameDirectory = host->gameDirectory;

    g_checkDone = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!g_checkDone)
    {
        return FALSE;
    }

    // Another instance may have started while an update is waiting for its game to close; it must not touch it.
    if (const auto applier = OpenMutexW(SYNCHRONIZE, FALSE, Update::UpdateMutexName.data()))
    {
        CloseHandle(applier);
        Log(L"An update is being installed; the check is skipped");
        SetEvent(g_checkDone);
        return TRUE;
    }

    // Leftovers of a finished update; the applier that may still be closing keeps its own file locked.
    std::error_code error;
    std::filesystem::remove_all(Update::DataDirectory() / L"update", error);

    const auto thread = CreateThread(nullptr, 0, CheckThread, nullptr, 0, nullptr);
    if (!thread)
    {
        return FALSE;
    }
    CloseHandle(thread);
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
    DWORD protection{};
    if (VirtualProtect(&vtable[16], sizeof(void*), PAGE_EXECUTE_READWRITE, &protection))
    {
        g_originalCreateDevice = reinterpret_cast<CreateDeviceFn>(
            InterlockedExchangePointer(&vtable[16], reinterpret_cast<void*>(&HookedCreateDevice)));
        DWORD ignored{};
        VirtualProtect(&vtable[16], sizeof(void*), protection, &ignored);
    }
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, void*)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(instance);
    }
    return TRUE;
}
