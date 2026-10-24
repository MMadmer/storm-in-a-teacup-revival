#include "GamePatches.h"

#include <d3d9.h>
#include <dsound.h>
#include <emmintrin.h>
#include <xmmintrin.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <type_traits>
#include <vector>


namespace
{
using LogFn = void(__cdecl*)(const wchar_t*);

constexpr std::size_t ParticleStride = 0x2C;
constexpr std::size_t ParticlesOffset = 0x14C;

void* g_dampingContinuation{};
void* g_oscillationContinuation{};
bool g_particlesInstalled{};
bool g_gridInstalled{};
bool g_mathInstalled{};
bool g_audioInstalled{};
void* g_shellSortContinuation{};
IDirect3DDevice9** g_gameDevice{};

template <std::size_t Size>
std::uint8_t* FindUniquePattern(HMODULE module, const std::array<int, Size>& pattern)
{
    auto* image = reinterpret_cast<std::uint8_t*>(module);
    const auto* ntHeaders = reinterpret_cast<const IMAGE_NT_HEADERS*>(
        image + reinterpret_cast<const IMAGE_DOS_HEADER*>(image)->e_lfanew
    );
    std::uint8_t* match{};
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
            const auto matches = std::equal(pattern.begin(), pattern.end(), begin + offset,
                [](int expected, std::uint8_t actual) { return expected < 0 || expected == actual; });
            if (!matches)
            {
                continue;
            }
            if (match)
            {
                return nullptr;
            }
            match = begin + offset;
        }
    }
    return match;
}

bool WriteJump(std::uint8_t* source, const void* destination, std::size_t overwrittenSize)
{
    const auto displacement = reinterpret_cast<std::intptr_t>(destination) -
        reinterpret_cast<std::intptr_t>(source + 5);
    DWORD oldProtection{};
    if (overwrittenSize < 5 || !VirtualProtect(source, overwrittenSize, PAGE_EXECUTE_READWRITE, &oldProtection))
    {
        return false;
    }
    std::fill_n(source, overwrittenSize, std::uint8_t{0x90});
    source[0] = 0xE9;
    std::memcpy(source + 1, &displacement, sizeof(std::int32_t));
    DWORD ignored{};
    VirtualProtect(source, overwrittenSize, oldProtection, &ignored);
    FlushInstructionCache(GetCurrentProcess(), source, overwrittenSize);
    return true;
}

float* Particle(const std::uint8_t* emitter, int index)
{
    return reinterpret_cast<float*>(
        *reinterpret_cast<std::uint8_t* const*>(emitter + ParticlesOffset) + index * ParticleStride
    );
}

// Sine and cosine to float precision (~1e-7): Cody-Waite reduction to [-pi/4, pi/4] plus the Cephes
// single-precision minimax polynomials, evaluated in double. Particle offsets are stored as floats anyway.
void SinCos(double x, double& sine, double& cosine)
{
    // The two-part pi/2 keeps the reduction exact below ~1e6; larger angles and NaN take the CRT path.
    if (!(std::abs(x) < 1.0e6))
    {
        sine = std::sin(x);
        cosine = std::cos(x);
        return;
    }
    constexpr double InverseHalfPi = 0.63661977236758134308;
    constexpr double HalfPiHigh = 1.57079632673412561417;
    constexpr double HalfPiLow = 6.07710050650619224932e-11;
    const auto quadrant = _mm_cvtsd_si32(_mm_set_sd(x * InverseHalfPi));
    const auto reduced = (x - quadrant * HalfPiHigh) - quadrant * HalfPiLow;
    const auto square = reduced * reduced;
    const auto s = reduced + reduced * square *
        (-1.6666654611e-1 + square * (8.3321608736e-3 + square * -1.9515295891e-4));
    const auto c = 1.0 - 0.5 * square + square * square *
        (4.166664568298827e-2 + square * (-1.388731625493765e-3 + square * 2.443315711809948e-5));
    switch (quadrant & 3)
    {
    case 0:
        sine = s;
        cosine = c;
        break;
    case 1:
        sine = c;
        cosine = -s;
        break;
    case 2:
        sine = -s;
        cosine = -c;
        break;
    default:
        sine = -c;
        cosine = s;
        break;
    }
}

