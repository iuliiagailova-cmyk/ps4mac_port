// SPDX-License-Identifier: GPL-2.0-or-later
// ps4mac_port: portable spin-wait hint and cheap cycle counter, for code that used the x86-64
// intrinsics __builtin_ia32_pause and __rdtsc directly (the video core also builds for arm64).
// The counter's unit differs by CPU (TSC ticks, or the 24 MHz arm64 virtual counter); callers only
// compare counts with each other.
#pragma once

#include "common/arch.h"
#include "common/types.h"

#ifdef ARCH_X86_64
#include <x86intrin.h>
#endif

namespace Common {

inline void CpuPause() {
#ifdef ARCH_X86_64
    __builtin_ia32_pause();
#elif defined(ARCH_ARM64)
    asm volatile("yield");
#endif
}

inline u64 CycleCount() {
#ifdef ARCH_X86_64
    return __rdtsc();
#elif defined(ARCH_ARM64)
    u64 value;
    asm volatile("mrs %0, cntvct_el0" : "=r"(value));
    return value;
#endif
}

/// Approximate CycleCount() ticks per second, for diagnostics (x86-64: assumes a ~3 GHz TSC).
inline double CycleCountsPerSecond() {
#ifdef ARCH_X86_64
    return 3.0e9;
#elif defined(ARCH_ARM64)
    u64 value;
    asm volatile("mrs %0, cntfrq_el0" : "=r"(value));
    return double(value);
#endif
}

} // namespace Common
