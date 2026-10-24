#include "StormModApi.h"

#include <Windows.h>
#include <d3dkmthk.h>
#define DIRECTINPUT_VERSION 0x0800
#include <dinput.h>
#include <d3d9.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>


namespace
{
enum class DisplayMode : int
{
    Windowed = 0,
    Fullscreen = 1,
    Borderless = 2,
};

using CreateDeviceFn = HRESULT(WINAPI*)(
    IDirect3D9*,
    UINT,
    D3DDEVTYPE,
    HWND,
    DWORD,
    D3DPRESENT_PARAMETERS*,
    IDirect3DDevice9**
);
using Direct3DCreate9Fn = IDirect3D9*(WINAPI*)(UINT);
using OptionsBuildPageFn = void(__thiscall*)(void*, void*);
using OptionsEventFn = int(__thiscall*)(void*, int);

#pragma pack(push, 1)
struct MenuEntry
{
    char label[32];
    std::int32_t eventId;
    std::uint8_t enabled;
    char value[35];
    std::uint32_t normalColor;
    std::uint32_t selectedColor;
};

struct MenuPage
{
    char title[32];
    MenuEntry entries[40];
};
#pragma pack(pop)

static_assert(sizeof(MenuEntry) == 80);

HMODULE g_module{};
const StormModHostApi* g_host{};
std::wstring g_iniPath;
std::atomic<DisplayMode> g_appliedMode{DisplayMode::Fullscreen};
DisplayMode g_pendingMode{DisplayMode::Fullscreen};
DisplayMode g_modeBeforeApply{DisplayMode::Fullscreen};
std::atomic_bool g_appliedVSync{true};
std::atomic_bool g_hardwareVSync{};
bool g_pendingVSync{true};
bool g_vsyncBeforeApply{true};
std::atomic_int g_appliedFpsLimit{};
int g_pendingFpsLimit{};
int g_fpsLimitBeforeApply{};
HANDLE g_frameTimer{};
LARGE_INTEGER g_performanceFrequency{};
std::int64_t g_lastLimitedFrame{};
std::atomic_uint32_t g_frameLimitGeneration{};
std::uint32_t g_observedFrameLimitGeneration{};
D3DKMT_HANDLE g_vblankAdapter{};
D3DDDI_VIDEO_PRESENT_SOURCE_ID g_vblankSourceId{};
HMONITOR g_vblankMonitor{};
Direct3DCreate9Fn g_originalDirect3DCreate9{};
CreateDeviceFn g_originalCreateDevice{};
D3DPRESENT_PARAMETERS* g_gamePresentParameters{};
bool g_callSitesPatched{};
OptionsBuildPageFn g_originalOptionsBuildPage{};
OptionsEventFn g_originalOptionsEvent{};
std::atomic_bool g_videoPageVisible{};
std::atomic_bool g_settingsCommitPending{};
std::atomic<HWND> g_gameWindow{};
std::uintptr_t g_keyboardOwner{};
ULONGLONG g_nextKeyboardScan{};
std::atomic_bool g_keyboardModeDirty{};
UINT g_backBufferWidth{};
UINT g_backBufferHeight{};
std::atomic<std::uint64_t> g_windowLayout{};
std::atomic_bool g_maintenanceStarted{};

void Log(const wchar_t* message)
{
    if (g_host && g_host->log)
    {
        g_host->log(L"ExpandedSettings", message);
    }
}

std::wstring GetModuleDirectory()
{
    std::array<wchar_t, 32768> path{};
    const auto length = GetModuleFileNameW(g_module, path.data(), static_cast<DWORD>(path.size()));
    if (!length || length == path.size())
    {
        return {};
    }
    std::wstring result(path.data(), length);
    result.resize(result.find_last_of(L"\\/"));
    return result;
}

template <typename Function>
Function PatchVtableEntry(void** vtable, std::size_t index, Function replacement)
{
    DWORD oldProtection{};
    if (!VirtualProtect(&vtable[index], sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtection))
    {
        return {};
    }

    const auto original = reinterpret_cast<Function>(InterlockedExchangePointer(
        &vtable[index],
        reinterpret_cast<void*>(replacement)
    ));
    DWORD ignored{};
    VirtualProtect(&vtable[index], sizeof(void*), oldProtection, &ignored);
    FlushInstructionCache(GetCurrentProcess(), &vtable[index], sizeof(void*));
    return original;
}

template <typename Function>
Function PatchImport(HMODULE module, const char* importedModule, const char* name, Function replacement)
{
    auto* base = reinterpret_cast<std::uint8_t*>(module);
    const auto* dosHeader = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dosHeader->e_magic != IMAGE_DOS_SIGNATURE)
    {
        return {};
    }

    const auto* ntHeaders = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dosHeader->e_lfanew);
    if (ntHeaders->Signature != IMAGE_NT_SIGNATURE)
    {
        return {};
    }

    const auto& importDirectory = ntHeaders->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    auto* descriptor = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + importDirectory.VirtualAddress);
    for (; descriptor->Name; ++descriptor)
    {
        const auto* moduleName = reinterpret_cast<const char*>(base + descriptor->Name);
        if (_stricmp(moduleName, importedModule) != 0)
        {
            continue;
        }

        auto* lookup = reinterpret_cast<IMAGE_THUNK_DATA*>(
            base + (descriptor->OriginalFirstThunk ? descriptor->OriginalFirstThunk : descriptor->FirstThunk)
        );
        auto* address = reinterpret_cast<IMAGE_THUNK_DATA*>(base + descriptor->FirstThunk);
        for (; lookup->u1.AddressOfData; ++lookup, ++address)
        {
            if (IMAGE_SNAP_BY_ORDINAL(lookup->u1.Ordinal))
            {
                continue;
            }
            const auto* import = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(
                base + lookup->u1.AddressOfData
            );
            if (std::strcmp(reinterpret_cast<const char*>(import->Name), name) != 0)
            {
                continue;
            }

            auto** slot = reinterpret_cast<void**>(&address->u1.Function);
            DWORD oldProtection{};
            if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &oldProtection))
            {
                return {};
            }
            const auto original = reinterpret_cast<Function>(InterlockedExchangePointer(
                slot,
                reinterpret_cast<void*>(replacement)
            ));
            DWORD ignored{};
            VirtualProtect(slot, sizeof(void*), oldProtection, &ignored);
            return original;
        }
    }
    return {};
}

