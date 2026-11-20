/* SPDX-License-Identifier: GPL-2.0-or-later
 * ps4mac_port step 3.3b: the runtime functions the video core (libspartanportgpu) calls, on the
 * graphics service's side. In one process they live in the x86 runtime (src/runtime_memory.c,
 * src/runtime_kernel.c); here they work from the service's copy of the guest's mapping table,
 * which the bridge keeps current (PS4MAC_MEMORY_MAP/UNMAP/PROTECT, in order with everything else).
 *
 * Guest memory is mapped read-write here at the guest's addresses, so the video core reads and
 * writes it directly. Page tracking (runtime_memory_gpu_protect) belongs to the guest and goes
 * over the bridge in step 3.4; until then it is only counted.
 */
#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <execinfo.h>

#include "service_runtime.h"

/* ---- The mapping table: sorted, non-overlapping [start, end) ranges with the guest's prot. */
typedef struct {
    uintptr_t start, end;
    int prot;
} Range;

static Range *ranges;
static size_t range_count, range_capacity;
static pthread_rwlock_t table_lock = PTHREAD_RWLOCK_INITIALIZER;
static ServiceGpuRange hook_map, hook_unmap, hook_invalidate;
static unsigned long long gpu_protects;
static uint64_t clock_start_ns;

/* First range with end > address. */
static size_t range_index(uintptr_t address) {
    size_t lo = 0, hi = range_count;
    while (lo < hi) {
        const size_t mid = (lo + hi) / 2;
        if (ranges[mid].end <= address) lo = mid + 1;
        else hi = mid;
    }
    return lo;
}

static void insert_at(size_t i, Range r) {
    if (range_count == range_capacity) {
        range_capacity = range_capacity ? range_capacity * 2 : 256;
        ranges = realloc(ranges, range_capacity * sizeof(Range));
        if (!ranges) abort();
    }
    memmove(ranges + i + 1, ranges + i, (range_count - i) * sizeof(Range));
    ranges[i] = r;
    ++range_count;
}

/* Removes [start, end) from the table, splitting ranges that stick out. Returns the index where
 * a range starting at `start` belongs. */
static size_t cut(uintptr_t start, uintptr_t end) {
    size_t i = range_index(start);
    if (i < range_count && ranges[i].start < start) {
        Range left = ranges[i];
        left.end = start;
        ranges[i].start = start;
        insert_at(i, left);
        ++i;
    }
    size_t j = i;
    while (j < range_count && ranges[j].end <= end) ++j;
    if (j < range_count && ranges[j].start < end) ranges[j].start = end;
    memmove(ranges + i, ranges + j, (range_count - j) * sizeof(Range));
    range_count -= j - i;
    return i;
}

void service_memory_mapped(uintptr_t address, uint64_t size, int prot) {
    pthread_rwlock_wrlock(&table_lock);
    const size_t i = cut(address, address + size);
    insert_at(i, (Range){address, address + size, prot});
    pthread_rwlock_unlock(&table_lock);
    if (hook_map) hook_map(address, size);
}

void service_memory_unmapped(uintptr_t address, uint64_t size) {
    pthread_rwlock_wrlock(&table_lock);
    cut(address, address + size);
    pthread_rwlock_unlock(&table_lock);
    if (hook_unmap) hook_unmap(address, size);
}

void service_memory_protected(uintptr_t address, uint64_t size, int prot) {
    const uintptr_t end = address + size;
    pthread_rwlock_wrlock(&table_lock);
    /* Only the mapped parts change; split at both ends first. */
    size_t i = range_index(address);
    if (i < range_count && ranges[i].start < address) {
        Range left = ranges[i];
        left.end = address;
        ranges[i].start = address;
        insert_at(i, left);
        ++i;
    }
    for (; i < range_count && ranges[i].start < end; ++i) {
        if (ranges[i].end > end) {
            Range right = ranges[i];
            right.start = end;
            ranges[i].end = end;
            insert_at(i + 1, right);
        }
        ranges[i].prot = prot;
    }
    pthread_rwlock_unlock(&table_lock);
    if (hook_invalidate) hook_invalidate(address, size);
}

size_t service_memory_range_count(void) {
    pthread_rwlock_rdlock(&table_lock);
    const size_t n = range_count;
    pthread_rwlock_unlock(&table_lock);
    return n;
}

void service_set_clock_start(uint64_t ns) { clock_start_ns = ns; }

/* ---- Called by the video core (declared in gpu/shim/spartanportgpu.cpp). */

void runtime_memory_set_gpu_hooks(ServiceGpuRange map, ServiceGpuRange unmap, ServiceGpuRange invalidate) {
    /* Existing mappings are replayed, as the x86 runtime does. */
    pthread_rwlock_rdlock(&table_lock);
    hook_map = map;
    hook_unmap = unmap;
    hook_invalidate = invalidate;
    const size_t n = range_count;
    Range *copy = malloc((n ? n : 1) * sizeof(Range));
    memcpy(copy, ranges, n * sizeof(Range));
    pthread_rwlock_unlock(&table_lock);
    for (size_t i = 0; i < n; ++i) map(copy[i].start, copy[i].end - copy[i].start);
    free(copy);
}

