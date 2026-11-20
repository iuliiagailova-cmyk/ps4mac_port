/* SPDX-License-Identifier: GPL-2.0-or-later
 * ps4mac_port step 3.0: how fast can the guest side (x86-64 under Rosetta 2) and the graphics
 * service (native arm64) talk through shared memory?
 *
 * Build:  clang -O2 -arch x86_64 -o bench_x86 bridge_bench.c
 *         clang -O2 -arch arm64  -o bench_arm bridge_bench.c
 * Run:    ./bench_x86 client ./bench_arm      (the client starts the server)
 *
 * Two single-producer single-consumer rings in a POSIX shared memory object: requests
 * client -> server, replies server -> client. Modes:
 *   spin    both sides poll (best case latency)
 *   sleep   the server sleeps in os_sync_wait_on_address between requests (idle service)
 *   hybrid  the server polls up to 50 us, then sleeps
 *   stream  one-way messages without replies (throughput)
 */
#include <errno.h>
#include <fcntl.h>
#include <mach/mach_time.h>
#include <os/os_sync_wait_on_address.h>
#include <spawn.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#define SLOTS 1024u
#define SHM_NAME "/ps4mac_bridge_bench"

typedef struct {
    uint64_t kind, sequence, a, b;
} Message;

typedef struct {
    _Alignas(64) _Atomic uint64_t head;
    _Alignas(64) _Atomic uint64_t tail;
    _Alignas(64) _Atomic uint32_t sleeping; /* consumer asleep on head_word */
    _Atomic uint32_t head_word;             /* low 32 bits of head, the wait address */
    Message slots[SLOTS];
} Ring;

typedef struct {
    Ring requests, replies;
    _Atomic uint32_t mode; /* 0 spin, 1 sleep, 2 hybrid, 3 stream, 9 quit */
    _Atomic uint32_t ready;
} Shared;

static uint64_t now_ns(void) {
    static mach_timebase_info_data_t tb;
    if (!tb.denom) mach_timebase_info(&tb);
    return mach_absolute_time() * tb.numer / tb.denom;
}

static void push(Ring *r, const Message *m) {
    const uint64_t head = atomic_load_explicit(&r->head, memory_order_relaxed);
    while (head - atomic_load_explicit(&r->tail, memory_order_acquire) >= SLOTS) { }
    r->slots[head % SLOTS] = *m;
    atomic_store_explicit(&r->head, head + 1, memory_order_release);
    atomic_store_explicit(&r->head_word, (uint32_t)(head + 1), memory_order_release);
    if (atomic_load_explicit(&r->sleeping, memory_order_seq_cst))
        os_sync_wake_by_address_any(&r->head_word, 4, OS_SYNC_WAKE_BY_ADDRESS_SHARED);
}

/* Waits for a message; spin_ns < 0 spins forever, 0 sleeps at once. */
static Message pop(Ring *r, int64_t spin_ns) {
    const uint64_t tail = atomic_load_explicit(&r->tail, memory_order_relaxed);
    const uint64_t start = spin_ns > 0 ? now_ns() : 0;
    for (uint32_t n = 0;; ++n) {
        if (atomic_load_explicit(&r->head, memory_order_acquire) != tail) break;
        if (spin_ns < 0) continue;
        if (spin_ns > 0 && ((n & 63) || now_ns() - start < (uint64_t)spin_ns)) continue;
        atomic_store_explicit(&r->sleeping, 1, memory_order_seq_cst);
        if (atomic_load_explicit(&r->head, memory_order_seq_cst) == tail)
            os_sync_wait_on_address(&r->head_word, (uint32_t)tail, 4, OS_SYNC_WAIT_ON_ADDRESS_SHARED);
        atomic_store_explicit(&r->sleeping, 0, memory_order_relaxed);
    }
    const Message m = r->slots[tail % SLOTS];
    atomic_store_explicit(&r->tail, tail + 1, memory_order_release);
    return m;
}