DisplayMode ReadDisplayMode()
{
    const auto value = GetPrivateProfileIntW(L"Video", L"DisplayMode", 1, g_iniPath.c_str());
    if (value == 0)
    {
        return DisplayMode::Windowed;
    }
    if (value == 2)
    {
        return DisplayMode::Borderless;
    }
    return DisplayMode::Fullscreen;
}

bool ReadVSync()
{
    return GetPrivateProfileIntW(L"Video", L"VSync", 1, g_iniPath.c_str()) != 0;
}

int ReadFpsLimit()
{
    const auto value = static_cast<int>(
        GetPrivateProfileIntW(L"Video", L"FpsLimit", 0, g_iniPath.c_str())
    );
    return (std::clamp)(value, 0, 240) / 30 * 30;
}

void WriteSettings()
{
    wchar_t value[16]{};
    _snwprintf_s(value, _TRUNCATE, L"%d", static_cast<int>(g_appliedMode.load()));
    WritePrivateProfileStringW(L"Video", L"DisplayMode", value, g_iniPath.c_str());
    _snwprintf_s(value, _TRUNCATE, L"%d", g_appliedVSync ? 1 : 0);
    WritePrivateProfileStringW(L"Video", L"VSync", value, g_iniPath.c_str());
    _snwprintf_s(value, _TRUNCATE, L"%d", g_appliedFpsLimit.load());
    WritePrivateProfileStringW(L"Video", L"FpsLimit", value, g_iniPath.c_str());
}

void ResetFrameLimiter()
{
    g_frameLimitGeneration.fetch_add(1, std::memory_order_relaxed);
}

void WaitForFrameLimit()
{
    const auto fpsLimit = g_appliedFpsLimit.load(std::memory_order_relaxed);
    if (!fpsLimit || !g_performanceFrequency.QuadPart)
    {
        g_lastLimitedFrame = 0;
        return;
    }

    const auto generation = g_frameLimitGeneration.load(std::memory_order_relaxed);
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    if (generation != g_observedFrameLimitGeneration || !g_lastLimitedFrame)
    {
        g_observedFrameLimitGeneration = generation;
        g_lastLimitedFrame = now.QuadPart;
        return;
    }

    const auto frameTicks = g_performanceFrequency.QuadPart / fpsLimit;
    const auto deadline = g_lastLimitedFrame + frameTicks;
    if (now.QuadPart >= deadline)
    {
        g_lastLimitedFrame = now.QuadPart;
        return;
    }

    auto remainingTicks = deadline - now.QuadPart;
    const auto spinTicks = (std::max)(g_performanceFrequency.QuadPart / 5000, 1LL);
    if (remainingTicks > spinTicks && g_frameTimer)
    {
        const auto timerTicks = remainingTicks - spinTicks;
        LARGE_INTEGER dueTime{};
        dueTime.QuadPart = -(std::max)(
            timerTicks * 10'000'000 / g_performanceFrequency.QuadPart,
            1LL
        );
        if (SetWaitableTimer(g_frameTimer, &dueTime, 0, nullptr, nullptr, FALSE))
        {
            WaitForSingleObject(g_frameTimer, INFINITE);
        }
    }

    do
    {
        YieldProcessor();
        QueryPerformanceCounter(&now);
        remainingTicks = deadline - now.QuadPart;
    }
    while (remainingTicks > 0);
    g_lastLimitedFrame = deadline;
}

bool IsReadableMemory(DWORD protection)
{
    constexpr DWORD readable = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
        PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    return (protection & readable) != 0 && !(protection & (PAGE_GUARD | PAGE_NOACCESS));
}

std::uintptr_t GetKeyboardOwnerVtable()
{
    return reinterpret_cast<std::uintptr_t>(g_host->gameModule) + 0x114478;
}

// Game threads may free the inspected memory at any time, so every read is SEH-guarded.
bool ReadKeyboardOwner(std::uintptr_t owner, HWND& window, IDirectInputDevice8A*& keyboard)
{
    __try
    {
        if (*reinterpret_cast<const std::uintptr_t*>(owner) != GetKeyboardOwnerVtable())
        {
            return false;
        }
        window = *reinterpret_cast<HWND*>(owner + 0x24);
        keyboard = *reinterpret_cast<IDirectInputDevice8A**>(owner + 0x2C);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }

    MEMORY_BASIC_INFORMATION deviceMemory{};
    return window && IsWindow(window) && keyboard &&
        VirtualQuery(keyboard, &deviceMemory, sizeof(deviceMemory)) &&
        deviceMemory.State == MEM_COMMIT && IsReadableMemory(deviceMemory.Protect);
}