int runtime_memory_region(uintptr_t address, uintptr_t *start, uintptr_t *end, int *mapped) {
    pthread_rwlock_rdlock(&table_lock);
    const size_t i = range_index(address);
    const int found = i < range_count;
    if (found && ranges[i].start <= address) {
        *start = ranges[i].start;
        *end = ranges[i].end;
        *mapped = (ranges[i].prot & PROT_READ) != 0;
    } else if (found) {
        *start = i ? ranges[i - 1].end : 0;
        *end = ranges[i].start;
        *mapped = 0;
    }
    pthread_rwlock_unlock(&table_lock);
    return found;
}

uint64_t runtime_memory_clamp(uintptr_t address, uint64_t size) {
    pthread_rwlock_rdlock(&table_lock);
    uint64_t length = 0;
    for (size_t i = range_index(address); i < range_count && length < size; ++i) {
        if (ranges[i].start > address + length) break;
        length = ranges[i].end - address;
    }
    pthread_rwlock_unlock(&table_lock);
    return length < size ? length : size;
}

int runtime_memory_is_mapped(uintptr_t address, uint64_t size) {
    return runtime_memory_clamp(address, size) == size;
}

/* The x86 side writes through a second view to get past its page protection; here guest memory
 * is never protected, so the bytes are written in place. */
int runtime_memory_write_backing(uintptr_t address, const void *data, uint64_t size) {
    if (!runtime_memory_is_mapped(address, size)) return 0;
    memcpy((void *)address, data, size);
    return 1;
}

static ServiceProtect protect_sender;
void service_set_protect_sender(ServiceProtect sender) { protect_sender = sender; }

/* Page tracking (step 3.4): the guest runs the mprotect; this returns once it has. */
void runtime_memory_gpu_protect(uintptr_t address, uint64_t size, int read, int write) {
    __atomic_fetch_add(&gpu_protects, 1, __ATOMIC_RELAXED);
    if (protect_sender) protect_sender(address, size, read, write);
}

/* One clock with the guest: CLOCK_MONOTONIC from the guest's process start. */
static uint64_t now_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec - clock_start_ns;
}
uint64_t runtime_process_time_us(void) { return now_ns() / 1000; }
uint64_t runtime_process_time_counter(void) { return now_ns(); }
uint64_t runtime_read_tsc(void) { return now_ns(); }
uint64_t runtime_tsc_frequency(void) { return 1000000000ull; }

int32_t *runtime_errno(void) {
    static __thread int32_t value;
    return &value;
}

/* Bits of optimizations to switch off (src/runtime_memory.c); SPARTAN_TOGGLE_FILE is not
 * watched here yet. */
uint64_t runtime_disabled_optimizations;

/* Speculative readers of guest memory register a recovery point (vk_pipeline_cache.cpp). */
__thread sigjmp_buf *runtime_fault_recover;

void runtime_report(void) {
    fprintf(stderr, "ps4mac_service: %zu guest ranges, %llu page-tracking requests\n",
            service_memory_range_count(), gpu_protects);
}

void runtime_restart(void) {
    fputs("ps4mac_service: restart is the guest's (not forwarded yet)\n", stderr);
}

static void on_fault(int sig, siginfo_t *info, void *context) {
    (void)context;
    if (runtime_fault_recover) {
        sigjmp_buf *recover = runtime_fault_recover;
        runtime_fault_recover = NULL;
        sigset_t unblock;
        sigemptyset(&unblock);
        sigaddset(&unblock, sig);
        pthread_sigmask(SIG_UNBLOCK, &unblock, NULL);
        siglongjmp(*recover, 1);
    }
    char line[128];
    const int n = snprintf(line, sizeof(line), "ps4mac_service: fatal signal %d at %p\n", sig, info->si_addr);
    (void)!write(2, line, (size_t)n);
    /* The faulting thread's stack (the process is ending: not async-signal-safe, acceptable). */
    void *frames[48];
    const int depth = backtrace(frames, 48);
    backtrace_symbols_fd(frames, depth, 2);
    signal(sig, SIG_DFL);
    raise(sig);
}

void service_install_fault_handler(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = on_fault;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
}

/* The guest's bridge API (src/runtime_bridge.c) that the shared GPU library also references: in
 * the service the library is never a remote guest. */
struct Ps4macMessage;
int runtime_bridge_remote_graphics(void) { return 0; }
void runtime_bridge_send(const struct Ps4macMessage *m) { (void)m; }
void runtime_bridge_set_handler(void (*handler)(const struct Ps4macMessage *)) { (void)handler; }
int runtime_bridge_is_shared(uintptr_t address, uint64_t size) { (void)address; (void)size; return 0; }
int runtime_bridge_wait_graphics(void) { return 0; }
int runtime_bridge_fault(uintptr_t address, int is_write) { (void)address; (void)is_write; return 0; }
void runtime_bridge_set_vo_layout(uint32_t size, uint32_t labels, uint32_t groups, uint32_t slots) {
    (void)size; (void)labels; (void)groups; (void)slots;
}
void *runtime_low_map(size_t size, int prot) { (void)size; (void)prot; return NULL; }