// Velocity damping: every particle of an emitter shares dt and the rate, so exp() runs once per change.
void __stdcall ApplyParticleDamping(const std::uint8_t* emitter, int index, float deltaTime)
{
    static float cachedDeltaTime = std::numeric_limits<float>::quiet_NaN();
    static float cachedRate = std::numeric_limits<float>::quiet_NaN();
    static double factor{};

    const auto rate = *reinterpret_cast<const float*>(emitter + 0x154);
    if (deltaTime != cachedDeltaTime || rate != cachedRate)
    {
        cachedDeltaTime = deltaTime;
        cachedRate = rate;
        factor = std::exp(-static_cast<double>(deltaTime) * rate);
    }
    auto* velocity = Particle(emitter, index) + 3;
    velocity[0] = static_cast<float>(factor * velocity[0]);
    velocity[1] = static_cast<float>(factor * velocity[1]);
    velocity[2] = static_cast<float>(factor * velocity[2]);
}

// Oscillation: sin(a) - sin(b) and cos(a) - cos(b) via the sum-to-product identities, one sincos per term.
void __stdcall ApplyParticleOscillation(const std::uint8_t* emitter, int index, float deltaTime)
{
    // The half-step sine depends only on the emitter frequency and dt, so it is shared by all particles.
    static float cachedFrequency = std::numeric_limits<float>::quiet_NaN();
    static float cachedDeltaTime = std::numeric_limits<float>::quiet_NaN();
    static double halfSine{};

    auto* particle = Particle(emitter, index);
    const auto frequencyValue = *reinterpret_cast<const float*>(emitter + 0xB0);
    const double frequency = frequencyValue;
    const double amplitude = *reinterpret_cast<const float*>(emitter + 0xB4);
    const auto base = 2.0 * (static_cast<double>(3.1415927f) * particle[9]);
    const auto current = base + frequency * particle[7];
    const auto previous = base + (static_cast<double>(particle[7]) - deltaTime) * frequency;
    if (frequencyValue != cachedFrequency || deltaTime != cachedDeltaTime)
    {
        cachedFrequency = frequencyValue;
        cachedDeltaTime = deltaTime;
        double halfCosine{};
        SinCos(-0.5 * deltaTime * frequency, halfSine, halfCosine);
    }

    double middleSine{};
    double middleCosine{};
    SinCos(0.5 * (previous + current), middleSine, middleCosine);
    particle[0] = static_cast<float>(amplitude * (2.0 * middleCosine * halfSine) + particle[0]);
    particle[1] = static_cast<float>(amplitude * (-2.0 * middleSine * halfSine) + particle[1]);
}

// Both blocks start with an empty x87 stack and leave only memory results; dt sits at [esp+1Ch] on entry.
__declspec(naked) void ParticleDampingThunk()
{
    __asm
    {
        pushad
        push dword ptr [esp + 3Ch]
        push edi
        push esi
        call ApplyParticleDamping
        popad
        jmp dword ptr [g_dampingContinuation]
    }
}

__declspec(naked) void ParticleOscillationThunk()
{
    __asm
    {
        pushad
        push dword ptr [esp + 3Ch]
        push edi
        push esi
        call ApplyParticleOscillation
        popad
        jmp dword ptr [g_oscillationContinuation]
    }
}

void InstallParticles(HMODULE gameModule, LogFn log)
{
    // Storm+0x480B3: exp(-dt * rate) via f2xm1/fscale applied to the particle velocity.
    constexpr std::array dampingPattern{
        0xD9, 0x44, 0x24, 0x1C, 0x8B, 0x86, 0x4C, 0x01, 0x00, 0x00, 0xD8, 0x8E, 0x54, 0x01, 0x00, 0x00,
        0x8B, 0xD7, 0x6B, 0xD2, 0x2C, 0xD9, 0xE0, 0x8D, 0x44, 0x10, 0x0C, 0xD9, 0xEA, 0xDE, 0xC9
    };
    constexpr std::size_t dampingLength = 0x45;

    // Storm+0x48115: two fsin and two fcos for the particle oscillation offset.
    constexpr std::array oscillationPattern{
        0xD9, 0x05, -1, -1, -1, -1, 0x8B, 0xCF, 0x6B, 0xC9, 0x2C, 0xD8, 0x4C, 0x11, 0x24, 0xDC,
        0xC0, 0xD9, 0x44, 0x11, 0x1C, 0xD8, 0x64, 0x24, 0x1C, 0x8D, 0x04, 0x11, 0xD8, 0x8E, 0xB0, 0x00
    };
    constexpr std::size_t oscillationLength = 0x8A;

    auto* damping = FindUniquePattern(gameModule, dampingPattern);
    auto* oscillation = FindUniquePattern(gameModule, oscillationPattern);
    if (!damping || !oscillation)
    {
        return;
    }
    g_dampingContinuation = damping + dampingLength;
    g_oscillationContinuation = oscillation + oscillationLength;
    if (WriteJump(damping, &ParticleDampingThunk, 5) && WriteJump(oscillation, &ParticleOscillationThunk, 6))
    {
        g_particlesInstalled = true;
        log(L"Replaced x87 particle damping and oscillation with SSE2 math");
    }
}