static Shared *map_shared(int create) {
    int fd = shm_open(SHM_NAME, O_RDWR | (create ? O_CREAT : 0), 0600);
    if (fd < 0) { perror("shm_open"); exit(1); }
    if (create && ftruncate(fd, sizeof(Shared))) { perror("ftruncate"); exit(1); }
    Shared *s = mmap(NULL, sizeof(Shared), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (s == MAP_FAILED) { perror("mmap"); exit(1); }
    close(fd);
    return s;
}

static int server(void) {
    Shared *s = map_shared(0);
    atomic_store(&s->ready, 1);
    uint64_t streamed = 0;
    for (;;) {
        const uint32_t mode = atomic_load(&s->mode);
        const int64_t spin = mode == 0 ? -1 : mode == 1 ? 0 : 50000;
        Message m = pop(&s->requests, mode == 3 ? -1 : spin);
        if (m.kind == 9) break;
        if (m.kind == 3) { ++streamed; continue; }
        if (m.kind == 4) { m.a = streamed; streamed = 0; }
        push(&s->replies, &m);
    }
    return 0;
}

static int cmp(const void *a, const void *b) {
    const uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}

static void round_trips(Shared *s, uint32_t mode, const char *name, unsigned gap_us) {
    enum { N = 20000 };
    static uint64_t t[N];
    atomic_store(&s->mode, mode);
    for (unsigned i = 0; i < N; ++i) {
        if (gap_us) { const uint64_t until = now_ns() + gap_us * 1000ull; while (now_ns() < until) { } }
        Message m = {1, i, 0, 0};
        const uint64_t start = now_ns();
        push(&s->requests, &m);
        Message r = pop(&s->replies, -1);
        t[i] = now_ns() - start;
        if (r.sequence != i) { fprintf(stderr, "out of order\n"); exit(1); }
    }
    qsort(t, N, sizeof(*t), cmp);
    printf("%-34s median %6.1f us   p90 %6.1f us   p99 %7.1f us   max %8.1f us\n", name,
           t[N / 2] / 1e3, t[N * 9 / 10] / 1e3, t[N * 99 / 100] / 1e3, t[N - 1] / 1e3);
}

static int client(const char *server_path) {
    shm_unlink(SHM_NAME);
    Shared *s = map_shared(1);
    memset(s, 0, sizeof(*s));
    pid_t pid;
    char *argv[] = {(char *)server_path, "server", NULL};
    extern char **environ;
    if (posix_spawn(&pid, server_path, NULL, NULL, argv, environ)) { perror("spawn"); return 1; }
    while (!atomic_load(&s->ready)) usleep(1000);
    round_trips(s, 0, "round trip, both spin", 0);
    round_trips(s, 2, "round trip, server spin 50us+sleep", 0);
    round_trips(s, 2, "  same, requests 200 us apart", 200);
    round_trips(s, 1, "round trip, server sleeps", 0);
    round_trips(s, 1, "  same, requests 200 us apart", 200);
    /* Stream: one-way messages, the server counts them. */
    atomic_store(&s->mode, 3);
    enum { M = 2000000 };
    const uint64_t start = now_ns();
    for (unsigned i = 0; i < M; ++i) { Message m = {3, i, 0, 0}; push(&s->requests, &m); }
    Message q = {4, 0, 0, 0};
    push(&s->requests, &q);
    Message r = pop(&s->replies, -1);
    const double sec = (now_ns() - start) / 1e9;
    printf("stream: %llu messages in %.3f s = %.1f M messages/s (%.0f ns each)\n",
           (unsigned long long)r.a, sec, r.a / sec / 1e6, sec * 1e9 / r.a);
    Message quit = {9, 0, 0, 0};
    atomic_store(&s->mode, 0);
    push(&s->requests, &quit);
    waitpid(pid, NULL, 0);
    shm_unlink(SHM_NAME);
    return 0;
}

int main(int argc, char **argv) {
    if (argc >= 2 && !strcmp(argv[1], "server")) return server();
    if (argc >= 3 && !strcmp(argv[1], "client")) return client(argv[2]);
    fprintf(stderr, "usage: %s client <server binary> | server\n", argv[0]);
    return 2;
}
