/* SPDX-License-Identifier: GPL-2.0-or-later
 * ps4mac_port step 3.2: the guest side of the bridge to the native graphics service.
 *
 * PS4MAC_SERVICE=<path> starts the service at launch: a shared bridge block and the guest memory
 * objects (pool: direct + flexible memory; low: images and runtime allocations) go to it over a
 * Unix socket (SCM_RIGHTS); every pool mapping and unmapping follows as a ring message, so the
 * service maps the same bytes at the same addresses. PS4MAC_BRIDGE_CHECK=1 compares checksums of
 * guest memory on both sides every few seconds. Without PS4MAC_SERVICE nothing here runs.
 */
#ifdef __APPLE__
#include <errno.h>
#include <mach/mach.h>
#include <time.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/ucontext.h>
#include <sys/wait.h>
#include <unistd.h>

#include "runtime.h"
#include "../bridge/ps4mac_ring.h"

int runtime_memory_region(uintptr_t address, uintptr_t *start, uintptr_t *end, int *mapped);
void runtime_mutex_dump_trace(FILE *out, unsigned count);
int runtime_memory_write_backing(uintptr_t address, const void *data, uint64_t size);
void runtime_memory_gpu_protect(uintptr_t address, uint64_t size, int read, int write);

static Ps4macSharedBlock *block;
static pid_t service_pid;
static pthread_mutex_t push_lock = PTHREAD_MUTEX_INITIALIZER; /* the ring has one producer */

/* Diagnostic (SPARTAN_LATENCY=<file.csv>): a timeline of the frame's critical path on the guest
 * side, "ns,kind,value" lines on CLOCK_MONOTONIC_RAW (the service's SpartanTrace JSON gives its
 * window start on the same clock). Kinds: S guest message sent (value: its kind),
 * A the address the main thread waits on (when it changes), I interrupt received (value: its id), F a page fault answered (value: wait ns * 2 + write), M the game's main thread changed state (value: 0 other,
 * 1 in the SPARTAN_LAT_WAIT rip range, 2 in SPARTAN_LAT_PAUSE; ranges "lo-hi", hex), R the render
 * thread (the first to submit graphics work) entered (1) or left (0) SPARTAN_LAT_IMAGE. The main
 * thread is sampled every SPARTAN_LAT_US microseconds (default 200) with thread_get_state; its
 * memory is not read (a read of a GPU-protected page would fault and change the timing).
 * SPARTAN_LAT_RIPS=1 adds every sample's rip: G the main thread's, Q the render thread's (in the image). */
static FILE *lat_file;
static mach_port_t game_thread_port; /* the game's main thread (runtime_bridge_note_game_thread) */
static mach_port_t render_thread_port; /* the first thread to submit graphics work */
static pthread_mutex_t lat_lock = PTHREAD_MUTEX_INITIALIZER;
static void lat_event(char kind, uint64_t value) {
    if (!lat_file) return;
    /* The clock of the service's std::chrono::steady_clock (ps4mac_now_ns leaves out sleep). */
    const uint64_t now = clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW);
    pthread_mutex_lock(&lat_lock);
    fprintf(lat_file, "%llu,%c,%llu\n", (unsigned long long)now, kind, (unsigned long long)value);
    pthread_mutex_unlock(&lat_lock);
}

static _Atomic uint64_t messages_sent;
static _Atomic int gfx_submitted; /* SPARTAN_HANG_DUMP=2 */
static void send_message(const Ps4macMessage *m) {
    atomic_fetch_add_explicit(&messages_sent, 1, memory_order_relaxed);
    if (m->kind == PS4MAC_SUBMIT_GFX) atomic_store_explicit(&gfx_submitted, 1, memory_order_relaxed);
    if (lat_file && m->kind == PS4MAC_SUBMIT_GFX && !render_thread_port)
        render_thread_port = mach_thread_self(); /* the thread that submits: the game's render thread */
    if (lat_file && m->kind != PS4MAC_MEMORY_MAP && m->kind != PS4MAC_MEMORY_UNMAP &&
        m->kind != PS4MAC_MEMORY_PROTECT)
        lat_event('S', m->kind);
    pthread_mutex_lock(&push_lock);
    if (ps4mac_ring_push(&block->to_graphics, m, 2000000000ull))
        fputs("Bridge: the graphics service does not take messages\n", stderr);
    pthread_mutex_unlock(&push_lock);
}

