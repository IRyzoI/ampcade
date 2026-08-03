#pragma once
// The ResamplingContainer/LanczosResampler headers vendored inside
// AudioDSPTools came from iPlug2 and expect these two symbols from it.
// Define them here so the files compile untouched outside iPlug2.

namespace iplug
{
constexpr double PI = 3.14159265358979323846;
}

#ifndef DEFAULT_BLOCK_SIZE
#define DEFAULT_BLOCK_SIZE 512
#endif