#pragma pack(push, 4)
struct PhysicsGrid
{
    std::int32_t columns;
    std::int32_t rows;
    std::int32_t rowShift;
    std::uint8_t reserved0[0x0C];
    float inverseCellWidth;
    float inverseCellHeight;
    float originX;
    float originY;
    std::uint8_t reserved1[0x04];
    std::int32_t indices[1024];
    std::int32_t indexCount;
    float largestObjectRadius;
    std::int32_t cellCount;
};
#pragma pack(pop)

static_assert(offsetof(PhysicsGrid, inverseCellWidth) == 0x18);
static_assert(offsetof(PhysicsGrid, originX) == 0x20);
static_assert(offsetof(PhysicsGrid, indices) == 0x2C);
static_assert(offsetof(PhysicsGrid, indexCount) == 0x102C);
static_assert(offsetof(PhysicsGrid, cellCount) == 0x1034);

// Matches the game's clamp: below zero -> 0, past the last interior cell -> last cell, else trunc + 1.
// The game truncates through a 64-bit fistp, whose NaN result has a zero low word, so NaN maps to 1.
std::int32_t CellCoordinate(float scaled, std::int32_t count)
{
    if (scaled != scaled)
    {
        return 1;
    }
    if (scaled < 0.0f)
    {
        return 0;
    }
    if (static_cast<float>(count - 2) < scaled)
    {
        return count - 1;
    }
    return _mm_cvtt_ss2si(_mm_set_ss(scaled)) + 1;
}

// CPhysSpace::FillIndexes. Single-precision math in the game's operation order gives the same cell bounds
// as its x87 code under the 24-bit precision Direct3D sets. The game silently dropped every cell past 1024
// (the far rows) and logged one line per dropped cell; an oversized query is now trimmed around its centre.
void __fastcall FillIndexes(PhysicsGrid* grid, void*, const float* position, float radius)
{
    grid->indexCount = 0;
    const auto left = (position[0] - radius) - grid->originX;
    const auto bottom = (position[1] - radius) - grid->originY;
    const auto right = (radius + position[0]) - grid->originX;
    const auto top = (radius + position[1]) - grid->originY;

    auto column0 = CellCoordinate(left * grid->inverseCellWidth, grid->columns);
    auto column1 = CellCoordinate(right * grid->inverseCellWidth, grid->columns);
    auto row0 = CellCoordinate(bottom * grid->inverseCellHeight, grid->rows);
    auto row1 = CellCoordinate(top * grid->inverseCellHeight, grid->rows);

    // Trim the longer side one cell at a time, alternating the two ends of each axis, until the rectangle fits.
    constexpr auto capacity = static_cast<std::int32_t>(std::extent_v<decltype(PhysicsGrid::indices)>);
    const auto trim = [](std::int32_t& low, std::int32_t& high, bool& fromLow)
    {
        fromLow ? ++low : --high;
        fromLow = !fromLow;
    };
    auto columnFromLow = true;
    auto rowFromLow = true;
    for (auto width = column1 - column0 + 1, height = row1 - row0 + 1;
         width > 0 && height > 0 && static_cast<std::int64_t>(width) * height > capacity;
         width = column1 - column0 + 1, height = row1 - row0 + 1)
    {
        width >= height ? trim(column0, column1, columnFromLow) : trim(row0, row1, rowFromLow);
    }

    // Same traversal as the original do-while, including its single pass over degenerate ranges.
    auto column = column0;
    auto row = row0;
    do
    {
        const auto index = column + (row << grid->rowShift);
        if (index >= 0 && index < grid->cellCount && grid->indexCount < capacity)
        {
            grid->indices[grid->indexCount++] = index;
        }
        if (++column > column1)
        {
            ++row;
            column = column0;
        }
    }
    while (row <= row1);
}