static void observe_mapping(uintptr_t address, uint64_t size, int kind, uint64_t phys, int prot) {
    if (!block || kind == 1) return; /* the low object is mapped whole by the service */
    const Ps4macMessage m = {
        .kind = kind == 2 ? PS4MAC_MEMORY_UNMAP : kind == 3 ? PS4MAC_MEMORY_PROTECT : PS4MAC_MEMORY_MAP,
        .a = address, .b = size, .c = phys, .d = (uint64_t)prot,
    };
    send_message(&m);
}

int runtime_bridge_gpu_init(const char *title, const char *serial, const char *user_dir, uint32_t sdk_version,
                            uint32_t psf_attributes, int32_t width, int32_t height) {
    if (!block) return 0;
    Ps4macGpuConfig *c = &block->gpu_config;
    snprintf(c->title, sizeof(c->title), "%s", title ? title : "");
    snprintf(c->serial, sizeof(c->serial), "%s", serial ? serial : "");
    snprintf(c->user_dir, sizeof(c->user_dir), "%s", user_dir ? user_dir : "");
    c->sdk_version = sdk_version;
    c->psf_attributes = psf_attributes;
    c->width = width;
    c->height = height;
    const Ps4macMessage m = {.kind = PS4MAC_GPU_INIT};
    send_message(&m); /* the ring's release store publishes the configuration */
    return 1;
}

/* Sends checksums of a few guest ranges; the replies are compared by the reply thread. */
static void *check_thread(void *unused) {
    (void)unused;
    for (unsigned round = 0;; ++round) {
        sleep(5);
        uintptr_t start, end;
        int mapped;
        unsigned sent = 0;
        /* Up to 8 mapped ranges of guest memory, 64 KiB each, from a walk through the user range. */
        for (uintptr_t at = block->user_min + (round % 64) * (UINT64_C(1) << 28); at < block->user_max && sent < 8;) {
            if (!runtime_memory_region(at, &start, &end, &mapped)) break;
            if (mapped && end - start >= 0x10000) {
                const uint64_t size = 0x10000;
                const Ps4macMessage m = {.kind = PS4MAC_CHECKSUM, .a = start, .b = size,
                                         .c = ps4mac_hash((const void *)start, size)};
                send_message(&m);
                ++sent;
            }
            at = end > at ? end : at + 0x10000;
        }
        /* And the start of low memory (the game's image). */
        const Ps4macMessage low = {.kind = PS4MAC_CHECKSUM, .a = block->low_base, .b = 0x10000,
                                   .c = ps4mac_hash((const void *)block->low_base, 0x10000)};
        send_message(&low);
    }
    return NULL;
}

static void (*message_handler)(const Ps4macMessage *);
static int remote_graphics;

/* For the GPU library's Ps4mac::Gfx client (gpu/shim/ps4mac_graphics.cpp). */
int runtime_bridge_remote_graphics(void) { return remote_graphics; }
/* Step 3.5: the service's window input (NULL in one process). */
Ps4macInput *runtime_bridge_input(void) { return remote_graphics && block ? &block->input : NULL; }
void runtime_bridge_send(const Ps4macMessage *m) { send_message(m); }
void runtime_bridge_set_handler(void (*handler)(const Ps4macMessage *)) { message_handler = handler; }
/* The guest's VideoOutPort layout, for the service's check (both builds must agree). */
void runtime_bridge_set_vo_layout(uint32_t size, uint32_t labels, uint32_t groups, uint32_t slots) {
    block->vo_port_size = size;
    block->vo_labels_offset = labels;
    block->vo_groups_offset = groups;
    block->vo_slots_offset = slots;
}
/* Waits until the service's video core answers; returns whether HDR output is supported. */
int runtime_bridge_wait_graphics(void) {
    while (!atomic_load(&block->gfx_ready)) {
        if (!atomic_load(&block->service_alive)) {
            fputs("STOP: the graphics service ended before its video core started\n", stderr);
            exit(21);
        }
        usleep(1000);
    }
    return (int)block->gfx_hdr_supported;
}
/* Memory the service maps too: the PS4 range (pool) and low memory. */
int runtime_bridge_is_shared(uintptr_t address, uint64_t size) {
    return block && address >= block->user_min && address + size <= block->user_max;
}

/* Step 3.4: a fault on guest memory, for the service's page tracking (the GPU library's fault
 * handler, gpu/shim/spartanportgpu.cpp). Returns whether it was a page the GPU protects; the
 * service has then changed the protection, and the access can be retried. */
