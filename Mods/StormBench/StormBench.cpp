#include "StormModApi.h"

#include <d3d9.h>

#include <Windows.h>
#include <TlHelp32.h>
#include <timeapi.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <iterator>
#include <string>
#include <vector>


namespace Benchmark
{
void OnFrameEnd();
}


namespace
{
using MenuBuildPageFn = void(__thiscall*)(void*, void*);
using MenuEventFn = int(__thiscall*)(void*, int);

// CGUI_MainMenu layout in Storm.exe v1.0.147.
constexpr std::uintptr_t MainMenuVtableRva = 0x108350;
constexpr std::uintptr_t MainMenuClassIdRva = 0x295D0;
constexpr std::size_t MenuBuildPageSlot = 19;
constexpr std::size_t MenuEventSlot = 22;
constexpr std::size_t MenuBusyOffset = 0xD50;
constexpr int SelectTrackEvent = 2000;
constexpr int SelectGliderEvent = 2101;

constexpr std::size_t StackCopyBytes = 16 * 1024;
constexpr std::size_t MaxReturnsPerSample = 96;
constexpr std::size_t MaxModules = 256;

enum class Stage
{
    WaitMenu,
    SelectGlider,
    Loading,
    Measure,
    Done,
};

struct Settings
{
    int track{};
    int glider{};
    double warmup{6.0};
    double duration{20.0};
    double timeout{240.0};
    bool profile{true};
    bool drive{false};
    bool screenshot{false};
    std::wstring label{L"run"};
};

struct Bucket
{
    std::uintptr_t key;
    std::uint32_t count;
};

// Fixed-capacity open-addressing counter; it never allocates after construction.
class Histogram
{
public:
    explicit Histogram(std::size_t capacityPowerOfTwo) : buckets_(capacityPowerOfTwo) {}

    void Add(std::uintptr_t key)
    {
        const auto mask = buckets_.size() - 1;
        auto index = (key * 2654435761u) & mask;
        for (std::size_t probe = 0; probe < 64; ++probe, index = (index + 1) & mask)
        {
            auto& bucket = buckets_[index];
            if (bucket.key == key)
            {
                ++bucket.count;
                return;
            }
            if (!bucket.count)
            {
                bucket = {key, 1};
                return;
            }
        }
        ++dropped_;
    }

    [[nodiscard]] std::vector<Bucket> Top(std::size_t count) const
    {
        std::vector<Bucket> used;
        std::copy_if(buckets_.begin(), buckets_.end(), std::back_inserter(used),
            [](const Bucket& bucket) { return bucket.count != 0; });
        std::sort(used.begin(), used.end(),
            [](const Bucket& left, const Bucket& right) { return left.count > right.count; });
        if (used.size() > count)
        {
            used.resize(count);
        }
        return used;
    }