void InstallPhysicsGrid(HMODULE gameModule, LogFn log)
{
    // Storm+0x57FA0: push ebp; mov ebp, esp; and esp, -8; sub esp, 2Ch; ...; mov [esi+102Ch], 0
    constexpr std::array pattern{
        0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF8, 0x83, 0xEC, 0x2C, 0x53, 0x56, 0x8B, 0xF1,
        0xC7, 0x86, 0x2C, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x57
    };
    if (auto* fillIndexes = FindUniquePattern(gameModule, pattern);
        fillIndexes && WriteJump(fillIndexes, &FillIndexes, 6))
    {
        g_gridInstalled = true;
        log(L"Physics grid queries no longer overflow or flood error.log");
    }
}
}



namespace
{
// Storm+0xA5A10: the renderer's vertex-constant filter compared its shadow copy with "repe cmpsb".
bool EqualVectors(const std::uint8_t* left, const std::uint8_t* right, int count)
{
    for (int index = 0; index < count; ++index, left += 16, right += 16)
    {
        const auto equal = _mm_cmpeq_epi8(_mm_loadu_si128(reinterpret_cast<const __m128i*>(left)),
            _mm_loadu_si128(reinterpret_cast<const __m128i*>(right)));
        if (_mm_movemask_epi8(equal) != 0xFFFF)
        {
            return false;
        }
    }
    return true;
}

unsigned int __fastcall SetVertexConstants(std::uint8_t* renderer, void*, int start, const float* data, int count)
{
    auto* shadow = renderer + 0x17C + start * 16;
    const auto bytes = static_cast<unsigned int>(count) * 16;
    if (EqualVectors(shadow, reinterpret_cast<const std::uint8_t*>(data), count))
    {
        return bytes;
    }
    std::memcpy(shadow, data, bytes);
    return static_cast<unsigned int>((*g_gameDevice)->SetVertexShaderConstantF(start, data, count));
}

// Storm+0x16400: 4x4 transpose that the game returned through a 64-byte "rep movsd" copy.
float* __cdecl TransposeMatrix(float* destination, const float* source)
{
    auto row0 = _mm_loadu_ps(source);
    auto row1 = _mm_loadu_ps(source + 4);
    auto row2 = _mm_loadu_ps(source + 8);
    auto row3 = _mm_loadu_ps(source + 12);
    _MM_TRANSPOSE4_PS(row0, row1, row2, row3);
    _mm_storeu_ps(destination, row0);
    _mm_storeu_ps(destination + 4, row1);
    _mm_storeu_ps(destination + 8, row2);
    _mm_storeu_ps(destination + 12, row3);
    return destination;
}

// Storm+0x89B0: destination = left * right for row-major 4x4 matrices; any argument may alias another.
float* __cdecl MultiplyMatrices(float* destination, const float* left, const float* right)
{
    const auto right0 = _mm_loadu_ps(right);
    const auto right1 = _mm_loadu_ps(right + 4);
    const auto right2 = _mm_loadu_ps(right + 8);
    const auto right3 = _mm_loadu_ps(right + 12);
    std::array<__m128, 4> rows{};
    for (std::size_t row = 0; row < rows.size(); ++row)
    {
        const auto* values = left + row * 4;
        rows[row] = _mm_add_ps(
            _mm_add_ps(_mm_mul_ps(_mm_set1_ps(values[0]), right0), _mm_mul_ps(_mm_set1_ps(values[1]), right1)),
            _mm_add_ps(_mm_mul_ps(_mm_set1_ps(values[2]), right2), _mm_mul_ps(_mm_set1_ps(values[3]), right3)));
    }
    for (std::size_t row = 0; row < rows.size(); ++row)
    {
        _mm_storeu_ps(destination + row * 4, rows[row]);
    }
    return destination;
}

struct SortEntry
{
    float key;
    int index;
};

// Storm+0x0BD40: the game's Shell sort of an index array by float key (gaps n/2, n/4, ..., 1 and a strict "<",
// so NaN keys never move). Equal keys are frequent and their order decides the blending order, so the exact
// algorithm is kept. The x87 version compared through the index array with an fnstsw stall per step; here the
// keys travel with their indices in one contiguous buffer, and each insertion shifts into a hole instead of
// swapping. Both make the same comparisons, so the result is identical.
void __stdcall SortIndices(const float* keys, int count, int* indices)
{
    if (count < 2)
    {
        return;
    }
    thread_local std::vector<SortEntry> entries;
    entries.resize(static_cast<std::size_t>(count));
    for (auto i = 0; i < count; ++i)
    {
        entries[i] = {keys[indices[i]], indices[i]};
    }
    for (auto gap = count / 2; gap > 0; gap /= 2)
    {
        for (auto i = gap; i < count; ++i)
        {
            const auto entry = entries[i];
            auto j = i;
            for (; j >= gap && entry.key < entries[j - gap].key; j -= gap)
            {
                entries[j] = entries[j - gap];
            }
            entries[j] = entry;
        }
    }
    for (auto i = 0; i < count; ++i)
    {
        indices[i] = entries[i].index;
    }
}

// Entered after the function's protected prologue: keys, count and indices are its stack arguments.
__declspec(naked) void ShellSortThunk()
{
    __asm
    {
        mov eax, esp
        push dword ptr [eax + 28h]
        push dword ptr [eax + 24h]
        push dword ptr [eax + 20h]
        call SortIndices
        jmp dword ptr [g_shellSortContinuation]
    }
}

// Storm+0x88480: the engine sets every looping sound's pitch each frame, mostly to the value it already has.
// DirectSound on current Windows makes SetFrequency expensive, while GetFrequency is a plain read.
BOOL __fastcall SetSoundFrequency(const std::uint8_t* sound, void*, float frequency)
{
    auto* buffer = *reinterpret_cast<IDirectSoundBuffer* const*>(sound + 0x10);
    if (!buffer)
    {
        return FALSE;
    }
    const auto target = static_cast<DWORD>(_mm_cvtt_ss2si(_mm_set_ss(frequency)));
    DWORD current{};
    if (SUCCEEDED(buffer->GetFrequency(&current)) && current == target)
    {
        return TRUE;
    }
    return buffer->SetFrequency(target) == DS_OK;
}

// Storm+0x87810: the listener commit recomputes every deferred 3D voice. Deferred values persist until the
// next commit, so committing 60 times a second keeps positioning updates inaudibly fresh.
BOOL __fastcall CommitDeferredAudio(const std::uint8_t* manager, void*, DWORD)
{
    auto* listener = *reinterpret_cast<IDirectSound3DListener* const*>(manager + 0x10);
    if (!listener)
    {
        return FALSE;
    }
    static LARGE_INTEGER frequency{};
    static LONGLONG nextCommit{};
    if (!frequency.QuadPart)
    {
        QueryPerformanceFrequency(&frequency);
    }
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    if (now.QuadPart < nextCommit)
    {
        return TRUE;
    }
    // A fixed schedule averages 60 Hz at any frame rate; after a stall it restarts from the current time.
    const auto interval = frequency.QuadPart / 60;
    nextCommit = now.QuadPart - nextCommit < interval ? nextCommit + interval : now.QuadPart + interval;
    return listener->CommitDeferredSettings() == DS_OK;
}

// DirectSound reopens the same registry keys on every buffer call; that kernel round trip cost more than the
// audio work itself. Keys opened by dsound.dll stay cached for the session and closing them is a no-op;
// misses are remembered for a second, so a key created later is still found.
struct CachedRegistryKey
{
    HKEY root;
    std::wstring path;
    DWORD options;
    REGSAM access;
    LSTATUS status;
    HKEY handle;
    ULONGLONG missExpiry;
};

constexpr ULONGLONG RegistryMissLifetimeMs = 1000;

using RegOpenKeyExAFn = LSTATUS(WINAPI*)(HKEY, LPCSTR, DWORD, REGSAM, PHKEY);
using RegOpenKeyExWFn = LSTATUS(WINAPI*)(HKEY, LPCWSTR, DWORD, REGSAM, PHKEY);
using RegCloseKeyFn = LSTATUS(WINAPI*)(HKEY);

RegOpenKeyExAFn g_regOpenKeyExA{};
RegOpenKeyExWFn g_regOpenKeyExW{};
RegCloseKeyFn g_regCloseKey{};
SRWLOCK g_registryLock = SRWLOCK_INIT;
std::vector<CachedRegistryKey> g_registryKeys;

std::wstring NormalizeKeyPath(const wchar_t* path)
{
    std::wstring result(path ? path : L"");
    CharLowerBuffW(result.data(), static_cast<DWORD>(result.size()));
    return result;
}

bool IsPredefinedKey(HKEY key)
{
    const auto value = reinterpret_cast<ULONG_PTR>(key);
    return value >= 0x80000000 && value <= 0x80000007;
}

LSTATUS OpenCachedKey(HKEY root, const std::wstring& path, DWORD options, REGSAM access, PHKEY result,
    LSTATUS(*open)(HKEY, DWORD, REGSAM, PHKEY, const void*), const void* originalPath)
{
    if (!result)
    {
        return open(root, options, access, result, originalPath);
    }
    const auto matches = [&](const CachedRegistryKey& key)
    {
        return key.root == root && key.options == options && key.access == access && key.path == path;
    };
    const auto now = GetTickCount64();

    // Only predefined roots and handles held by this cache are never closed, so only they keep naming the
    // same key; any other handle value may be reused by Windows for a different key later.
    auto stableRoot = IsPredefinedKey(root);
    AcquireSRWLockShared(&g_registryLock);
    for (const auto& key : g_registryKeys)
    {
        stableRoot = stableRoot || (key.status == ERROR_SUCCESS && key.handle == root);
        if (matches(key) && (key.status == ERROR_SUCCESS || now < key.missExpiry))
        {
            const auto status = key.status;
            *result = key.handle;
            ReleaseSRWLockShared(&g_registryLock);
            return status;
        }
    }
    ReleaseSRWLockShared(&g_registryLock);

    const auto status = open(root, options, access, result, originalPath);
    // Only definite answers are cached; transient errors such as access races are retried next time.
    if (stableRoot && (status == ERROR_SUCCESS || status == ERROR_FILE_NOT_FOUND))
    {
        CachedRegistryKey entry{root, path, options, access, status, status == ERROR_SUCCESS ? *result : nullptr,
            now + RegistryMissLifetimeMs};
        AcquireSRWLockExclusive(&g_registryLock);
        // An expired miss is replaced; opened handles are never dropped, since callers may still hold them.
        const auto miss = std::find_if(g_registryKeys.begin(), g_registryKeys.end(),
            [&](const CachedRegistryKey& key) { return key.status != ERROR_SUCCESS && matches(key); });
        if (miss != g_registryKeys.end())
        {
            *miss = std::move(entry);
        }
        else
        {
            g_registryKeys.push_back(std::move(entry));
        }
        ReleaseSRWLockExclusive(&g_registryLock);
    }
    return status;
}

LSTATUS WINAPI CachedRegOpenKeyExA(HKEY root, LPCSTR path, DWORD options, REGSAM access, PHKEY result)
{
    std::wstring widePath;
    if (path)
    {
        const auto length = MultiByteToWideChar(CP_ACP, 0, path, -1, nullptr, 0);
        widePath.resize(length > 0 ? length - 1 : 0);
        MultiByteToWideChar(CP_ACP, 0, path, -1, widePath.data(), length);
    }
    return OpenCachedKey(root, NormalizeKeyPath(widePath.c_str()), options, access, result,
        [](HKEY key, DWORD flags, REGSAM sam, PHKEY output, const void* original)
        {
            return g_regOpenKeyExA(key, static_cast<LPCSTR>(original), flags, sam, output);
        }, path);
}

LSTATUS WINAPI CachedRegOpenKeyExW(HKEY root, LPCWSTR path, DWORD options, REGSAM access, PHKEY result)
{
    return OpenCachedKey(root, NormalizeKeyPath(path), options, access, result,
        [](HKEY key, DWORD flags, REGSAM sam, PHKEY output, const void* original)
        {
            return g_regOpenKeyExW(key, static_cast<LPCWSTR>(original), flags, sam, output);
        }, path);
}

LSTATUS WINAPI CachedRegCloseKey(HKEY key)
{
    AcquireSRWLockShared(&g_registryLock);
    const auto cached = std::any_of(g_registryKeys.begin(), g_registryKeys.end(),
        [key](const CachedRegistryKey& entry) { return entry.status == ERROR_SUCCESS && entry.handle == key; });
    ReleaseSRWLockShared(&g_registryLock);
    return cached ? ERROR_SUCCESS : g_regCloseKey(key);
}

// Finds the import address slot of one function, matching the imported DLL name case-insensitively.
void** FindImportSlot(HMODULE module, const char* importedModule, const char* name)
{
    auto* base = reinterpret_cast<std::uint8_t*>(module);
    const auto* ntHeaders = reinterpret_cast<const IMAGE_NT_HEADERS*>(
        base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew
    );
    const auto& directory = ntHeaders->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    for (auto* descriptor = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + directory.VirtualAddress);
         descriptor->Name; ++descriptor)
    {
        if (_stricmp(reinterpret_cast<const char*>(base + descriptor->Name), importedModule) != 0)
        {
            continue;
        }
        auto* lookup = reinterpret_cast<IMAGE_THUNK_DATA*>(base + descriptor->OriginalFirstThunk);
        auto* address = reinterpret_cast<IMAGE_THUNK_DATA*>(base + descriptor->FirstThunk);
        for (; lookup->u1.AddressOfData; ++lookup, ++address)
        {
            if (IMAGE_SNAP_BY_ORDINAL(lookup->u1.Ordinal) || std::strcmp(reinterpret_cast<const char*>(
                    reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(base + lookup->u1.AddressOfData)->Name), name) != 0)
            {
                continue;
            }
            return reinterpret_cast<void**>(&address->u1.Function);
        }
    }
    return nullptr;
}