static uint32_t fault_sequence;
static __thread ucontext_t *fault_uc; /* the faulting thread's registers (signal context) */
void runtime_bridge_fault_context(void *ucontext) { fault_uc = (ucontext_t *)ucontext; }

/* A simple store at the fault's address, decoded from the faulting instruction: MOV r/m, reg and
 * MOV r/m, imm (8 to 64 bits), the common forms of a counter or flag update. The service may let
 * the guest perform it itself (result 2): the page stays protected (BufferCache::StoreMayBypass). */
typedef struct { uint64_t value; unsigned size, length; } GuestStore;
static uint64_t uc_gpr(const ucontext_t *uc, unsigned n) {
    const _STRUCT_X86_THREAD_STATE64 *r = &uc->uc_mcontext->__ss;
    switch (n) {
    case 0: return r->__rax; case 1: return r->__rcx; case 2: return r->__rdx; case 3: return r->__rbx;
    case 4: return r->__rsp; case 5: return r->__rbp; case 6: return r->__rsi; case 7: return r->__rdi;
    case 8: return r->__r8; case 9: return r->__r9; case 10: return r->__r10; case 11: return r->__r11;
    case 12: return r->__r12; case 13: return r->__r13; case 14: return r->__r14; default: return r->__r15;
    }
}
static int decode_store(const ucontext_t *uc, uintptr_t address, GuestStore *out) {
    const uint8_t *ip = (const uint8_t *)(uintptr_t)uc->uc_mcontext->__ss.__rip;
    unsigned i = 0, op16 = 0, rex = 0;
    for (;; ++i) {
        if (i > 4) return 0;
        if (ip[i] == 0x66) op16 = 1;
        else if (ip[i] == 0x2e || ip[i] == 0x3e || ip[i] == 0x26 || ip[i] == 0x36) continue;
        else break; /* fs/gs (0x64/0x65), lock, rep: not handled */
    }
    if ((ip[i] & 0xf0) == 0x40) rex = ip[i++];
    const uint8_t op = ip[i++];
    unsigned size;
    if (op == 0x88 || op == 0xc6) size = 1;
    else if (op == 0x89 || op == 0xc7) size = (rex & 8) ? 8 : op16 ? 2 : 4;
    else return 0;
    const uint8_t modrm = ip[i++];
    const unsigned mod = modrm >> 6, reg = ((modrm >> 3) & 7) | ((rex & 4) << 1), rm = modrm & 7;
    if (mod == 3) return 0;
    if ((op == 0xc6 || op == 0xc7) && ((modrm >> 3) & 7) != 0) return 0;
    uint64_t ea = 0;
    int rip_relative = 0;
    if (rm == 4) {
        const uint8_t sib = ip[i++];
        const unsigned index = ((sib >> 3) & 7) | ((rex & 2) << 2), base = (sib & 7) | ((rex & 1) << 3);
        if (index != 4) ea += uc_gpr(uc, index) << (sib >> 6);
        if ((sib & 7) == 5 && mod == 0) { int32_t d; memcpy(&d, ip + i, 4); i += 4; ea += (uint64_t)(int64_t)d; }
        else ea += uc_gpr(uc, base);
    } else if (rm == 5 && mod == 0) {
        rip_relative = 1;
    } else {
        ea += uc_gpr(uc, rm | ((rex & 1) << 3));
    }
    if (mod == 1) { ea += (uint64_t)(int64_t)(int8_t)ip[i]; i += 1; }
    else if (mod == 2 || rip_relative) { int32_t d; memcpy(&d, ip + i, 4); i += 4; ea += (uint64_t)(int64_t)d; }
    uint64_t value;
    if (op == 0x88) {
        if (!rex && reg >= 4) return 0; /* AH..BH */
        value = uc_gpr(uc, reg) & 0xff;
    } else if (op == 0x89) {
        value = uc_gpr(uc, reg);
    } else if (op == 0xc6) {
        value = ip[i++];
    } else if (size == 2) {
        uint16_t imm; memcpy(&imm, ip + i, 2); i += 2; value = imm;
    } else {
        int32_t imm; memcpy(&imm, ip + i, 4); i += 4; value = (uint64_t)(int64_t)imm;
    }
    if (rip_relative) ea += (uint64_t)(uintptr_t)ip + i;
    if (ea != address) return 0; /* not the access that faulted */
    out->value = value;
    out->size = size;
    out->length = i;
    return 1;
}

