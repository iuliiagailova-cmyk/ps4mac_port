// SPDX-License-Identifier: GPL-2.0-or-later
// RnCPort: see spartanport_trace.h.
#include "spartanport_trace.h"

#include <pthread.h>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace SpartanTrace {
namespace {

struct Event {
    const char* name;
    std::uint64_t start;
    std::uint64_t end; // == start: instant
    std::uint64_t arg;
};

struct ThreadLog {
    std::uint64_t tid = 0;
    std::string name;
    std::vector<Event> events;
    std::mutex mutex; // the writer reads it once recording has stopped
};

struct Global {
    std::mutex mutex;
    std::vector<std::unique_ptr<ThreadLog>> threads;
    const char* path = nullptr;
    const char* trigger = nullptr;
    std::uint64_t delay_ns = 0;
    std::uint64_t length_ns = 0;
    std::uint64_t process_start = 0;
    std::atomic<std::uint64_t> window_start{0};
    std::atomic<int> state{0}; // 0 waiting, 1 recording, 2 done
    std::atomic<std::uint64_t> next_poll{0};
};

Global& G() {
    static Global* g = [] {
        auto* g = new Global;
        g->path = std::getenv("SPARTAN_TRACE");
        g->trigger = std::getenv("SPARTAN_TRACE_TRIGGER");
        const char* delay = std::getenv("SPARTAN_TRACE_DELAY");
        const char* seconds = std::getenv("SPARTAN_TRACE_SECONDS");
        g->delay_ns = std::uint64_t((delay ? std::atof(delay) : 90.0) * 1e9);
        g->length_ns = std::uint64_t((seconds ? std::atof(seconds) : 3.0) * 1e9);
        g->process_start = Now();
        if (!g->path || !*g->path) {
            g->state = 2;
        }
        return g;
    }();
    return *g;
}

ThreadLog& Log() {
    thread_local ThreadLog* log = [] {
        auto owned = std::make_unique<ThreadLog>();
        owned->tid = reinterpret_cast<std::uint64_t>(pthread_self());
        char name[64] = {};
        pthread_getname_np(pthread_self(), name, sizeof(name));
        owned->name = name[0] ? name : "thread";
        owned->events.reserve(1 << 16);
        auto& g = G();
        std::scoped_lock lock{g.mutex};
        g.threads.push_back(std::move(owned));
        return g.threads.back().get();
    }();
    return *log;
}

void Escape(std::FILE* f, const std::string& s) {
    for (char c : s) {
        if (c == '"' || c == '\\') {
            std::fputc('\\', f);
        }
        if (static_cast<unsigned char>(c) >= 0x20) {
            std::fputc(c, f);
        }
    }
}

void Write() {
    auto& g = G();
    std::FILE* f = std::fopen(g.path, "w");
    if (!f) {
        std::printf("Trace: cannot write %s\n", g.path);
        return;
    }
    std::fprintf(f, "{\"base_ns\":%llu,\"traceEvents\":[\n", (unsigned long long)g.window_start.load());
    bool first = true;
    std::size_t count = 0;
    const std::uint64_t base = g.window_start.load();
    std::scoped_lock lock{g.mutex};
    for (const auto& t : g.threads) {
        std::scoped_lock tl{t->mutex};
        std::fprintf(f, "%s{\"ph\":\"M\",\"name\":\"thread_name\",\"pid\":1,\"tid\":%llu,\"args\":{\"name\":\"",
                     first ? "" : ",\n", (unsigned long long)t->tid);
        Escape(f, t->name);
        std::fputs("\"}}", f);
        first = false;
        for (const Event& e : t->events) {
            const double ts = (double(e.start) - double(base)) / 1000.0;
            if (e.end == e.start) {
                std::fprintf(f, ",\n{\"ph\":\"i\",\"s\":\"t\",\"name\":\"%s\",\"pid\":1,\"tid\":%llu,\"ts\":%.3f,\"args\":{\"v\":%llu}}",
                             e.name, (unsigned long long)t->tid, ts, (unsigned long long)e.arg);
            } else {
                std::fprintf(f, ",\n{\"ph\":\"X\",\"name\":\"%s\",\"pid\":1,\"tid\":%llu,\"ts\":%.3f,\"dur\":%.3f,\"args\":{\"v\":%llu}}",
                             e.name, (unsigned long long)t->tid, ts, (e.end - e.start) / 1000.0,
                             (unsigned long long)e.arg);
            }
            ++count;
        }
    }
    std::fputs("\n]}\n", f);
    std::fclose(f);
    std::printf("Trace: %zu events from %zu threads written to %s\n", count, g.threads.size(), g.path);
}

} // namespace

std::uint64_t Now() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

void Complete(const char* name, std::uint64_t start_ns, std::uint64_t end_ns, std::uint64_t arg) {
    if (end_ns <= start_ns) {
        end_ns = start_ns + 1;
    }
    auto& log = Log();
    std::scoped_lock lock{log.mutex};
    log.events.push_back({name, start_ns, end_ns, arg});
}

void Instant(const char* name, std::uint64_t arg) {
    const std::uint64_t now = Now();
    auto& log = Log();
    std::scoped_lock lock{log.mutex};
    log.events.push_back({name, now, now, arg});
}

void Poll() {
    auto& g = G();
    const int state = g.state.load(std::memory_order_relaxed);
    if (state == 2) {
        return;
    }
    const std::uint64_t now = Now();
    if (now < g.next_poll.load(std::memory_order_relaxed)) {
        return;
    }
    g.next_poll.store(now + 20'000'000, std::memory_order_relaxed); // every 20 ms
    if (state == 0) {
        bool start = false;
        if (g.trigger && *g.trigger) {
            start = std::remove(g.trigger) == 0;
        } else {
            start = now - g.process_start >= g.delay_ns;
        }
        int expected = 0;
        if (start && g.state.compare_exchange_strong(expected, 1)) {
            g.window_start = now;
            recording.store(true, std::memory_order_relaxed);
            std::printf("Trace: recording %.1f s\n", g.length_ns / 1e9);
        }
        return;
    }
    if (now - g.window_start.load() >= g.length_ns) {
        int expected = 1;
        if (g.state.compare_exchange_strong(expected, 2)) {
            recording.store(false, std::memory_order_relaxed);
            Write();
        }
    }
}

} // namespace SpartanTrace