bool PatchSlot(void** slot, void* replacement)
{
    DWORD oldProtection{};
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &oldProtection))
    {
        return false;
    }
    InterlockedExchangePointer(slot, replacement);
    DWORD ignored{};
    VirtualProtect(slot, sizeof(void*), oldProtection, &ignored);
    return true;
}

bool InstallRegistryCache()
{
    // Patching twice would make the hooks their own originals.
    static bool installed{};
    const auto dsound = GetModuleHandleW(L"dsound.dll");
    if (installed || !dsound)
    {
        return false;
    }
    constexpr auto registry = "api-ms-win-core-registry-l1-1-0.dll";
    auto** closeSlot = FindImportSlot(dsound, registry, "RegCloseKey");
    auto** openASlot = FindImportSlot(dsound, registry, "RegOpenKeyExA");
    auto** openWSlot = FindImportSlot(dsound, registry, "RegOpenKeyExW");
    if (!closeSlot || !openASlot || !openWSlot)
    {
        return false;
    }

    // Other threads may already call into dsound, so the originals are set before any slot changes, and the
    // close hook goes first: a cached handle must never reach the real RegCloseKey.
    g_regCloseKey = reinterpret_cast<RegCloseKeyFn>(*closeSlot);
    g_regOpenKeyExA = reinterpret_cast<RegOpenKeyExAFn>(*openASlot);
    g_regOpenKeyExW = reinterpret_cast<RegOpenKeyExWFn>(*openWSlot);
    if (!PatchSlot(closeSlot, reinterpret_cast<void*>(&CachedRegCloseKey)))
    {
        return false;
    }
    installed = true;
    return PatchSlot(openASlot, reinterpret_cast<void*>(&CachedRegOpenKeyExA)) &&
        PatchSlot(openWSlot, reinterpret_cast<void*>(&CachedRegOpenKeyExW));
}