    [[nodiscard]] std::uint32_t Dropped() const { return dropped_; }

private:
    std::vector<Bucket> buckets_;
    std::uint32_t dropped_{};
};

struct ModuleRange
{
    std::uintptr_t begin;
    std::uintptr_t end;
    char name[64];
};

const StormModHostApi* g_host{};
HMODULE g_module{};
Settings g_settings{};
bool g_active{};
Stage g_stage{Stage::WaitMenu};

MenuBuildPageFn g_originalMainMenuBuildPage{};
MenuEventFn g_mainMenuEvent{};
std::atomic<void*> g_mainMenu{};

LARGE_INTEGER g_frequency{};
std::int64_t g_startTicks{};
std::int64_t g_lastFrameTicks{};
std::int64_t g_lastHitchTicks{};
std::int64_t g_stageTicks{};
std::int64_t g_measureStartTicks{};
std::uint32_t g_stageFrames{};
std::vector<float> g_frameMilliseconds;

std::uintptr_t g_gameBegin{};
std::uintptr_t g_gameEnd{};
HANDLE g_mainThread{};
DWORD g_mainThreadId{};
std::uintptr_t g_mainStackBase{};

HANDLE g_samplerThread{};
std::atomic_bool g_sampling{};
std::uint32_t g_sampleCount{};
Histogram* g_selfHistogram{};
Histogram* g_inclusiveHistogram{};
Histogram* g_externalCallerHistogram{};
Histogram* g_systemCallerHistogram{};
std::size_t g_ntdllModule{MaxModules};
std::array<ModuleRange, MaxModules> g_modules{};
std::size_t g_moduleCount{};
std::array<std::uint32_t, MaxModules + 1> g_moduleSamples{};
alignas(16) std::array<std::uint8_t, StackCopyBytes> g_stackCopy{};

void Log(const wchar_t* message)
{
    if (g_host && g_host->log)
    {
        g_host->log(L"Benchmark", message);
    }
}

double Seconds(std::int64_t ticks)
{
    return static_cast<double>(ticks) / static_cast<double>(g_frequency.QuadPart);
}

std::int64_t Now()
{
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    return now.QuadPart;
}

void ParseSettings(const std::wstring& text)
{
    std::size_t position = 0;
    while (position < text.size())
    {
        auto end = text.find(L';', position);
        if (end == std::wstring::npos)
        {
            end = text.size();
        }
        const auto pair = text.substr(position, end - position);
        position = end + 1;

        const auto separator = pair.find(L'=');
        if (separator == std::wstring::npos)
        {
            continue;
        }
        const auto key = pair.substr(0, separator);
        const auto value = pair.substr(separator + 1);
        if (key == L"track")
        {
            g_settings.track = _wtoi(value.c_str());
        }
        else if (key == L"glider")
        {
            g_settings.glider = _wtoi(value.c_str());
        }
        else if (key == L"warmup")
        {
            g_settings.warmup = _wtof(value.c_str());
        }
        else if (key == L"duration")
        {
            g_settings.duration = _wtof(value.c_str());
        }
        else if (key == L"timeout")
        {
            g_settings.timeout = _wtof(value.c_str());
        }
        else if (key == L"profile")
        {
            g_settings.profile = _wtoi(value.c_str()) != 0;
        }
        else if (key == L"screenshot")
        {
            g_settings.screenshot = _wtoi(value.c_str()) != 0;
        }
        else if (key == L"drive")
        {
            g_settings.drive = _wtoi(value.c_str()) != 0;
        }
        else if (key == L"label")
        {
            g_settings.label = value;
        }
    }
}

template <typename Function>
Function PatchPointer(void** slot, Function replacement)
{
    DWORD oldProtection{};
    if (!VirtualProtect(slot, sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtection))
    {
        return {};
    }
    const auto original = reinterpret_cast<Function>(
        InterlockedExchangePointer(slot, reinterpret_cast<void*>(replacement))
    );
    DWORD ignored{};
    VirtualProtect(slot, sizeof(void*), oldProtection, &ignored);
    return original;
}

// The application loop (Storm+0x385C0) ends each frame with "mov ecx, esi; call [edx+38h]".
constexpr std::uintptr_t FrameCallRva = 0x385F7;
constexpr std::array<std::uint8_t, 5> FrameCallBytes{0x8B, 0xCE, 0xFF, 0x52, 0x38};

void __cdecl OnGameFrame()
{
    Benchmark::OnFrameEnd();
}

__declspec(naked) void FrameCallThunk()
{
    __asm
    {
        mov ecx, esi
        call dword ptr [edx + 38h]
        call OnGameFrame
        ret
    }
}

bool g_frameHookInstalled{};

// SecuROM decrypts the game code after the mods load, so this is retried until the bytes match.
bool InstallGameFrameHook(std::uint8_t* gameBase)
{
    if (g_frameHookInstalled)
    {
        return true;
    }
    auto* site = gameBase + FrameCallRva;
    if (!std::equal(FrameCallBytes.begin(), FrameCallBytes.end(), site))
    {
        return false;
    }
    const auto displacement = reinterpret_cast<std::intptr_t>(&FrameCallThunk) -
        reinterpret_cast<std::intptr_t>(site + 5);
    DWORD oldProtection{};
    if (!VirtualProtect(site, FrameCallBytes.size(), PAGE_EXECUTE_READWRITE, &oldProtection))
    {
        return false;
    }
    site[0] = 0xE8;
    std::memcpy(site + 1, &displacement, sizeof(std::int32_t));
    DWORD ignored{};
    VirtualProtect(site, FrameCallBytes.size(), oldProtection, &ignored);
    FlushInstructionCache(GetCurrentProcess(), site, FrameCallBytes.size());
    g_frameHookInstalled = true;
    return true;
}

void __fastcall HookedMainMenuBuildPage(void* menu, void*, void* page)
{
    if (!InstallGameFrameHook(static_cast<std::uint8_t*>(static_cast<void*>(g_host->gameModule))))
    {
        Log(L"Unsupported Storm.exe build: frame loop signature mismatch");
    }
    g_mainMenu = menu;
    g_originalMainMenuBuildPage(menu, page);
}

bool IsCallSite(std::uintptr_t address)
{

    // Accept only addresses that directly follow an x86 CALL encoding.
    const auto* code = reinterpret_cast<const std::uint8_t*>(address);
    if (code[-5] == 0xE8)
    {
        return true;
    }
    if (code[-2] == 0xFF && ((code[-1] & 0xF8) == 0xD0 ||
        ((code[-1] & 0xF8) == 0x10 && code[-1] != 0x14 && code[-1] != 0x15)))
    {
        return true;
    }
    if (code[-3] == 0xFF && (((code[-2] & 0xF8) == 0x50 && code[-2] != 0x54) || code[-2] == 0x14))
    {
        return true;
    }
    if (code[-4] == 0xFF && code[-3] == 0x54)
    {
        return true;
    }
    if (code[-6] == 0xFF && (code[-5] == 0x15 || ((code[-5] & 0xF8) == 0x90 && code[-5] != 0x94)))
    {
        return true;
    }
    return code[-7] == 0xFF && code[-6] == 0x94;
}

void CaptureModules()
{
    g_moduleCount = 0;
    const auto snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, GetCurrentProcessId());
    if (snapshot == INVALID_HANDLE_VALUE)
    {
        return;
    }