std::uintptr_t FindKeyboardOwnerInRegion(std::uintptr_t begin, std::uintptr_t end)
{
    const auto vtable = GetKeyboardOwnerVtable();
    __try
    {
        for (auto candidate = (begin + 3) & ~std::uintptr_t{3}; candidate + 0x30 <= end; candidate += 4)
        {
            if (*reinterpret_cast<const std::uintptr_t*>(candidate) == vtable)
            {
                HWND window{};
                IDirectInputDevice8A* keyboard{};
                if (ReadKeyboardOwner(candidate, window, keyboard))
                {
                    return candidate;
                }
            }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
    return 0;
}

DWORD GetKeyboardCooperativeFlags()
{
    return g_appliedMode == DisplayMode::Fullscreen
        ? DISCL_NONEXCLUSIVE | DISCL_FOREGROUND | DISCL_NOWINKEY
        : DISCL_NONEXCLUSIVE | DISCL_FOREGROUND;
}

// In windowed modes the game answers WM_SIZE by suspending and resetting its graphics, and that suspend
// handler uses the sound and music managers without checks. They are created only after the intro videos,
// so until then the window must not be resized or the game crashes (vanilla does too if resized then).
bool IsGameAudioReady()
{
    const auto* gameBase = reinterpret_cast<const std::uint8_t*>(g_host->gameModule);
    return *reinterpret_cast<void* const*>(gameBase + 0x1A66D4) &&
        *reinterpret_cast<void* const*>(gameBase + 0x1A66D8);
}

// The owner's destructor leaves its fields behind, so a device released during shutdown can still look valid.
bool SetKeyboardCooperativeLevel(IDirectInputDevice8A* keyboard, HWND window, DWORD flags)
{
    __try
    {
        keyboard->Unacquire();
        const auto result = keyboard->SetCooperativeLevel(window, flags);
        keyboard->Acquire();
        return SUCCEEDED(result);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

// Called only from the maintenance thread, which owns all keyboard state.
void ApplyKeyboardCooperativeMode()
{
    HWND window{};
    IDirectInputDevice8A* keyboard{};
    if (!g_keyboardOwner || !ReadKeyboardOwner(g_keyboardOwner, window, keyboard))
    {
        g_keyboardOwner = 0;
        return;
    }

    // Losing the keyboard runs the same suspend handler, so the mode only changes while the game is in front
    // and fully initialised; until then the change stays pending.
    if (GetForegroundWindow() != window || !IsGameAudioReady())
    {
        g_keyboardModeDirty = true;
        return;
    }

    // The game may re-acquire between Unacquire and SetCooperativeLevel; the next pass retries.
    if (!SetKeyboardCooperativeLevel(keyboard, window, GetKeyboardCooperativeFlags()))
    {
        g_keyboardModeDirty = true;
    }
}

void DiscoverKeyboardDevice()
{
    HWND window{};
    IDirectInputDevice8A* keyboard{};
    if (g_keyboardOwner && ReadKeyboardOwner(g_keyboardOwner, window, keyboard))
    {
        return;
    }
    g_keyboardOwner = 0;

    // Scanning the whole address space is expensive, so a missing keyboard is looked for every few seconds.
    const auto now = GetTickCount64();
    if (now < g_nextKeyboardScan)
    {
        return;
    }
    g_nextKeyboardScan = now + 5000;

    SYSTEM_INFO systemInfo{};
    GetSystemInfo(&systemInfo);
    auto address = reinterpret_cast<std::uintptr_t>(systemInfo.lpMinimumApplicationAddress);
    const auto maximumAddress = reinterpret_cast<std::uintptr_t>(systemInfo.lpMaximumApplicationAddress);
    while (address < maximumAddress)
    {
        MEMORY_BASIC_INFORMATION memory{};
        if (!VirtualQuery(reinterpret_cast<void*>(address), &memory, sizeof(memory)))
        {
            break;
        }

        const auto regionStart = reinterpret_cast<std::uintptr_t>(memory.BaseAddress);
        const auto regionEnd = regionStart + memory.RegionSize;
        if (memory.State == MEM_COMMIT && memory.Type == MEM_PRIVATE &&
            IsReadableMemory(memory.Protect) && memory.RegionSize >= 0x30)
        {
            if (const auto owner = FindKeyboardOwnerInRegion(regionStart, regionEnd))
            {
                g_keyboardOwner = owner;
                ApplyKeyboardCooperativeMode();
                return;
            }
        }
        address = regionEnd;
    }
}

void WriteCp1251(char* destination, std::size_t capacity, const wchar_t* text)
{
    std::memset(destination, 0, capacity);
    WideCharToMultiByte(
        1251,
        0,
        text,
        -1,
        destination,
        static_cast<int>(capacity),
        nullptr,
        nullptr
    );
}

const wchar_t* DisplayModeText(DisplayMode mode)
{
    switch (mode)
    {
    case DisplayMode::Windowed:
        return L"В окне";
    case DisplayMode::Borderless:
        return L"В окне без рамки";
    default:
        return L"Полноэкранный";
    }
}

void InjectEntry(
    MenuPage& page,
    std::size_t index,
    const wchar_t* label,
    const wchar_t* value,
    std::int32_t eventId
)
{
    auto& entry = page.entries[index];
    entry = {};
    WriteCp1251(entry.label, std::size(entry.label), label);
    WriteCp1251(entry.value, std::size(entry.value), value);
    entry.eventId = eventId;
    entry.enabled = 1;
    entry.normalColor = 0xFF40A0FF;
    entry.selectedColor = 0xFF55556E;
}

void InjectExpandedVideoEntries(MenuPage& page)
{
    const auto applyEntry = page.entries[7];
    const auto backEntry = page.entries[8];
    InjectEntry(page, 6, L"Режим экрана", DisplayModeText(g_pendingMode), 1100);
    InjectEntry(
        page,
        7,
        L"Вертикальная синхронизация",
        g_pendingVSync ? L"Вкл." : L"Выкл.",
        1110
    );

    wchar_t fpsLimit[16]{};
    if (g_pendingFpsLimit)
    {
        _snwprintf_s(fpsLimit, _TRUNCATE, L"%d", g_pendingFpsLimit);
    }
    else
    {
        wcscpy_s(fpsLimit, L"Без лимита");
    }
    InjectEntry(page, 8, L"Лимит FPS", fpsLimit, 1120);
    page.entries[9] = applyEntry;
    page.entries[10] = backEntry;
}

bool IsVideoPage(const MenuPage& page)
{
    constexpr std::array<std::uint8_t, 5> videoTitle{0xC2, 0xC8, 0xC4, 0xC5, 0xCE};
    return std::memcmp(page.title, videoTitle.data(), videoTitle.size()) == 0 &&
        page.title[videoTitle.size()] == '\0';
}

// Row count, reserved last row, gap-shift limit and row spacing inside OptionsBuildPage.
constexpr std::array<std::size_t, 4> MenuLayoutOffsets{0x1DE, 0x1F7, 0x2AB, 0x38B};
constexpr std::array<std::uint8_t, 4> OriginalMenuLayout{9, 8, 8, 42};
constexpr std::array<std::uint8_t, 4> ExpandedMenuLayout{11, 10, 9, 34};

// The layout constants are shared by every options page, so the expanded values are
// written only while the Video page is being built and restored for all other pages.
bool SetMenuLayout(bool expanded)
{
    if (!g_originalOptionsBuildPage)
    {
        return false;
    }

    auto* buildPage = reinterpret_cast<std::uint8_t*>(g_originalOptionsBuildPage);
    const auto& current = expanded ? OriginalMenuLayout : ExpandedMenuLayout;
    const auto& target = expanded ? ExpandedMenuLayout : OriginalMenuLayout;
    auto alreadyApplied = true;
    for (std::size_t index = 0; index < MenuLayoutOffsets.size(); ++index)
    {
        const auto value = buildPage[MenuLayoutOffsets[index]];
        if (value != current[index] && value != target[index])
        {
            return false;
        }
        alreadyApplied = alreadyApplied && value == target[index];
    }
    if (alreadyApplied)
    {
        return true;
    }

    constexpr auto firstOffset = MenuLayoutOffsets.front();
    constexpr auto patchSize = MenuLayoutOffsets.back() - firstOffset + 1;
    DWORD oldProtection{};
    if (!VirtualProtect(buildPage + firstOffset, patchSize, PAGE_EXECUTE_READWRITE, &oldProtection))
    {
        return false;
    }
    for (std::size_t index = 0; index < MenuLayoutOffsets.size(); ++index)
    {
        buildPage[MenuLayoutOffsets[index]] = target[index];
    }
    DWORD ignored{};
    VirtualProtect(buildPage + firstOffset, patchSize, oldProtection, &ignored);
    FlushInstructionCache(GetCurrentProcess(), buildPage + firstOffset, patchSize);
    return true;
}

// Mode and back-buffer size of the last device creation or reset that laid out the window. Only the game
// thread writes it, so a maintenance pass during a slow Reset cannot cancel the re-layout after it.
std::uint64_t WindowLayout(DisplayMode mode, UINT width, UINT height)
{
    return (static_cast<std::uint64_t>(mode) + 1) << 48 | static_cast<std::uint64_t>(width) << 24 | height;
}

void ApplyWindowStyle(HWND window, DisplayMode mode, UINT width, UINT height)
{
    const auto monitor = MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST);
    MONITORINFO monitorInfo{sizeof(monitorInfo)};
    GetMonitorInfoW(monitor, &monitorInfo);

    // Only a leftover topmost state from exclusive fullscreen needs a z-order change; otherwise the window
    // stays where the user put it in the stack.
    const auto topmost = (GetWindowLongPtrW(window, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0;
    const auto insertAfter = topmost ? HWND_NOTOPMOST : nullptr;
    const UINT zOrderFlags = topmost ? 0 : SWP_NOZORDER;

    if (mode == DisplayMode::Windowed)
    {
        constexpr DWORD style = WS_OVERLAPPEDWINDOW | WS_VISIBLE | WS_CLIPSIBLINGS;
        constexpr DWORD extendedStyle = WS_EX_APPWINDOW | WS_EX_WINDOWEDGE;
        SetWindowLongPtrW(window, GWL_STYLE, style);
        SetWindowLongPtrW(window, GWL_EXSTYLE, extendedStyle);

        const auto workWidth = monitorInfo.rcWork.right - monitorInfo.rcWork.left;
        const auto workHeight = monitorInfo.rcWork.bottom - monitorInfo.rcWork.top;
        RECT frameRectangle{};
        AdjustWindowRectEx(&frameRectangle, style, FALSE, extendedStyle);
        const auto frameWidth = frameRectangle.right - frameRectangle.left;
        const auto frameHeight = frameRectangle.bottom - frameRectangle.top;
        const auto maximumClientWidth = (std::max)(1L, workWidth - frameWidth);
        const auto maximumClientHeight = (std::max)(1L, workHeight - frameHeight);
        const auto scale = (std::min)({
            1.0,
            static_cast<double>(maximumClientWidth) / (std::max)(1u, width),
            static_cast<double>(maximumClientHeight) / (std::max)(1u, height),
        });
        const auto clientWidth = static_cast<LONG>(width * scale);
        const auto clientHeight = static_cast<LONG>(height * scale);
        RECT rectangle{0, 0, clientWidth, clientHeight};
        AdjustWindowRectEx(&rectangle, style, FALSE, extendedStyle);
        const auto windowWidth = rectangle.right - rectangle.left;
        const auto windowHeight = rectangle.bottom - rectangle.top;
        const auto x = monitorInfo.rcWork.left +
            (monitorInfo.rcWork.right - monitorInfo.rcWork.left - windowWidth) / 2;
        const auto y = monitorInfo.rcWork.top +
            (monitorInfo.rcWork.bottom - monitorInfo.rcWork.top - windowHeight) / 2;
        SetWindowPos(
            window,
            insertAfter,
            x,
            y,
            windowWidth,
            windowHeight,
            SWP_FRAMECHANGED | SWP_SHOWWINDOW | SWP_NOACTIVATE | zOrderFlags
        );
        return;
    }

    SetWindowLongPtrW(window, GWL_STYLE, WS_POPUP | WS_VISIBLE | WS_CLIPSIBLINGS);
    SetWindowLongPtrW(window, GWL_EXSTYLE, WS_EX_APPWINDOW);
    if (mode == DisplayMode::Borderless)
    {
        SetWindowPos(
            window,
            insertAfter,
            monitorInfo.rcMonitor.left,
            monitorInfo.rcMonitor.top,
            monitorInfo.rcMonitor.right - monitorInfo.rcMonitor.left,
            monitorInfo.rcMonitor.bottom - monitorInfo.rcMonitor.top,
            SWP_FRAMECHANGED | SWP_SHOWWINDOW | SWP_NOACTIVATE | zOrderFlags
        );
    }
}

void CloseVerticalBlankAdapter()
{
    if (!g_vblankAdapter)
    {
        return;
    }

    D3DKMT_CLOSEADAPTER closeData{g_vblankAdapter};
    D3DKMTCloseAdapter(&closeData);
    g_vblankAdapter = {};
    g_vblankSourceId = {};
    g_vblankMonitor = {};
}

bool EnsureVerticalBlankAdapter()
{
    const auto monitor = MonitorFromWindow(g_gameWindow, MONITOR_DEFAULTTOPRIMARY);
    if (g_vblankAdapter && monitor == g_vblankMonitor)
    {
        return true;
    }

    CloseVerticalBlankAdapter();
    MONITORINFOEXW monitorInfo{};
    monitorInfo.cbSize = sizeof(monitorInfo);
    if (!GetMonitorInfoW(monitor, &monitorInfo))
    {
        return false;
    }

    const auto deviceContext = CreateDCW(nullptr, monitorInfo.szDevice, nullptr, nullptr);
    if (!deviceContext)
    {
        return false;
    }

    D3DKMT_OPENADAPTERFROMHDC openData{};
    openData.hDc = deviceContext;
    const auto status = D3DKMTOpenAdapterFromHdc(&openData);
    DeleteDC(deviceContext);
    if (status < 0)
    {
        return false;
    }

    g_vblankAdapter = openData.hAdapter;
    g_vblankSourceId = openData.VidPnSourceId;
    g_vblankMonitor = monitor;
    return true;
}

void WaitForVerticalBlank()
{
    if (!g_appliedVSync || g_hardwareVSync || !EnsureVerticalBlankAdapter())
    {
        return;
    }

    D3DKMT_WAITFORVERTICALBLANKEVENT waitData{};
    waitData.hAdapter = g_vblankAdapter;
    waitData.VidPnSourceId = g_vblankSourceId;
    if (D3DKMTWaitForVerticalBlankEvent(&waitData) < 0)
    {
        CloseVerticalBlankAdapter();
    }
}

void ConfigurePresentation(D3DPRESENT_PARAMETERS& parameters)
{
    // Exclusive fullscreen syncs in hardware; windowed modes are composed by DWM and paced by
    // WaitForVerticalBlank instead, so VSync can change there without a device reset.
    const auto hardwareVSync = g_appliedMode == DisplayMode::Fullscreen && g_appliedVSync;
    parameters.PresentationInterval = hardwareVSync ? D3DPRESENT_INTERVAL_ONE : D3DPRESENT_INTERVAL_IMMEDIATE;
    g_hardwareVSync = hardwareVSync;
    if (g_appliedMode == DisplayMode::Fullscreen)
    {
        parameters.Windowed = FALSE;
        g_backBufferWidth = parameters.BackBufferWidth;
        g_backBufferHeight = parameters.BackBufferHeight;
        return;
    }

    auto* gameBase = reinterpret_cast<std::uint8_t*>(g_host->gameModule);
    const auto configPath = std::wstring(g_host->gameDirectory) + L"\\data\\Config\\cfg.ini";
    const auto selectedWidth = static_cast<std::uint32_t>(GetPrivateProfileIntW(
        L"Video",
        L"Width",
        *reinterpret_cast<std::uint32_t*>(gameBase + 0x1A7E8C),
        configPath.c_str()
    ));
    const auto selectedHeight = static_cast<std::uint32_t>(GetPrivateProfileIntW(
        L"Video",
        L"Height",
        *reinterpret_cast<std::uint32_t*>(gameBase + 0x1A7E90),
        configPath.c_str()
    ));
    if (selectedWidth && selectedHeight)
    {
        parameters.BackBufferWidth = selectedWidth;
        parameters.BackBufferHeight = selectedHeight;
    }
    parameters.Windowed = TRUE;
    parameters.BackBufferFormat = D3DFMT_UNKNOWN;
    parameters.FullScreen_RefreshRateInHz = 0;
    parameters.hDeviceWindow = g_gameWindow;
    g_backBufferWidth = parameters.BackBufferWidth;
    g_backBufferHeight = parameters.BackBufferHeight;
}

void SynchronizeEngineDisplayMode(const D3DPRESENT_PARAMETERS& parameters)
{
    auto* gameBase = reinterpret_cast<std::uint8_t*>(g_host->gameModule);
    const auto windowed = g_appliedMode != DisplayMode::Fullscreen;
    *reinterpret_cast<std::uint8_t*>(gameBase + 0x1A77D0) = windowed ? 1 : 0;
    *reinterpret_cast<std::uint32_t*>(gameBase + 0x1A77B0) =
        windowed ? 0 : parameters.BackBufferWidth;
    *reinterpret_cast<std::uint32_t*>(gameBase + 0x1A77B4) =
        windowed ? 0 : parameters.BackBufferHeight;
}

bool IsWindowAbove(HWND window, HWND reference)
{
    for (auto current = GetTopWindow(nullptr); current; current = GetWindow(current, GW_HWNDNEXT))
    {
        if (current == reference)
        {
            return false;
        }
        if (current == window)
        {
            return true;
        }
    }
    return false;
}

void MaintainWindowState()
{
    if (!g_gameWindow || g_appliedMode == DisplayMode::Fullscreen || IsIconic(g_gameWindow) || !IsGameAudioReady())
    {
        return;
    }

    // Minimize and maximize are the user's choice, so those bits never count as a style mismatch.
    constexpr LONG_PTR placementStyles = WS_MINIMIZE | WS_MAXIMIZE;
    const auto desiredStyle = static_cast<LONG_PTR>(g_appliedMode == DisplayMode::Windowed
        ? WS_OVERLAPPEDWINDOW | WS_VISIBLE | WS_CLIPSIBLINGS
        : WS_POPUP | WS_VISIBLE | WS_CLIPSIBLINGS);
    const auto desiredExtendedStyle = static_cast<LONG_PTR>(g_appliedMode == DisplayMode::Windowed
        ? WS_EX_APPWINDOW | WS_EX_WINDOWEDGE
        : WS_EX_APPWINDOW);
    if ((GetWindowLongPtrW(g_gameWindow, GWL_STYLE) & ~placementStyles) != desiredStyle ||
        GetWindowLongPtrW(g_gameWindow, GWL_EXSTYLE) != desiredExtendedStyle)
    {
        ApplyWindowStyle(
            g_gameWindow,
            g_appliedMode,
            g_backBufferWidth,
            g_backBufferHeight
        );
    }

    const auto foreground = GetForegroundWindow();
    if (g_appliedMode == DisplayMode::Borderless && foreground &&
        foreground != g_gameWindow && IsWindowAbove(g_gameWindow, foreground))
    {
        SetWindowPos(
            g_gameWindow,
            foreground,
            0,
            0,
            0,
            0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE
        );
    }
}

// Returns to the settings that were active before the last Apply, also on the menu page.
void RollBackAppliedSettings()
{
    if (g_appliedMode != g_modeBeforeApply)
    {
        g_keyboardModeDirty = true;
    }
    g_appliedMode = g_modeBeforeApply;
    g_appliedVSync = g_vsyncBeforeApply;
    g_appliedFpsLimit = g_fpsLimitBeforeApply;
    g_hardwareVSync = g_appliedMode == DisplayMode::Fullscreen && g_appliedVSync;
    g_pendingMode = g_appliedMode;
    g_pendingVSync = g_appliedVSync;
    g_pendingFpsLimit = g_appliedFpsLimit;
    ResetFrameLimiter();
}

HRESULT WINAPI GameReset(IDirect3DDevice9* device, D3DPRESENT_PARAMETERS* parameters)
{
    if (parameters)
    {
        D3DDEVICE_CREATION_PARAMETERS creationParameters{};
        device->GetCreationParameters(&creationParameters);
        g_gameWindow = creationParameters.hFocusWindow;
        // The game passes its own parameter block, which the engine sync rewrites, so the explicit
        // back-buffer size has to be written after it.
        SynchronizeEngineDisplayMode(*parameters);
        ConfigurePresentation(*parameters);
    }
    const auto result = device->Reset(parameters);

    // A lost-device recovery keeps mode and size, so the user's window placement survives it.
    const auto layout = parameters
        ? WindowLayout(g_appliedMode, parameters->BackBufferWidth, parameters->BackBufferHeight)
        : 0;
    if (SUCCEEDED(result) && parameters && g_windowLayout.exchange(layout) != layout)
    {
        ApplyWindowStyle(
            g_gameWindow,
            g_appliedMode,
            parameters->BackBufferWidth,
            parameters->BackBufferHeight
        );
    }
    if (g_settingsCommitPending.exchange(false))
    {
        if (SUCCEEDED(result))
        {
            WriteSettings();
            Log(L"Expanded video settings applied and saved");
        }
        else
        {
            RollBackAppliedSettings();
            Log(L"Video settings reset failed; selections rolled back");
        }
    }
    return result;
}

HRESULT WINAPI GamePresent(IDirect3DDevice9* device)
{
    MaintainWindowState();
    WaitForFrameLimit();
    WaitForVerticalBlank();
    const auto window = g_appliedMode == DisplayMode::Fullscreen ? nullptr : g_gameWindow.load();
    return device->Present(nullptr, nullptr, window, nullptr);
}

// The native D3D9 runtime rewrites its per-device dispatch table, so device vtable hooks get lost. The game
// presents from one place and resets from three, so those call sites are redirected here instead.
// Storm+0xA3BF8: push 0 x4; push eax; call [ecx+44h]
__declspec(naked) void PresentCallThunk()
{
    __asm
    {
        push eax
        call GamePresent
        ret
    }
}

// Storm+0xA7427, 0xA780C, 0xA7832: push offset presentParameters; push eax; call [ecx+40h]
__declspec(naked) void ResetCallThunk()
{
    __asm
    {
        push dword ptr [g_gamePresentParameters]
        push eax
        call GameReset
        ret
    }
}

template <std::size_t Size>
std::vector<std::uint8_t*> FindCode(const std::array<std::uint8_t, Size>& pattern)
{
    std::vector<std::uint8_t*> matches;
    auto* image = reinterpret_cast<std::uint8_t*>(g_host->gameModule);
    const auto* ntHeaders = reinterpret_cast<const IMAGE_NT_HEADERS*>(
        image + reinterpret_cast<const IMAGE_DOS_HEADER*>(image)->e_lfanew
    );
    const auto* section = IMAGE_FIRST_SECTION(ntHeaders);
    for (WORD index = 0; index < ntHeaders->FileHeader.NumberOfSections; ++index, ++section)
    {
        const auto size = static_cast<std::size_t>(section->Misc.VirtualSize);
        if (!(section->Characteristics & IMAGE_SCN_MEM_EXECUTE) || size < pattern.size())
        {
            continue;
        }
        auto* begin = image + section->VirtualAddress;
        for (std::size_t offset = 0; offset <= size - pattern.size(); ++offset)
        {
            if (std::equal(pattern.begin(), pattern.end(), begin + offset))
            {
                matches.push_back(begin + offset);
            }
        }
    }
    return matches;
}

struct CallPatch
{
    std::uint8_t* site;
    const void* target;
    std::size_t length;
    DWORD protection;
};

// Every site is unlocked before any is written: a game redirected at only some of them would mix the
// mod's presentation path with its own.
bool WriteCalls(std::vector<CallPatch>& patches)
{
    std::size_t unlocked = 0;
    while (unlocked < patches.size() && VirtualProtect(patches[unlocked].site, patches[unlocked].length,
        PAGE_EXECUTE_READWRITE, &patches[unlocked].protection))
    {
        ++unlocked;
    }
    const auto complete = unlocked == patches.size();
    if (complete)
    {
        for (const auto& [site, target, length, protection] : patches)
        {
            const auto displacement = reinterpret_cast<std::intptr_t>(target) -
                reinterpret_cast<std::intptr_t>(site + 5);
            std::fill_n(site, length, std::uint8_t{0x90});
            site[0] = 0xE8;
            std::memcpy(site + 1, &displacement, sizeof(std::int32_t));
            FlushInstructionCache(GetCurrentProcess(), site, length);
        }
    }

    // Sites can share a page, and a later unlock saw the protection an earlier one set, so the original
    // protections are restored in reverse order.
    while (unlocked)
    {
        const auto& patch = patches[--unlocked];
        DWORD ignored{};
        VirtualProtect(patch.site, patch.length, patch.protection, &ignored);
    }
    return complete;
}

// Runs once the protected game code is decrypted, which is the case by the time the device is created.
void PatchDeviceCallSites()
{
    if (g_callSitesPatched)
    {
        return;
    }
    constexpr std::array<std::uint8_t, 20> presentSite{
        0x8B, 0x86, 0x20, 0x06, 0x00, 0x00, 0x8B, 0x08, 0x6A, 0x00, 0x6A, 0x00, 0x6A, 0x00, 0x6A, 0x00,
        0x50, 0xFF, 0x51, 0x44
    };
    constexpr std::array<std::uint8_t, 16> resetSite{
        0xA1, 0x78, 0x76, 0x5A, 0x00, 0x8B, 0x08, 0x68, 0xB0, 0x77, 0x5A, 0x00, 0x50, 0xFF, 0x51, 0x40
    };
    const auto presents = FindCode(presentSite);
    const auto resets = FindCode(resetSite);
    if (presents.size() != 1 || resets.size() != 3)
    {
        Log(L"Unsupported Storm.exe build: Present/Reset call sites not found");
        return;
    }

    g_gamePresentParameters = *reinterpret_cast<D3DPRESENT_PARAMETERS**>(resets.front() + 8);
    std::vector<CallPatch> patches{{presents.front() + 8, &PresentCallThunk, 12}};
    for (auto* site : resets)
    {
        patches.push_back({site + 7, &ResetCallThunk, 9});
    }
    g_callSitesPatched = WriteCalls(patches);
    Log(g_callSitesPatched ? L"Present and Reset call sites redirected" : L"Present/Reset call site patching failed");
}

DWORD WINAPI MaintainRuntimeState(void*)
{
    Sleep(10000);
    while (true)
    {
        DiscoverKeyboardDevice();
        if (g_keyboardModeDirty.exchange(false))
        {
            ApplyKeyboardCooperativeMode();
        }
        MaintainWindowState();
        Sleep(250);
    }
}

WNDPROC g_layoutWindowProcedure{};

LRESULT CALLBACK SuppressSizeWindowProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    return message == WM_SIZE ? 0 : CallWindowProcA(g_layoutWindowProcedure, window, message, wParam, lParam);
}

// The intro videos play before the game's first Present, so the window is laid out before the device exists.
// The game would answer that resize with its suspend-and-reset path, which crashes before the sound manager
// exists; the device is about to be created at the new size anyway, so WM_SIZE is withheld meanwhile.
void LayOutWindowBeforeDevice(HWND window, UINT width, UINT height)
{
    g_layoutWindowProcedure = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrA(window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&SuppressSizeWindowProcedure)));
    if (!g_layoutWindowProcedure)
    {
        return;
    }
    ApplyWindowStyle(window, g_appliedMode, width, height);
    SetWindowLongPtrA(window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_layoutWindowProcedure));
}

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
    // Without its Present and Reset hooks the mod could not pace frames or follow mode changes, so it
    // leaves presentation, the window and the menu entirely to the game.
    PatchDeviceCallSites();
    if (!g_callSitesPatched)
    {
        return g_originalCreateDevice(direct3D, adapter, deviceType, focusWindow, behaviorFlags, parameters,
            returnedDevice);
    }
    if (parameters)
    {
        g_gameWindow = focusWindow;
        // The game passes its own parameter block, which the engine sync rewrites, so the explicit
        // back-buffer size has to be written after it.
        SynchronizeEngineDisplayMode(*parameters);
        ConfigurePresentation(*parameters);
        if (g_appliedMode != DisplayMode::Fullscreen)
        {
            LayOutWindowBeforeDevice(focusWindow, parameters->BackBufferWidth, parameters->BackBufferHeight);
        }
    }
    const auto result = g_originalCreateDevice(
        direct3D,
        adapter,
        deviceType,
        focusWindow,
        behaviorFlags,
        parameters,
        returnedDevice
    );
    if (SUCCEEDED(result))
    {
        if (parameters)
        {
            g_windowLayout = WindowLayout(g_appliedMode, parameters->BackBufferWidth, parameters->BackBufferHeight);
        }
        if (!g_maintenanceStarted.exchange(true))
        {
            const auto maintenance = CreateThread(
                nullptr,
                0,
                MaintainRuntimeState,
                nullptr,
                0,
                nullptr
            );
            if (maintenance)
            {
                CloseHandle(maintenance);
            }
        }
        if (g_settingsCommitPending.exchange(false))
        {
            WriteSettings();
        }
    }
    else if (g_settingsCommitPending.exchange(false))
    {
        RollBackAppliedSettings();
    }
    return result;
}