int runtime_bridge_fault(uintptr_t address, int is_write) {
    if (!block || !remote_graphics || !runtime_bridge_is_shared(address, 1)) return 0;
    pthread_mutex_lock(&push_lock); /* sequences in ring order: the service answers in order */
    const uint32_t sequence = ++fault_sequence;
    static int store_bypass = -1;
    if (store_bypass < 0) {
        const char *env = getenv("SPARTAN_STORE_BYPASS");
        store_bypass = env && *env == '1'; /* off by default (BufferCache::StoreMayBypass) */
    }
    GuestStore store = {0};
    const int can_store = is_write && store_bypass && fault_uc && decode_store(fault_uc, address, &store) &&
                          (address & 4095) + store.size <= 4096;
    const Ps4macMessage m = {.kind = is_write ? PS4MAC_WRITE_FAULT : PS4MAC_READ_FAULT,
                             .sequence = sequence, .a = address, .b = can_store ? store.size : 0};
    const int pushed = !ps4mac_ring_push(&block->to_graphics, &m, 2000000000ull);
    pthread_mutex_unlock(&push_lock);
    if (!pushed) return 0;
    const uint64_t start = ps4mac_now_ns();
    ps4mac_counter_wait(&block->fault_done, sequence, 200000);
    atomic_fetch_add_explicit(&block->stat_fault_wait_ns, ps4mac_now_ns() - start, memory_order_relaxed);
    if (lat_file) {
        const uint64_t waited = ps4mac_now_ns() - start;
        lat_event('F', waited * 2 + (uint64_t)(is_write != 0)); /* at its end */
        if (waited > 2000000) { /* a slow one: where, from which instruction, on the main thread? */
            lat_event('X', address);
            lat_event('P', fault_uc ? fault_uc->uc_mcontext->__ss.__rip : 0);
            lat_event('T', mach_thread_self() == game_thread_port);
        }
    }
    const int result = block->fault_result[sequence % 256];
    if (result == 2 && can_store) {
        /* The service left the store to us: the page stays protected, the GPU's bytes on it too. */
        runtime_memory_write_backing(address, &store.value, store.size);
        fault_uc->uc_mcontext->__ss.__rip += store.length;
        static _Atomic uint64_t bypassed;
        const uint64_t n = atomic_fetch_add_explicit(&bypassed, 1, memory_order_relaxed) + 1;
        if ((n & (n - 1)) == 0 && n >= 64)
            fprintf(stderr, "Bridge: %llu guest stores performed without the GPU's download\n",
                    (unsigned long long)n);
        return 1;
    }
    return result != 0;
}

static void *reply_thread(void *unused) {
    (void)unused;
    unsigned ok = 0, bad = 0;
    uint64_t last_report = ps4mac_now_ns();
    const int stats = getenv("SPARTAN_FRAME_STATS") != NULL;
    uint64_t last_stats = ps4mac_now_ns(), last[6] = {0};
    for (;;) {
        if (stats && remote_graphics && ps4mac_now_ns() - last_stats >= 5000000000ull) {
            const uint64_t now[6] = {atomic_load(&block->stat_protects), atomic_load(&block->stat_protect_wait_ns),
                                     atomic_load(&block->stat_faults), atomic_load(&block->stat_fault_handle_ns),
                                     atomic_load(&block->stat_fault_wait_ns), atomic_load(&block->stat_fault_drain_ns)};
            const double s = (ps4mac_now_ns() - last_stats) / 1e9;
            const double protects = (double)(now[0] - last[0]), faults = (double)(now[2] - last[2]);
            printf("Bridge cost: %.0f protects/s (service waits %.1f ms/s, %.1f us each); %.0f faults/s "
                   "(service handles %.1f ms/s, of it faults on GPU-written pages %.1f ms/s; guest waits %.1f ms/s, %.1f us each)\n",
                   protects / s, (now[1] - last[1]) / 1e6 / s, protects ? (now[1] - last[1]) / 1e3 / protects : 0.0,
                   faults / s, (now[3] - last[3]) / 1e6 / s, (now[5] - last[5]) / 1e6 / s, (now[4] - last[4]) / 1e6 / s,
                   faults ? (now[4] - last[4]) / 1e3 / faults : 0.0);
            fflush(stdout);
            memcpy(last, now, sizeof(last));
            last_stats = ps4mac_now_ns();
        }
        Ps4macMessage m;
        if (!ps4mac_ring_pop(&block->to_guest, &m, 50000, 1000000000ull)) {
            if (!atomic_load(&block->service_alive)) return NULL;
            continue;
        }
        if (m.kind == PS4MAC_IRQ) lat_event('I', m.a);
        if (m.kind == PS4MAC_PROTECT) {
            /* The service's page tracking: synchronous for it (it waits for protect_done). */
            runtime_memory_gpu_protect((uintptr_t)m.a, m.b, (int)(m.c & 1), (int)((m.c >> 1) & 1));
            ps4mac_counter_set(&block->protect_done, (uint32_t)m.sequence);
            continue;
        }
        if (m.kind != PS4MAC_CHECKSUM_REPLY) {
            if (message_handler) message_handler(&m);
            continue;
        }
        {
            if (m.c == m.d) ++ok;
            else {
                ++bad;
                fprintf(stderr, "Bridge: checksum differs at %#llx (+%#llx): guest %#llx, service %#llx\n",
                        (unsigned long long)m.a, (unsigned long long)m.b, (unsigned long long)m.c,
                        (unsigned long long)m.d);
            }
        }
        if (ps4mac_now_ns() - last_report > 5000000000ull && ok + bad) {
            printf("Bridge: checksums equal %u, different %u\n", ok, bad);
            ok = bad = 0;
            last_report = ps4mac_now_ns();
        }
    }
}