void InstallAudio(HMODULE gameModule, LogFn log)
{
    constexpr std::array frequencyPattern{
        0x56, 0x8B, 0x71, 0x10, 0x85, 0xF6, 0x74, 0x1F, 0xD9, 0x44, 0x24, 0x08, 0x57, 0x8B, 0x3E, 0xE8,
        -1, -1, -1, -1, 0x50, 0x56, 0xFF, 0x57, 0x44, 0x85, 0xC0, 0x5F, 0x75, 0x09, 0xB8, 0x01, 0x00, 0x00,
        0x00, 0x5E, 0xC2, 0x04, 0x00
    };
    constexpr std::array commitPattern{
        0x8B, 0x41, 0x10, 0x85, 0xC0, 0x75, 0x05, 0x32, 0xC0, 0xC2, 0x04, 0x00, 0x8B, 0x08, 0x50, 0xFF,
        0x51, 0x44, 0xF7, 0xD8, 0x1B, 0xC0, 0x40, 0xC2, 0x04, 0x00
    };
    auto* setFrequency = FindUniquePattern(gameModule, frequencyPattern);
    auto* commit = FindUniquePattern(gameModule, commitPattern);
    if (setFrequency && commit && WriteJump(setFrequency, &SetSoundFrequency, 6) &&
        WriteJump(commit, &CommitDeferredAudio, 5))
    {
        g_audioInstalled = true;
        log(L"DirectSound pitch updates and 3D commits no longer run redundantly every frame");
    }
    if (InstallRegistryCache())
    {
        log(L"DirectSound registry lookups are cached");
    }
}