IDirect3D9* WINAPI HookedDirect3DCreate9(UINT sdkVersion)
{
    const auto direct3D = g_originalDirect3DCreate9(sdkVersion);
    if (direct3D && !g_originalCreateDevice)
    {
        auto** vtable = *reinterpret_cast<void***>(direct3D);
        g_originalCreateDevice = PatchVtableEntry(vtable, 16, HookedCreateDevice);
    }
    return direct3D;
}

void __fastcall HookedOptionsBuildPage(void* menu, void*, MenuPage* page)
{
    if (!g_callSitesPatched)
    {
        g_originalOptionsBuildPage(menu, page);
        return;
    }

    // A null page rebuilds the copy kept at menu+0x50, which already holds the injected rows.
    const auto* layoutPage = page
        ? page
        : reinterpret_cast<const MenuPage*>(static_cast<std::uint8_t*>(menu) + 0x50);
    const auto isVideoLayout = IsVideoPage(*layoutPage);
    const auto expandedLayout = SetMenuLayout(isVideoLayout) && isVideoLayout;
    if (page && isVideoLayout)
    {
        if (!g_videoPageVisible.exchange(true))
        {
            g_pendingMode = g_appliedMode;
            g_pendingVSync = g_appliedVSync;
            g_pendingFpsLimit = g_appliedFpsLimit.load();
        }
        if (expandedLayout)
        {
            InjectExpandedVideoEntries(*page);
        }
        else
        {
            InjectEntry(
                *page,
                6,
                L"Режим экрана",
                DisplayModeText(g_pendingMode),
                1100
            );
        }
    }
    else if (page)
    {
        g_videoPageVisible = false;
    }
    g_originalOptionsBuildPage(menu, page);

    // The builder is shared with the main and pause menus, so the Video layout lives only for this call.
    if (expandedLayout)
    {
        SetMenuLayout(false);
    }
}