/* Diagnostic (SPARTAN_HANG_DUMP=1): when the guest sends the service nothing for 3 s after it
 * started, the game's main thread state, once: R&C's main thread spins at a GPU result
 * (rip 0x17fae80..0x17fae93 of the image: while (((float*)r15)[r13] == -1.0f) pause). */
void runtime_bridge_note_game_thread(void) { game_thread_port = mach_thread_self(); }
/* SPARTAN_HANG_DUMP=2 also samples the main thread with thread_get_state every 200 us until the
 * first graphics submission: that made the rare start hang (before any submission) frequent. */
static void *start_stress_thread(void *unused) {
    (void)unused;
    while (!atomic_load(&gfx_submitted)) {
        struct timespec ts = {0, 200000}; nanosleep(&ts, NULL);
        if (!game_thread_port) continue;
        x86_thread_state64_t st; mach_msg_type_number_t n = x86_THREAD_STATE64_COUNT;
        thread_get_state(game_thread_port, x86_THREAD_STATE64, (thread_state_t)&st, &n);
    }
    return NULL;
}
static void *hang_thread(void *unused) {
    (void)unused;
    uint64_t last = 0; int quiet = 0, dumped = 0;
    for (;;) {
        struct timespec ts = {1, 0}; nanosleep(&ts, NULL);
        const uint64_t now = atomic_load_explicit(&messages_sent, memory_order_relaxed);
        quiet = (now == last && now > 0) ? quiet + 1 : 0; /* start hangs send ~20 messages */
        last = now;
        if (quiet < 3 || dumped || !game_thread_port) continue;
        dumped = 1;
        x86_thread_state64_t st; mach_msg_type_number_t n = x86_THREAD_STATE64_COUNT;
        if (thread_get_state(game_thread_port, x86_THREAD_STATE64, (thread_state_t)&st, &n) != KERN_SUCCESS) {
            fputs("Hang dump: thread_get_state failed\n", stderr);
            continue;
        }
        fprintf(stderr, "Hang dump: game main thread rip=%#llx rsp=%#llx rbx=%#llx r12=%#llx r13=%#llx r14=%#llx r15=%#llx\n",
                (unsigned long long)st.__rip, (unsigned long long)st.__rsp, (unsigned long long)st.__rbx,
                (unsigned long long)st.__r12, (unsigned long long)st.__r13, (unsigned long long)st.__r14,
                (unsigned long long)st.__r15);
        /* SPARTAN_HANG_PEEK=<hex address>[,...]: 32-bit values the game keeps there (e.g. a status) */
        for (const char *peek = getenv("SPARTAN_HANG_PEEK"); peek && *peek;) {
            char *end;
            const uintptr_t address = (uintptr_t)strtoull(peek, &end, 16);
            if (end == peek) break;
            fprintf(stderr, "Hang dump: [%#llx] = %#x\n", (unsigned long long)address,
                    *(volatile uint32_t *)address);
            peek = *end == ',' ? end + 1 : end;
        }
        runtime_mutex_dump_trace(stderr, 400);
        if (st.__r15 > 0x10000 && st.__r13 < (1u << 26)) {
            const float *f = (const float *)(uintptr_t)st.__r15;
            fprintf(stderr, "Hang dump: wait address %#llx (r15 + 4*r13) holds %f; r15 base %#llx\n",
                    (unsigned long long)(st.__r15 + 4 * st.__r13), (double)f[st.__r13],
                    (unsigned long long)st.__r15);
            const Ps4macMessage diag = {.kind = PS4MAC_DIAG_ADDRESS, .a = st.__r15 + 4 * st.__r13};
            send_message(&diag);
        }
        fflush(stderr);
    }
    return NULL;
}

