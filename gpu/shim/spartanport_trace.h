// SPDX-License-Identifier: GPL-2.0-or-later
// RnCPort: a timeline of the GPU layer's threads for a few seconds, as Chrome trace JSON
// (opens in https://ui.perfetto.dev).
//
// SPARTAN_TRACE=<file.json> enables it. Recording starts when SPARTAN_TRACE_TRIGGER=<file>
// appears (the file is removed) or, without a trigger, SPARTAN_TRACE_DELAY seconds after start
// (default 90), and lasts SPARTAN_TRACE_SECONDS (default 3); the JSON is written once.
// Off, a scope costs one relaxed load.
#pragma once

#include <atomic>
#include <cstdint>

namespace SpartanTrace {

/// True while events are being recorded.
inline std::atomic<bool> recording{false};

std::uint64_t Now();
void Complete(const char* name, std::uint64_t start_ns, std::uint64_t end_ns, std::uint64_t arg = 0);
void Instant(const char* name, std::uint64_t arg = 0);
/// Called often from busy threads: starts or ends the recording window when due.
void Poll();

class Scope {
public:
    explicit Scope(const char* name_, std::uint64_t arg_ = 0) {
        if (recording.load(std::memory_order_relaxed)) {
            name = name_;
            arg = arg_;
            start = Now();
        }
    }
    ~Scope() {
        if (name) {
            Complete(name, start, Now(), arg);
        }
    }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

private:
    const char* name = nullptr;
    std::uint64_t start = 0;
    std::uint64_t arg = 0;
};

inline void Mark(const char* name, std::uint64_t arg = 0) {
    if (recording.load(std::memory_order_relaxed)) {
        Instant(name, arg);
    }
}

} // namespace SpartanTrace
