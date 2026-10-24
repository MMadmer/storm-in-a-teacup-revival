#pragma once

#include <Windows.h>


namespace GamePatches
{
struct Options
{
    bool fastParticles;
    bool physicsGrid;
    bool fastMath;
    bool fastAudio;
};

// Applies the code patches once the protected game code has been decrypted; safe to call repeatedly.
void Install(HMODULE gameModule, const Options& options, void(__cdecl* log)(const wchar_t*));
}