static int lat_range(const char *env, uint64_t *lo, uint64_t *hi) {
    const char *v = getenv(env);
    return v && sscanf(v, "%llx-%llx", (unsigned long long *)lo, (unsigned long long *)hi) == 2;
}
static void *lat_thread(void *unused) {
    (void)unused;
    uint64_t wait_lo = 0, wait_hi = 0, pause_lo = 0, pause_hi = 0;
    lat_range("SPARTAN_LAT_WAIT", &wait_lo, &wait_hi);
    lat_range("SPARTAN_LAT_PAUSE", &pause_lo, &pause_hi);
    const char *rips_env = getenv("SPARTAN_LAT_RIPS");
    const int log_rips = rips_env && *rips_env == '1';
    uint64_t image_lo = 0, image_hi = 0; /* the game's code, for the render thread's state */
    lat_range("SPARTAN_LAT_IMAGE", &image_lo, &image_hi);
    const char *us_env = getenv("SPARTAN_LAT_US");
    const long us = us_env ? atol(us_env) : 200;
    int last = -1;
    uint64_t last_flush = 0;
    for (;;) {
        struct timespec ts = {0, us * 1000}; nanosleep(&ts, NULL);
        /* Not before the game submits graphics work: sampled during its start, the game hung
         * there in 3 of 4 runs (before any submission, the known rare black screen otherwise). */
        if (!game_thread_port || !render_thread_port) continue;
        x86_thread_state64_t st; mach_msg_type_number_t n = x86_THREAD_STATE64_COUNT;
        if (thread_get_state(game_thread_port, x86_THREAD_STATE64, (thread_state_t)&st, &n) != KERN_SUCCESS)
            continue;
        const int state = (st.__rip >= wait_lo && st.__rip < wait_hi) ? 1
                        : (st.__rip >= pause_lo && st.__rip < pause_hi) ? 2 : 0;
        if (log_rips) {
            lat_event('G', st.__rip); /* SPARTAN_LAT_RIPS=1: the main thread's rip too */
            /* In SPARTAN_LAT_PAUSE (the game's spin-wait): its caller, the first return address
             * into the image among the top stack words (the stack is never GPU-protected). */
            if (state == 2 && st.__rsp) {
                const uint64_t *sp = (const uint64_t *)(uintptr_t)st.__rsp;
                for (int k = 0; k < 24; ++k)
                    if (sp[k] >= image_lo && sp[k] < image_hi) { lat_event('H', sp[k]); break; }
            }
        }
        if (state != last) lat_event('M', (uint64_t)state);
        if (state == 1 && last != 1) { /* the address it waits on (R&C: r15 + 4 * r13), when it changes */
            static uint64_t last_address;
            const uint64_t address = st.__r15 + 4 * st.__r13;
            if (address != last_address) lat_event('A', address);
            last_address = address;
        }
        last = state;
        if (render_thread_port) {
            /* The render thread: in the game's code (1) or not (0: the runtime, its waits). */
            static int last_render = -1;
            x86_thread_state64_t rs; mach_msg_type_number_t rn = x86_THREAD_STATE64_COUNT;
            if (thread_get_state(render_thread_port, x86_THREAD_STATE64, (thread_state_t)&rs, &rn) == KERN_SUCCESS) {
                const int in_game = rs.__rip >= image_lo && rs.__rip < image_hi;
                if (log_rips && in_game) lat_event('Q', rs.__rip); /* SPARTAN_LAT_RIPS=1: where */
                if (log_rips && rs.__rip >= pause_lo && rs.__rip < pause_hi && rs.__rsp) {
                    const uint64_t *sp = (const uint64_t *)(uintptr_t)rs.__rsp;
                    for (int k = 0; k < 24; ++k)
                        if (sp[k] >= image_lo && sp[k] < image_hi) { lat_event('K', sp[k]); break; }
                }
                if (in_game != last_render) lat_event('R', (uint64_t)in_game);
                last_render = in_game;
            }
        }
        const uint64_t now = ps4mac_now_ns();
        if (now - last_flush > 100000000ull) {
            pthread_mutex_lock(&lat_lock);
            fflush(lat_file);
            pthread_mutex_unlock(&lat_lock);
            last_flush = now;
        }
    }
    return NULL;
}

