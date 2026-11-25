// Process memory usage for the debug overlay. Kept in its own TU: windows.h
// cannot share a translation unit with raylib.h (extern "C" name clashes).
#include "platform.hpp"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <psapi.h>
#endif

namespace gh::platform {

double processMemoryMB() {
#if defined(_WIN32)
    PROCESS_MEMORY_COUNTERS_EX pmc{};
    if (K32GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc),
                                sizeof(pmc)))
        return double(pmc.WorkingSetSize) / 1048576.0;
    return 0.0;
#else
    return 0.0;
#endif
}

}  // namespace gh::platform