    MODULEENTRY32W entry{sizeof(entry)};
    if (Module32FirstW(snapshot, &entry))
    {
        do
        {
            auto& range = g_modules[g_moduleCount++];
            range.begin = reinterpret_cast<std::uintptr_t>(entry.modBaseAddr);
            range.end = range.begin + entry.modBaseSize;
            WideCharToMultiByte(CP_UTF8, 0, entry.szModule, -1, range.name, sizeof(range.name), nullptr, nullptr);
        }
        while (g_moduleCount < g_modules.size() && Module32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
}

std::size_t FindModule(std::uintptr_t address)
{
    for (std::size_t index = 0; index < g_moduleCount; ++index)
    {
        if (address >= g_modules[index].begin && address < g_modules[index].end)
        {
            return index;
        }
    }
    return MaxModules;
}

// Module images, the game's SecuROM sections included, can contain unreadable pages, so code bytes are read
// under SEH.
bool IsSystemCallSite(std::uintptr_t address)
{
    __try
    {
        return IsCallSite(address);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

bool IsGameCallSite(std::uintptr_t address)
{
    return address >= g_gameBegin + 8 && address < g_gameEnd && IsSystemCallSite(address);
}

// Return addresses inside other modules, for time the game spends below system DLLs.
void RecordSystemCallers(std::size_t stackBytes)
{
    std::size_t found = 0;
    for (std::size_t offset = 0; offset + sizeof(std::uint32_t) <= stackBytes && offset < 2048 && found < 8;
         offset += sizeof(std::uint32_t))
    {
        std::uint32_t value{};
        std::memcpy(&value, g_stackCopy.data() + offset, sizeof(value));
        const auto module = FindModule(value);
        if (module == MaxModules || (value >= g_gameBegin && value < g_gameEnd) ||
            value < g_modules[module].begin + 8 || !IsSystemCallSite(value))
        {
            continue;
        }
        g_systemCallerHistogram->Add(value);
        ++found;
    }
}

void ProcessSample(std::uintptr_t eip, std::size_t stackBytes)
{
    ++g_sampleCount;
    g_selfHistogram->Add(eip);
    ++g_moduleSamples[FindModule(eip)];

    std::array<std::uintptr_t, MaxReturnsPerSample> returns{};
    std::size_t returnCount = 0;
    for (std::size_t offset = 0; offset + sizeof(std::uint32_t) <= stackBytes && returnCount < returns.size();
         offset += sizeof(std::uint32_t))
    {
        std::uint32_t value{};
        std::memcpy(&value, g_stackCopy.data() + offset, sizeof(value));
        if (!IsGameCallSite(value) ||
            std::find(returns.begin(), returns.begin() + returnCount, value) != returns.begin() + returnCount)
        {
            continue;
        }
        returns[returnCount++] = value;
    }

    for (std::size_t index = 0; index < returnCount; ++index)
    {
        g_inclusiveHistogram->Add(returns[index]);
    }
    if (returnCount && (eip < g_gameBegin || eip >= g_gameEnd))
    {
        g_externalCallerHistogram->Add(returns[0]);
    }
    if (g_ntdllModule != MaxModules && FindModule(eip) == g_ntdllModule)
    {
        RecordSystemCallers(stackBytes);
    }
}


// Secondary threads are sampled for instruction pointer only, to see which one bounds the frame.
constexpr std::size_t MaxSurveyThreads = 48;

struct SurveyThread
{
    HANDLE handle;
    DWORD id;
    char name[48];
    std::uint32_t samples;
    std::uint32_t kernelSamples;
    Histogram* addresses;
};

std::array<SurveyThread, MaxSurveyThreads> g_surveyThreads{};
std::size_t g_surveyThreadCount{};

void CaptureSurveyThreads()
{
    const auto snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
    {
        return;
    }
    THREADENTRY32 entry{sizeof(entry)};
    const auto process = GetCurrentProcessId();
    const auto current = GetCurrentThreadId();
    if (Thread32First(snapshot, &entry))
    {
        do
        {
            if (entry.th32OwnerProcessID != process || entry.th32ThreadID == current ||
                g_surveyThreadCount == g_surveyThreads.size())
            {
                continue;
            }
            const auto handle = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT |
                THREAD_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ThreadID);
            if (!handle)
            {
                continue;
            }
            auto& thread = g_surveyThreads[g_surveyThreadCount++];
            thread.handle = handle;
            thread.id = entry.th32ThreadID;
            PWSTR description{};
            if (SUCCEEDED(GetThreadDescription(handle, &description)) && description)
            {
                WideCharToMultiByte(CP_UTF8, 0, description, -1, thread.name, sizeof(thread.name), nullptr, nullptr);
                LocalFree(description);
            }
            thread.addresses = new Histogram(1u << 12);
        }
        while (Thread32Next(snapshot, &entry));
    }
    CloseHandle(snapshot);
}

bool IsSyscallReturn(std::uintptr_t eip)
{
    // WOW64 system call stubs return right after "call edx".
    const auto module = FindModule(eip);
    if (module == MaxModules)
    {
        return false;
    }
    const auto* code = reinterpret_cast<const std::uint8_t*>(eip);
    __try
    {
        return code[-2] == 0xFF && code[-1] == 0xD2;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

void SampleSurveyThreads()
{
    CONTEXT context{};
    for (std::size_t index = 0; index < g_surveyThreadCount; ++index)
    {
        auto& thread = g_surveyThreads[index];
        if (thread.id == g_mainThreadId || SuspendThread(thread.handle) == static_cast<DWORD>(-1))
        {
            continue;
        }
        context.ContextFlags = CONTEXT_CONTROL;
        const auto captured = GetThreadContext(thread.handle, &context) != FALSE;
        ResumeThread(thread.handle);
        if (!captured)
        {
            continue;
        }
        ++thread.samples;
        if (IsSyscallReturn(context.Eip))
        {
            ++thread.kernelSamples;
        }
        thread.addresses->Add(context.Eip);
    }
}

DWORD WINAPI SamplerThread(void*)
{
    timeBeginPeriod(1);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    CONTEXT context{};
    while (g_sampling.load(std::memory_order_relaxed))
    {
        Sleep(1);

        // The game thread may hold heap or loader locks, so nothing allocates while it is suspended.
        if (SuspendThread(g_mainThread) == static_cast<DWORD>(-1))
        {
            break;
        }
        context.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
        std::size_t stackBytes = 0;
        const auto captured = GetThreadContext(g_mainThread, &context) != FALSE;
        if (captured && context.Esp < g_mainStackBase)
        {
            stackBytes = (std::min)(StackCopyBytes, static_cast<std::size_t>(g_mainStackBase - context.Esp));
            std::memcpy(g_stackCopy.data(), reinterpret_cast<const void*>(context.Esp), stackBytes);
        }
        ResumeThread(g_mainThread);

        if (captured)
        {
            ProcessSample(context.Eip, stackBytes);
        }
        SampleSurveyThreads();
    }
    timeEndPeriod(1);
    return 0;
}

void StartSampler()
{
    if (!g_settings.profile)
    {
        return;
    }

    CaptureModules();
    CaptureSurveyThreads();
    g_selfHistogram = new Histogram(1u << 18);
    g_inclusiveHistogram = new Histogram(1u << 16);
    g_externalCallerHistogram = new Histogram(1u << 14);
    g_systemCallerHistogram = new Histogram(1u << 14);
    for (std::size_t index = 0; index < g_moduleCount; ++index)
    {
        if (_stricmp(g_modules[index].name, "ntdll.dll") == 0)
        {
            g_ntdllModule = index;
        }
    }
    g_sampling = true;
    g_samplerThread = CreateThread(nullptr, 0, SamplerThread, nullptr, 0, nullptr);
    if (!g_samplerThread)
    {
        g_sampling = false;
    }
}

void StopSampler()
{
    if (!g_samplerThread)
    {
        return;
    }
    g_sampling = false;
    WaitForSingleObject(g_samplerThread, 5000);
    CloseHandle(g_samplerThread);
    g_samplerThread = {};
}

std::string DescribeAddress(std::uintptr_t address)
{
    char text[160]{};
    const auto module = FindModule(address);
    if (module == MaxModules)
    {
        _snprintf_s(text, _TRUNCATE, "0x%08IX", address);
    }
    else if (address >= g_gameBegin && address < g_gameEnd)
    {
        _snprintf_s(text, _TRUNCATE, "Storm+0x%06IX (va 0x%08IX)", address - g_gameBegin, address);
    }
    else
    {
        _snprintf_s(text, _TRUNCATE, "%s+0x%IX", g_modules[module].name, address - g_modules[module].begin);
    }
    return text;
}

void WriteHistogram(FILE* file, const char* title, const Histogram* histogram, std::size_t count)
{
    if (!histogram)
    {
        return;
    }
    std::fprintf(file, "\n== %s (dropped %u) ==\n", title, histogram->Dropped());
    for (const auto& bucket : histogram->Top(count))
    {
        std::fprintf(file, "%7u  %6.2f%%  %s\n", bucket.count,
            g_sampleCount ? 100.0 * bucket.count / g_sampleCount : 0.0,
            DescribeAddress(bucket.key).c_str());
    }
}

std::wstring ReportPath()
{
    std::array<wchar_t, MAX_PATH> path{};
    const auto length = GetModuleFileNameW(g_module, path.data(), static_cast<DWORD>(path.size()));
    std::wstring result(path.data(), length);
    result.resize(result.find_last_of(L"\\/") + 1);
    return result + L"Benchmark-" + g_settings.label + L".txt";
}

void WriteReport(const char* status)
{
    FILE* file{};
    if (_wfopen_s(&file, ReportPath().c_str(), L"wb") || !file)
    {
        return;
    }

    char label[128]{};
    WideCharToMultiByte(CP_UTF8, 0, g_settings.label.c_str(), -1, label, sizeof(label), nullptr, nullptr);
    std::fprintf(file, "label=%s status=%s track=%d glider=%d warmup=%.1f duration=%.1f\n",
        label, status, g_settings.track, g_settings.glider, g_settings.warmup, g_settings.duration);

    auto frames = g_frameMilliseconds;
    if (!frames.empty())
    {
        std::sort(frames.begin(), frames.end());
        double total = 0.0;
        for (const auto frame : frames)
        {
            total += frame;
        }
        const auto percentile = [&](double fraction)
        {
            return frames[(std::min)(frames.size() - 1, static_cast<std::size_t>(fraction * frames.size()))];
        };
        const auto slowCount = (std::max)(std::size_t{1}, frames.size() / 100);
        double slowTotal = 0.0;
        for (std::size_t index = frames.size() - slowCount; index < frames.size(); ++index)
        {
            slowTotal += frames[index];
        }
        std::fprintf(file,
            "frames=%zu avg_fps=%.2f avg_ms=%.3f median_ms=%.3f p95_ms=%.3f p99_ms=%.3f max_ms=%.3f low1_fps=%.2f\n",
            frames.size(), 1000.0 * frames.size() / total, total / frames.size(), percentile(0.5),
            percentile(0.95), percentile(0.99), frames.back(), 1000.0 * slowCount / slowTotal);

        wchar_t summary[256]{};
        _snwprintf_s(summary, _TRUNCATE, L"%s: %.2f FPS avg, %.2f FPS 1%% low, %.3f ms p99 over %zu frames",
            g_settings.label.c_str(), 1000.0 * frames.size() / total, 1000.0 * slowCount / slowTotal,
            percentile(0.99), frames.size());
        Log(summary);
    }

    if (g_sampleCount)
    {
        std::fprintf(file, "\nsamples=%u\n== modules ==\n", g_sampleCount);
        for (std::size_t index = 0; index <= g_moduleCount; ++index)
        {
            const auto samples = g_moduleSamples[index == g_moduleCount ? MaxModules : index];
            if (samples)
            {
                std::fprintf(file, "%7u  %6.2f%%  %s\n", samples, 100.0 * samples / g_sampleCount,
                    index == g_moduleCount ? "<unknown>" : g_modules[index].name);
            }
        }
        WriteHistogram(file, "self (instruction pointer)", g_selfHistogram, 700);
        WriteHistogram(file, "inclusive (game return addresses on stack)", g_inclusiveHistogram, 200);
        WriteHistogram(file, "time outside the game, by nearest game caller", g_externalCallerHistogram, 120);
        WriteHistogram(file, "system return addresses while in ntdll", g_systemCallerHistogram, 60);

        std::fprintf(file, "\n== threads (samples, %% in syscalls) ==\n");
        for (std::size_t index = 0; index < g_surveyThreadCount; ++index)
        {
            const auto& thread = g_surveyThreads[index];
            if (!thread.samples || thread.kernelSamples == thread.samples)
            {
                continue;
            }
            std::fprintf(file, "\nthread %lu '%s': busy %.1f%%\n", thread.id, thread.name,
                100.0 - 100.0 * thread.kernelSamples / thread.samples);
            for (const auto& bucket : thread.addresses->Top(12))
            {
                std::fprintf(file, "   %6u  %s\n", bucket.count, DescribeAddress(bucket.key).c_str());
            }
        }
    }
    std::fclose(file);
}

void WriteBitmap(UINT width, UINT height, const std::uint8_t* pixels, std::size_t pitch)
{
    auto path = ReportPath();
    path.replace(path.size() - 4, 4, L".bmp");
    FILE* file{};
    if (_wfopen_s(&file, path.c_str(), L"wb") || !file)
    {
        return;
    }
    BITMAPINFOHEADER info{sizeof(info), static_cast<LONG>(width), -static_cast<LONG>(height), 1, 32, BI_RGB};
    const auto pixelBytes = width * height * 4;
    BITMAPFILEHEADER header{0x4D42, static_cast<DWORD>(sizeof(BITMAPFILEHEADER) + sizeof(info) + pixelBytes), 0, 0,
        sizeof(BITMAPFILEHEADER) + sizeof(info)};
    std::fwrite(&header, sizeof(header), 1, file);
    std::fwrite(&info, sizeof(info), 1, file);
    for (UINT row = 0; row < height; ++row)
    {
        std::fwrite(pixels + row * pitch, 4, width, file);
    }
    std::fclose(file);
}

IDirect3DDevice9* GameDevice()
{
    return *reinterpret_cast<IDirect3DDevice9**>(reinterpret_cast<std::uint8_t*>(g_host->gameModule) + 0x1A7678);
}

HWND GameWindow()
{
    D3DDEVICE_CREATION_PARAMETERS creation{};
    auto* device = GameDevice();
    return device && SUCCEEDED(device->GetCreationParameters(&creation)) ? creation.hFocusWindow : nullptr;
}

// Called at the end of a game-loop iteration, when the back buffer holds the fully rendered next frame.
void SaveScreenshot()
{
    auto* device = GameDevice();
    IDirect3DSurface9* backBuffer{};
    if (!device || FAILED(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backBuffer)))
    {
        return;
    }
    D3DSURFACE_DESC desc{};
    IDirect3DSurface9* resolved{};
    IDirect3DSurface9* readable{};
    D3DLOCKED_RECT locked{};
    // The bitmap writer expects 32-bit pixels; 16-bit back buffers are skipped.
    if (SUCCEEDED(backBuffer->GetDesc(&desc)) &&
        (desc.Format == D3DFMT_X8R8G8B8 || desc.Format == D3DFMT_A8R8G8B8) &&
        SUCCEEDED(device->CreateRenderTarget(desc.Width, desc.Height, desc.Format, D3DMULTISAMPLE_NONE, 0, FALSE,
            &resolved, nullptr)) &&
        SUCCEEDED(device->StretchRect(backBuffer, nullptr, resolved, nullptr, D3DTEXF_NONE)) &&
        SUCCEEDED(device->CreateOffscreenPlainSurface(desc.Width, desc.Height, desc.Format, D3DPOOL_SYSTEMMEM,
            &readable, nullptr)) &&
        SUCCEEDED(device->GetRenderTargetData(resolved, readable)) &&
        SUCCEEDED(readable->LockRect(&locked, nullptr, D3DLOCK_READONLY)))
    {
        WriteBitmap(desc.Width, desc.Height, static_cast<const std::uint8_t*>(locked.pBits), locked.Pitch);
        readable->UnlockRect();
    }
    for (auto* surface : {readable, resolved, backBuffer})
    {
        if (surface)
        {
            surface->Release();
        }
    }
}

// A game started from a console does not get the foreground, and a background borderless window would skew
// the measurement; a synthetic Alt press lifts the foreground lock for this one call.
void BringGameToFront()
{
    const auto window = GameWindow();
    if (!window || GetForegroundWindow() == window)
    {
        return;
    }
    INPUT alt[2]{};
    alt[0].type = INPUT_KEYBOARD;
    alt[0].ki.wVk = VK_MENU;
    alt[1] = alt[0];
    alt[1].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(1, &alt[0], sizeof(INPUT));
    SetForegroundWindow(window);
    SendInput(1, &alt[1], sizeof(INPUT));
    Log(GetForegroundWindow() == window ? L"Game window brought to the foreground"
                                        : L"Game window is not in the foreground");
}

void Finish(const char* status)
{
    if (g_settings.drive)
    {
        INPUT input{};
        input.type = INPUT_KEYBOARD;
        input.ki.wScan = 0x48;
        input.ki.dwFlags = KEYEVENTF_SCANCODE | KEYEVENTF_EXTENDEDKEY | KEYEVENTF_KEYUP;
        SendInput(1, &input, sizeof(input));
    }
    StopSampler();
    WriteReport(status);
    if (g_settings.screenshot)
    {
        SaveScreenshot();
    }
    g_stage = Stage::Done;
    TerminateProcess(GetCurrentProcess(), 0);
}

void EnterStage(Stage stage, std::int64_t now)
{
    g_stage = stage;
    g_stageTicks = now;
    g_stageFrames = 0;
}

bool IsMenuReady(void* menu)
{
    return menu && !*(static_cast<const std::uint8_t*>(menu) + MenuBusyOffset);
}

void StartMeasurement(std::int64_t now)
{
    EnterStage(Stage::Measure, now);
    g_frameMilliseconds.clear();
    g_frameMilliseconds.reserve(400000);

    if (!g_mainThread)
    {
        DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &g_mainThread,
            THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, 0);
        g_mainStackBase = reinterpret_cast<std::uintptr_t>(reinterpret_cast<NT_TIB*>(NtCurrentTeb())->StackBase);
        g_mainThreadId = GetCurrentThreadId();
    }
    StartSampler();

    if (g_settings.drive)
    {
        INPUT input{};
        input.type = INPUT_KEYBOARD;
        input.ki.wScan = 0x48;
        input.ki.dwFlags = KEYEVENTF_SCANCODE | KEYEVENTF_EXTENDEDKEY;
        SendInput(1, &input, sizeof(input));
    }
    Log(L"Measurement started");

    // The setup above must not land in the first measured frame.
    g_measureStartTicks = g_lastFrameTicks = Now();
}
}


namespace Benchmark
{
bool Initialize(const StormModHostApi* host, HMODULE module)
{
    std::array<wchar_t, 512> value{};
    const auto length = GetEnvironmentVariableW(L"STORM_BENCH", value.data(), static_cast<DWORD>(value.size()));
    if (!length || length >= value.size())
    {
        return false;
    }

    g_host = host;
    g_module = module;
    ParseSettings(std::wstring(value.data(), length));
    QueryPerformanceFrequency(&g_frequency);
    g_startTicks = Now();

    auto* gameBase = reinterpret_cast<std::uint8_t*>(host->gameModule);
    const auto* ntHeaders = reinterpret_cast<const IMAGE_NT_HEADERS*>(
        gameBase + reinterpret_cast<const IMAGE_DOS_HEADER*>(gameBase)->e_lfanew
    );
    g_gameBegin = reinterpret_cast<std::uintptr_t>(gameBase);
    g_gameEnd = g_gameBegin + ntHeaders->OptionalHeader.SizeOfImage;

    auto** vtable = reinterpret_cast<void**>(gameBase + MainMenuVtableRva);
    if (vtable[1] != gameBase + MainMenuClassIdRva)
    {
        Log(L"Unsupported Storm.exe build: main menu vtable mismatch");
        return false;
    }
    InstallGameFrameHook(gameBase);
    g_mainMenuEvent = reinterpret_cast<MenuEventFn>(vtable[MenuEventSlot]);
    g_originalMainMenuBuildPage = reinterpret_cast<MenuBuildPageFn>(
        PatchPointer(&vtable[MenuBuildPageSlot], reinterpret_cast<void*>(&HookedMainMenuBuildPage))
    );
    if (!g_originalMainMenuBuildPage)
    {
        return false;
    }

    g_active = true;
    wchar_t message[256]{};
    _snwprintf_s(message, _TRUNCATE, L"Armed: label=%s track=%d glider=%d warmup=%.1f duration=%.1f profile=%d",
        g_settings.label.c_str(), g_settings.track, g_settings.glider, g_settings.warmup, g_settings.duration,
        g_settings.profile ? 1 : 0);
    Log(message);
    return true;
}


void OnFrameEnd()
{
    if (!g_active || g_stage == Stage::Done)
    {
        return;
    }

    const auto now = Now();
    if (g_lastFrameTicks)
    {
        const auto frameTicks = now - g_lastFrameTicks;
        if (Seconds(frameTicks) > 0.25)
        {
            g_lastHitchTicks = now;
        }
        if (g_stage == Stage::Measure)
        {
            g_frameMilliseconds.push_back(static_cast<float>(Seconds(frameTicks) * 1000.0));
        }
    }
    g_lastFrameTicks = now;
    ++g_stageFrames;

    if (Seconds(now - g_startTicks) > g_settings.timeout)
    {
        Finish("timeout");
        return;
    }

    auto* menu = g_mainMenu.load();
    switch (g_stage)
    {
    case Stage::WaitMenu:
        if (!menu)
        {
            g_stageFrames = 0;
        }
        else if (g_stageFrames >= 60 && IsMenuReady(menu))
        {
            BringGameToFront();
            g_mainMenuEvent(menu, SelectTrackEvent + g_settings.track);
            EnterStage(Stage::SelectGlider, now);
            Log(L"Track selected");
        }
        break;
    case Stage::SelectGlider:
        if (g_stageFrames >= 30 && IsMenuReady(menu))
        {
            g_mainMenuEvent(menu, SelectGliderEvent + g_settings.glider);
            EnterStage(Stage::Loading, now);
            g_lastHitchTicks = now;
            Log(L"Glider selected, race loading");
        }
        break;
    case Stage::Loading:
        if (Seconds(now - g_stageTicks) > 3.0 && Seconds(now - g_lastHitchTicks) >= g_settings.warmup)
        {
            StartMeasurement(now);
        }
        break;
    case Stage::Measure:
        if (Seconds(now - g_measureStartTicks) >= g_settings.duration)
        {
            Finish("ok");
        }
        break;
    case Stage::Done:
        break;
    }
}
}


extern "C" __declspec(dllexport) BOOL __cdecl StormMod_Initialize(const StormModHostApi* host)
{
    if (!host || host->version != StormModApiVersion)
    {
        return FALSE;
    }
    Benchmark::Initialize(host, g_module);
    return TRUE;
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