static void *watch_thread(void *unused) {
    (void)unused;
    int status = 0;
    waitpid(service_pid, &status, 0);
    atomic_store(&block->service_alive, 0);
    if (WIFSIGNALED(status))
        fprintf(stderr, "STOP: the graphics service was killed by signal %d\n", WTERMSIG(status));
    else
        fprintf(stderr, "Bridge: the graphics service exited (status %d)\n", WEXITSTATUS(status));
    fflush(stdout);
    _exit(WIFSIGNALED(status) ? 31 : 0);
    return NULL;
}

/* The service's environment: the guest's, except two variables its libraries read at load.
 * VK_DRIVER_FILES names the arm64 KosmicKrisp (PS4MAC_SERVICE_ICD, else vulkan/ next to the
 * service, else the arm64 dependency prefix); SPARTAN_GPU_USER_DIR gets an arm64/ folder so the
 * two video cores (step 3.3b) never share pipeline caches. */
static char **service_environment(const char *service) {
    extern char **environ;
    static char icd[1024], user[1024];
    const char *icd_env = getenv("PS4MAC_SERVICE_ICD");
    if (icd_env && *icd_env) snprintf(icd, sizeof(icd), "VK_DRIVER_FILES=%s", icd_env);
    else {
        char dir[1024];
        snprintf(dir, sizeof(dir), "%s", service);
        char *slash = strrchr(dir, '/');
        if (slash) *slash = 0; else snprintf(dir, sizeof(dir), ".");
        char path[1024];
        snprintf(path, sizeof(path), "%s/vulkan/kosmickrisp_mesa_icd.json", dir);
        if (access(path, R_OK)) {
            const char *home = getenv("HOME");
            snprintf(path, sizeof(path), "%s/Library/Caches/ps4mac-arm64-deps/share/vulkan/icd.d/kosmickrisp_mesa_icd.json",
                     home ? home : "");
        }
        snprintf(icd, sizeof(icd), "VK_DRIVER_FILES=%s", path);
    }
    const char *user_env = getenv("SPARTAN_GPU_USER_DIR");
    char user_dir[900];
    snprintf(user_dir, sizeof(user_dir), "%s/arm64", user_env && *user_env ? user_env : "user");
    mkdir(user_dir, 0755); /* the library falls back to another folder when it does not exist */
    snprintf(user, sizeof(user), "SPARTAN_GPU_USER_DIR=%s", user_dir);
    size_t n = 0;
    while (environ[n]) ++n;
    char **env = calloc(n + 3, sizeof(char *));
    size_t k = 0;
    for (size_t i = 0; i < n; ++i)
        if (strncmp(environ[i], "VK_DRIVER_FILES=", 16) && strncmp(environ[i], "VK_ICD_FILENAMES=", 17) &&
            strncmp(environ[i], "SPARTAN_GPU_USER_DIR=", 21))
            env[k++] = environ[i];
    env[k++] = icd;
    env[k++] = user;
    env[k] = NULL;
    return env;
}

