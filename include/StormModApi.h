#pragma once

#include <Windows.h>
#include <cstdint>


inline constexpr std::uint32_t StormModApiVersion = 1;

struct StormModHostApi
{
    std::uint32_t size;
    std::uint32_t version;
    HMODULE gameModule;
    const wchar_t* gameDirectory;
    void(__cdecl* log)(const wchar_t* modName, const wchar_t* message);
};

using StormModInitialize = BOOL(__cdecl*)(const StormModHostApi* host);
using StormModShutdown = void(__cdecl*)();