void InstallMath(HMODULE gameModule, LogFn log)
{
    constexpr std::array constantsPattern{
        0x8B, 0x54, 0x24, 0x04, 0x8B, 0x44, 0x24, 0x0C, 0x53, 0x8B, 0x5C, 0x24, 0x0C, 0x55, 0xC1, 0xE2,
        0x04, 0x56, 0xC1, 0xE0, 0x04, 0x8D, 0x94, 0x0A, 0x7C, 0x01, 0x00, 0x00, 0x57, 0x8B, 0xC8, 0x8B,
        0xFB, 0x8B, 0xF2, 0x33, 0xED, 0xF3, 0xA6
    };
    constexpr std::array transposePattern{
        0x83, 0xEC, 0x40, 0x8B, 0x44, 0x24, 0x48, 0x8B, 0x08, 0x8B, 0x50, 0x10, 0x89, 0x4C, 0x24, 0x00,
        0x8B, 0x48, 0x20, 0x89, 0x54, 0x24, 0x04, 0x8B, 0x50, 0x30, 0x89, 0x4C, 0x24, 0x08
    };
    constexpr std::array multiplyPattern{
        0x83, 0xEC, 0x40, 0x8B, 0x44, 0x24, 0x4C, 0x8B, 0x4C, 0x24, 0x48, 0xD9, 0x40, 0x20, 0xD8, 0x49,
        0x08, 0x56, 0xD9, 0x41, 0x04, 0x57, 0xD8, 0x48, 0x10, 0xDE, 0xC1, 0xD9, 0x01, 0xD8, 0x08, 0xDE
    };
    constexpr std::array shellSortPattern{
        0x3B, 0xEA, 0x7D, 0x79, 0x8D, 0x04, 0xAB, 0x89, 0x44, 0x24, 0x10, 0x33, 0xC9, 0x8B, 0xC2, 0x2B,
        0xC5, 0x89, 0x4C, 0x24, 0x14, 0x89, 0x44, 0x24, 0x18, 0x8D, 0xA4, 0x24, 0x00, 0x00, 0x00, 0x00
    };
    constexpr std::size_t shellSortEpilogue = 0x87;

    auto* constants = FindUniquePattern(gameModule, constantsPattern);
    auto* transpose = FindUniquePattern(gameModule, transposePattern);
    auto* multiply = FindUniquePattern(gameModule, multiplyPattern);
    auto* shellSort = FindUniquePattern(gameModule, shellSortPattern);
    // The filter reads the device from a global through "mov eax, [device]" at +0x45.
    if (!constants || !transpose || !multiply || !shellSort || constants[0x45] != 0xA1)
    {
        return;
    }
    g_gameDevice = *reinterpret_cast<IDirect3DDevice9***>(constants + 0x46);
    g_shellSortContinuation = shellSort + shellSortEpilogue;
    if (WriteJump(constants, &SetVertexConstants, 5) && WriteJump(transpose, &TransposeMatrix, 5) &&
        WriteJump(multiply, &MultiplyMatrices, 5) && WriteJump(shellSort, &ShellSortThunk, 5))
    {
        g_mathInstalled = true;
        log(L"Replaced matrix, sort and shader-constant routines with SSE versions");
    }
}
}


namespace GamePatches
{
void Install(HMODULE gameModule, const Options& options, LogFn log)
{
    if (options.fastParticles && !g_particlesInstalled)
    {
        InstallParticles(gameModule, log);
    }
    if (options.physicsGrid && !g_gridInstalled)
    {
        InstallPhysicsGrid(gameModule, log);
    }
    if (options.fastMath && !g_mathInstalled)
    {
        InstallMath(gameModule, log);
    }
    if (options.fastAudio && !g_audioInstalled)
    {
        InstallAudio(gameModule, log);
    }
}
}