int __fastcall HookedOptionsEvent(void* menu, void*, int eventId)
{
    if (!g_callSitesPatched)
    {
        return g_originalOptionsEvent(menu, eventId);
    }

    if (eventId == 1100 || eventId == 1101)
    {
        const auto direction = eventId == 1100 ? 1 : 2;
        g_pendingMode = static_cast<DisplayMode>(
            (static_cast<int>(g_pendingMode) + direction) % 3
        );
        return g_originalOptionsEvent(menu, 1008);
    }

    if (eventId == 1110 || eventId == 1111)
    {
        g_pendingVSync = !g_pendingVSync;
        return g_originalOptionsEvent(menu, 1008);
    }

    if (eventId == 1120 || eventId == 1121)
    {
        constexpr auto valueCount = 9;
        const auto currentIndex = g_pendingFpsLimit / 30;
        const auto direction = eventId == 1120 ? 1 : valueCount - 1;
        g_pendingFpsLimit = (currentIndex + direction) % valueCount * 30;
        return g_originalOptionsEvent(menu, 1008);
    }

    const auto settingsChanged = g_pendingMode != g_appliedMode ||
        g_pendingVSync != g_appliedVSync ||
        g_pendingFpsLimit != g_appliedFpsLimit.load();
    if (eventId == 1014 && settingsChanged)
    {
        const auto displayResetRequired = g_pendingMode != g_appliedMode ||
            (g_pendingMode == DisplayMode::Fullscreen && g_pendingVSync != g_appliedVSync);

        g_modeBeforeApply = g_appliedMode;
        g_vsyncBeforeApply = g_appliedVSync;
        g_fpsLimitBeforeApply = g_appliedFpsLimit.load();
        g_appliedMode = g_pendingMode;
        g_appliedVSync = g_pendingVSync;
        g_appliedFpsLimit = g_pendingFpsLimit;
        ResetFrameLimiter();
        g_settingsCommitPending = displayResetRequired;
        if (g_appliedMode != g_modeBeforeApply)
        {
            g_keyboardModeDirty = true;
        }

        if (displayResetRequired)
        {
            // The game changes the video mode only when the selected resolution index (830) differs from
            // the current one (840), which it never rewrites itself. The current index is invalidated for
            // this one call, and ChangeMode resets the device synchronously inside it.
            auto* values = static_cast<std::int32_t*>(menu);
            const auto savedCurrentResolution = values[840];
            if (values[840] == values[830])
            {
                values[840] = -1;
            }
            const auto result = g_originalOptionsEvent(menu, eventId);
            values[840] = savedCurrentResolution;
            if (g_settingsCommitPending.exchange(false))
            {
                RollBackAppliedSettings();
                Log(L"Video settings were not applied by a device reset; selections rolled back");
            }
            return result;
        }
        WriteSettings();
        Log(L"Expanded video settings applied and saved");
    }
    else if (eventId == 1005)
    {
        g_pendingMode = g_appliedMode;
        g_pendingVSync = g_appliedVSync;
        g_pendingFpsLimit = g_appliedFpsLimit.load();
        g_settingsCommitPending = false;
        g_videoPageVisible = false;
    }

    return g_originalOptionsEvent(menu, eventId);
}