void runtime_bridge_start(void) {
    const char *service = getenv("PS4MAC_SERVICE");
    if (!service || !*service) return;
    char name[64];
    snprintf(name, sizeof(name), "/ps4mac-bridge-%d", (int)getpid());
    const int block_fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
    if (block_fd < 0 || (shm_unlink(name), ftruncate(block_fd, sizeof(Ps4macSharedBlock)))) {
        fprintf(stderr, "STOP: cannot create the bridge block: %s\n", strerror(errno));
        exit(21);
    }
    block = mmap(NULL, sizeof(Ps4macSharedBlock), PROT_READ | PROT_WRITE, MAP_SHARED, block_fd, 0);
    if (block == MAP_FAILED) { perror("bridge mmap"); exit(21); }
    int pool_fd, low_fd;
    runtime_memory_share(&pool_fd, &block->pool_size, &low_fd, &block->low_base, &block->low_size,
                         &block->user_min, &block->user_max);
    block->version = PS4MAC_BRIDGE_VERSION;
    block->clock_start_ns = runtime_process_start_ns();
    block->page_size = (uint32_t)getpagesize();
    atomic_store(&block->guest_alive, 1);

    int sockets[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets)) { perror("socketpair"); exit(21); }
    /* Only the service's end goes to it (dup2 to fd 3): with ours too it would never see EOF. */
    fcntl(sockets[0], F_SETFD, FD_CLOEXEC);
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, sockets[1], 3);
    char *argv[] = {(char *)service, "--socket", "3", NULL};
    char **env = service_environment(service);
    if (posix_spawn(&service_pid, service, &actions, NULL, argv, env)) {
        fprintf(stderr, "STOP: cannot start the graphics service %s: %s\n", service, strerror(errno));
        exit(21);
    }
    posix_spawn_file_actions_destroy(&actions);
    close(sockets[1]);

    /* The three objects, in this order: bridge block, pool, low memory. */
    int fds[3] = {block_fd, pool_fd, low_fd};
    char control[CMSG_SPACE(sizeof(fds))];
    memset(control, 0, sizeof(control));
    char tag = 'B';
    struct iovec iov = {&tag, 1};
    struct msghdr msg = {.msg_iov = &iov, .msg_iovlen = 1, .msg_control = control,
                         .msg_controllen = sizeof(control)};
    struct cmsghdr *c = CMSG_FIRSTHDR(&msg);
    c->cmsg_level = SOL_SOCKET;
    c->cmsg_type = SCM_RIGHTS;
    c->cmsg_len = CMSG_LEN(sizeof(fds));
    memcpy(CMSG_DATA(c), fds, sizeof(fds));
    if (sendmsg(sockets[0], &msg, 0) != 1) { perror("bridge sendmsg"); exit(21); }
    /* The service answers once guest memory is mapped. */
    char ready = 0;
    if (read(sockets[0], &ready, 1) != 1 || ready != 'R') {
        fputs("STOP: the graphics service did not start\n", stderr);
        exit(21);
    }
    atomic_store(&block->service_alive, 1);
    printf("Bridge: graphics service %s started (pid %d); guest memory shared\n", service, (int)service_pid);
    fflush(stdout);
    runtime_memory_set_mapping_observer(observe_mapping);

    pthread_t thread;
    pthread_create(&thread, NULL, watch_thread, NULL);
    {
        const char *hang = getenv("SPARTAN_HANG_DUMP");
        if (hang && *hang == '1') {
            pthread_t hang_watcher;
            pthread_create(&hang_watcher, NULL, hang_thread, NULL);
            pthread_detach(hang_watcher);
        } else if (hang && *hang == '2') {
            pthread_t hang_watcher, stress;
            pthread_create(&hang_watcher, NULL, hang_thread, NULL);
            pthread_detach(hang_watcher);
            pthread_create(&stress, NULL, start_stress_thread, NULL);
            pthread_detach(stress);
        }
    }
    {
        const char *lat = getenv("SPARTAN_LATENCY");
        if (lat && *lat && (lat_file = fopen(lat, "w"))) {
            pthread_t sampler;
            pthread_create(&sampler, NULL, lat_thread, NULL);
            pthread_detach(sampler);
            printf("Latency: guest timeline to %s\n", lat);
        }
    }
    pthread_detach(thread);
    pthread_create(&thread, NULL, reply_thread, NULL);
    pthread_detach(thread);
    /* Step 3.3c: the service renders (the guest's Ps4mac::Gfx sends to it) unless
     * PS4MAC_SERVICE_VIDEO=0 (the guest renders, the service only maps memory: step 3.2). */
    const char *video = getenv("PS4MAC_SERVICE_VIDEO");
    remote_graphics = !(video && *video == '0');
    const char *check = getenv("PS4MAC_BRIDGE_CHECK");
    if (check && *check == '1') {
        pthread_create(&thread, NULL, check_thread, NULL);
        pthread_detach(thread);
    }
}
#else
void runtime_bridge_start(void) {}
int runtime_bridge_remote_graphics(void) { return 0; }
struct Ps4macInput *runtime_bridge_input(void) { return NULL; }
void runtime_bridge_note_game_thread(void) {}
int runtime_bridge_fault(uintptr_t address, int is_write) { (void)address; (void)is_write; return 0; }
int runtime_bridge_gpu_init(const char *title, const char *serial, const char *user_dir, uint32_t sdk_version,
                            uint32_t psf_attributes, int32_t width, int32_t height) {
    (void)title; (void)serial; (void)user_dir; (void)sdk_version; (void)psf_attributes; (void)width; (void)height;
    return 0;
}
#endif