bool InstallHooks()
{
    // Validate the build before patching anything: a failed init unloads this DLL.
    auto* gameBase = reinterpret_cast<std::uint8_t*>(g_host->gameModule);
    auto** optionsVtable = reinterpret_cast<void**>(gameBase + 0x108020);
    const auto expectedClassId = reinterpret_cast<void*>(gameBase + 0x25FC0);
    if (optionsVtable[1] != expectedClassId)
    {
        Log(L"Unsupported Storm.exe build: options vtable signature mismatch");
        return false;
    }

    g_appliedMode = ReadDisplayMode();
    g_pendingMode = g_appliedMode;
    g_appliedVSync = ReadVSync();
    g_pendingVSync = g_appliedVSync;
    g_appliedFpsLimit = ReadFpsLimit();
    g_pendingFpsLimit = g_appliedFpsLimit.load();

    // Benchmark runs measure uncapped frame pacing without touching the saved settings.
    if (GetEnvironmentVariableW(L"STORM_BENCH", nullptr, 0))
    {
        g_appliedVSync = false;
        g_pendingVSync = false;
        g_appliedFpsLimit = 0;
        g_pendingFpsLimit = 0;
    }
    QueryPerformanceFrequency(&g_performanceFrequency);
    g_frameTimer = CreateWaitableTimerExW(
        nullptr,
        nullptr,
        CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
        TIMER_ALL_ACCESS
    );
    if (!g_frameTimer)
    {
        g_frameTimer = CreateWaitableTimerW(nullptr, FALSE, nullptr);
    }

    g_originalOptionsBuildPage = reinterpret_cast<OptionsBuildPageFn>(
        PatchVtableEntry(optionsVtable, 19, reinterpret_cast<void*>(HookedOptionsBuildPage))
    );
    g_originalOptionsEvent = reinterpret_cast<OptionsEventFn>(
        PatchVtableEntry(optionsVtable, 22, reinterpret_cast<void*>(HookedOptionsEvent))
    );
    if (!g_originalOptionsBuildPage || !g_originalOptionsEvent)
    {
        // Restore whichever entry was patched so nothing points into the unloaded DLL.
        if (g_originalOptionsBuildPage)
        {
            PatchVtableEntry(optionsVtable, 19, reinterpret_cast<void*>(g_originalOptionsBuildPage));
        }
        if (g_originalOptionsEvent)
        {
            PatchVtableEntry(optionsVtable, 22, reinterpret_cast<void*>(g_originalOptionsEvent));
        }
        Log(L"Runtime hook installation failed");
        return false;
    }

    // The d3d9 proxy reports the instance through StormMod_OnDirect3D9Created; the import
    // hook only covers late injection, so its absence is not fatal.
    g_originalDirect3DCreate9 = PatchImport(
        g_host->gameModule,
        "d3d9.dll",
        "Direct3DCreate9",
        HookedDirect3DCreate9
    );
    Log(L"Runtime hooks installed");
    return true;
}
}


extern "C" __declspec(dllexport) BOOL __cdecl StormMod_Initialize(const StormModHostApi* host)
{
    if (!host || host->version != StormModApiVersion)
    {
        return FALSE;
    }

    g_host = host;
    g_iniPath = GetModuleDirectory() + L"\\ExpandedSettings.ini";
    if (!InstallHooks())
    {
        return FALSE;
    }
    return TRUE;
}

extern "C" __declspec(dllexport) void __cdecl StormMod_Shutdown()
{
    if (g_frameTimer)
    {
        CloseHandle(g_frameTimer);
        g_frameTimer = {};
    }
    CloseVerticalBlankAdapter();
}

extern "C" __declspec(dllexport) void __cdecl StormMod_OnDirect3D9Created(void* instance)
{
    auto* direct3D = static_cast<IDirect3D9*>(instance);
    if (direct3D && !g_originalCreateDevice)
    {
        auto** vtable = *reinterpret_cast<void***>(direct3D);
        g_originalCreateDevice = PatchVtableEntry(vtable, 16, HookedCreateDevice);
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
