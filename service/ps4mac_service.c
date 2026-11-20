/* SPDX-License-Identifier: GPL-2.0-or-later
 * ps4mac_port: the native arm64 graphics service.
 *
 * Started by the guest side (src/runtime_bridge.c) with a Unix socket on fd 3. Receives the bridge
 * block and the guest memory objects, reserves the PS4 user range and maps guest memory at the
 * same addresses as the guest: the low object whole, pool ranges as the guest maps them (ring
 * messages, which also keep the service's mapping table, service_runtime.c). Answers checksum
 * requests. Exits when the guest side goes away (socket closed).
 *
 * The video core (libspartanportgpu, arm64) is linked in and starts on PS4MAC_GPU_INIT (window,
 * Vulkan on KosmicKrisp); the guest's Ps4mac::Gfx calls arrive on the ring and are answered by
 * gpu/shim/ps4mac_graphics_server.cpp (step 3.3c). PS4MAC_SERVICE_VIDEO=0: no video core here
 * (the guest renders itself; step 3.2).
 */
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>

#include "../bridge/ps4mac_ring.h"
#include "../gpu/spartanportgpu.h"
#include "service_runtime.h"

static Ps4macSharedBlock *block;
static int pool_fd = -1, low_fd = -1;
static unsigned long long maps, unmaps, protects, checks, faults;
static pthread_mutex_t gpu_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t gpu_cond = PTHREAD_COND_INITIALIZER;
static int gpu_requested;
static _Atomic int gfx_ready;
static pthread_mutex_t to_guest_lock = PTHREAD_MUTEX_INITIALIZER; /* the ring has one producer */

int spartanportgpu_service_init(const SpartanGpuConfig *config, void (*reply)(const Ps4macMessage *));
int ps4mac_gfx_server_handle(const Ps4macMessage *m);
int ps4mac_gfx_server_check_port(uint32_t size, uint32_t labels, uint32_t groups, uint32_t slots);
int ps4mac_gfx_server_hdr_supported(void);
int ps4mac_gfx_server_fault(uintptr_t address, int is_write);

/* From any thread: replies (ring thread), interrupts (graphics thread), presents (present thread). */
static void push_to_guest(const Ps4macMessage *m) {
    pthread_mutex_lock(&to_guest_lock);
    if (ps4mac_ring_push(&block->to_guest, m, 2000000000ull))
        fputs("ps4mac_service: the guest does not take messages\n", stderr);
    pthread_mutex_unlock(&to_guest_lock);
}

/* Page tracking (step 3.4): the guest applies the protection; the video core goes on once it has
 * (a write the guest makes after this returns faults). Sequences in ring order. */
static uint32_t protect_sequence;
static void protect_guest(uintptr_t address, uint64_t size, int read, int write) {
    pthread_mutex_lock(&to_guest_lock);
    const uint32_t sequence = ++protect_sequence;
    const Ps4macMessage m = {.kind = PS4MAC_PROTECT, .sequence = sequence, .a = address, .b = size,
                             .c = (uint64_t)(read ? 1 : 0) | (uint64_t)(write ? 2 : 0)};
    const int pushed = !ps4mac_ring_push(&block->to_guest, &m, 2000000000ull);
    pthread_mutex_unlock(&to_guest_lock);
    if (pushed) ps4mac_counter_wait(&block->protect_done, sequence, 200000);
}

static void fail(const char *what) {
    fprintf(stderr, "ps4mac_service: %s: %s\n", what, strerror(errno));
    exit(1);
}

static void receive_objects(int socket_fd) {
    int fds[3] = {-1, -1, -1};
    char control[CMSG_SPACE(sizeof(fds))];
    char tag = 0;
    struct iovec iov = {&tag, 1};
    struct msghdr msg = {.msg_iov = &iov, .msg_iovlen = 1, .msg_control = control,
                         .msg_controllen = sizeof(control)};
    if (recvmsg(socket_fd, &msg, 0) != 1 || tag != 'B') fail("recvmsg");
    struct cmsghdr *c = CMSG_FIRSTHDR(&msg);
    if (!c || c->cmsg_type != SCM_RIGHTS) fail("no descriptors");
    memcpy(fds, CMSG_DATA(c), sizeof(fds));
    block = mmap(NULL, sizeof(Ps4macSharedBlock), PROT_READ | PROT_WRITE, MAP_SHARED, fds[0], 0);
    if (block == MAP_FAILED) fail("map bridge block");
    if (block->version != PS4MAC_BRIDGE_VERSION) {
        fprintf(stderr, "ps4mac_service: bridge version %u, expected %u\n", block->version, PS4MAC_BRIDGE_VERSION);
        exit(1);
    }
    pool_fd = fds[1];
    low_fd = fds[2];
}

static void map_guest_memory(void) {
    /* The PS4 user range, reserved like the guest does (runtime_memory_reserve). */
    void *want = (void *)(uintptr_t)block->user_min;
    void *p = mmap(want, block->user_max - block->user_min, PROT_NONE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (p != want) {
        fprintf(stderr, "ps4mac_service: cannot reserve the PS4 range at %p (got %p)\n", want, p);
        exit(1);
    }
    /* Low memory, whole: offset = address - low_base on both sides. */
    if (low_fd >= 0) {
        void *low = (void *)(uintptr_t)block->low_base;
        /* Inside the reservation above (user_max covers low memory): MAP_FIXED replaces only it. */
        void *q = mmap(low, block->low_size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, low_fd, 0);
        if (q != low) {
            fprintf(stderr, "ps4mac_service: cannot map low memory at %p (got %p)\n", low, q);
            exit(1);
        }
    }
}

static void handle(const Ps4macMessage *m) {
    switch (m->kind) {
    case PS4MAC_MEMORY_MAP:
        if (mmap((void *)(uintptr_t)m->a, m->b, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, pool_fd,
                 (off_t)m->c) == MAP_FAILED)
            fprintf(stderr, "ps4mac_service: map %#llx+%#llx failed: %s\n", (unsigned long long)m->a,
                    (unsigned long long)m->b, strerror(errno));
        else
            service_memory_mapped((uintptr_t)m->a, m->b, (int)m->d);
        ++maps;
        break;
    case PS4MAC_MEMORY_UNMAP:
        mmap((void *)(uintptr_t)m->a, m->b, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED, -1, 0);
        service_memory_unmapped((uintptr_t)m->a, m->b);
        ++unmaps;
        break;
    case PS4MAC_MEMORY_PROTECT:
        /* Memory stays read-write here; the table keeps the guest's view (what the GPU may read). */
        service_memory_protected((uintptr_t)m->a, m->b, (int)m->d);
        ++protects;
        break;
    case PS4MAC_GPU_INIT:
        pthread_mutex_lock(&gpu_lock);
        gpu_requested = 1;
        pthread_cond_signal(&gpu_cond);
        pthread_mutex_unlock(&gpu_lock);
        break;
    case PS4MAC_CHECKSUM: {
        const Ps4macMessage reply = {.kind = PS4MAC_CHECKSUM_REPLY, .a = m->a, .b = m->b, .c = m->c,
                                     .d = ps4mac_hash((const void *)(uintptr_t)m->a, m->b)};
        push_to_guest(&reply);
        ++checks;
        break;
    }
    case PS4MAC_WRITE_FAULT:
    case PS4MAC_READ_FAULT: {
        const int handled = atomic_load(&gfx_ready) &&
                            ps4mac_gfx_server_fault((uintptr_t)m->a, m->kind == PS4MAC_WRITE_FAULT);
        block->fault_result[m->sequence % 256] = (uint8_t)handled;
        ps4mac_counter_set(&block->fault_done, (uint32_t)m->sequence);
        ++faults;
        break;
    }
    case PS4MAC_GFX_SET_VO_PORT:
        if (!ps4mac_gfx_server_check_port(block->vo_port_size, block->vo_labels_offset,
                                          block->vo_groups_offset, block->vo_slots_offset)) {
            fputs("ps4mac_service: the guest's and the service's builds disagree; exiting\n", stderr);
            _exit(1);
        }
        ps4mac_gfx_server_handle(m);
        break;
    default:
        if (atomic_load(&gfx_ready) && !ps4mac_gfx_server_handle(m))
            fprintf(stderr, "ps4mac_service: unknown message %u\n", m->kind);
        break;
    }
}

static void *socket_watch(void *arg) {
    /* The guest side never writes again: EOF means it is gone. */
    char byte;
    while (read((int)(intptr_t)arg, &byte, 1) > 0) { }
    fprintf(stderr, "ps4mac_service: guest side gone (%llu maps, %llu unmaps, %llu protects, %llu faults, "
            "%llu checksums); exiting\n", maps, unmaps, protects, faults, checks);
    runtime_report();
    _exit(0);
    return NULL;
}

static void *ring_loop(void *unused) {
    (void)unused;
    /* Poll after the last message, then sleep (step 3.0); PS4MAC_SERVICE_SPIN_US, default 2000. */
    const char *spin_env = getenv("PS4MAC_SERVICE_SPIN_US");
    const uint64_t spin_ns = (spin_env ? strtoull(spin_env, NULL, 10) : 2000) * 1000ull;
    for (;;) {
        Ps4macMessage m;
        if (ps4mac_ring_pop(&block->to_graphics, &m, spin_ns, 1000000000ull)) handle(&m);
    }
    return NULL;
}

static void *gpu_init_thread(void *unused) {
    (void)unused;
    const Ps4macGpuConfig *c = &block->gpu_config;
    const SpartanGpuConfig config = {c->title, c->serial, c->user_dir, c->sdk_version, c->psf_attributes,
                                     c->width, c->height};
    const uint64_t start = ps4mac_now_ns();
    service_set_protect_sender(protect_guest); /* before the video core registers guest memory */
    if (spartanportgpu_service_init(&config, push_to_guest)) {
        fputs("ps4mac_service: video core initialization failed\n", stderr);
        _exit(1);
    }
    block->gfx_hdr_supported = (uint32_t)ps4mac_gfx_server_hdr_supported();
    atomic_store(&gfx_ready, 1);
    atomic_store(&block->gfx_ready, 1); /* the guest's Ps4mac::Gfx starts sending */
    fprintf(stderr, "ps4mac_service: video core ready in %.0f ms (%s, %zu guest ranges registered)\n",
            (ps4mac_now_ns() - start) / 1e6, c->serial, service_memory_range_count());
    return NULL;
}

int main(int argc, char **argv) {
    int socket_fd = -1;
    for (int i = 1; i + 1 < argc; ++i)
        if (!strcmp(argv[i], "--socket")) socket_fd = atoi(argv[i + 1]);
    if (socket_fd < 0) {
        fprintf(stderr, "usage: %s --socket <fd> (started by the ps4mac_port runtime)\n", argv[0]);
        return 2;
    }
    setvbuf(stdout, NULL, _IOLBF, 0); /* the guest's log is a pipe: lines, not blocks */
    receive_objects(socket_fd);
    map_guest_memory();
    service_set_clock_start(block->clock_start_ns);
    service_install_fault_handler();
    atomic_store(&block->service_alive, 1);
    if (write(socket_fd, "R", 1) != 1) fail("ready");
    pthread_t watcher;
    pthread_create(&watcher, NULL, socket_watch, (void *)(intptr_t)socket_fd);
    pthread_t ring;
    pthread_create(&ring, NULL, ring_loop, NULL);
    const char *video = getenv("PS4MAC_SERVICE_VIDEO");
    if (video && *video == '0') {
        pthread_join(ring, NULL);
        return 0;
    }
    /* The video core: started on PS4MAC_GPU_INIT; its window runs on this (the main) thread. */
    pthread_mutex_lock(&gpu_lock);
    while (!gpu_requested) pthread_cond_wait(&gpu_cond, &gpu_lock);
    pthread_mutex_unlock(&gpu_lock);
    pthread_t init;
    pthread_create(&init, NULL, gpu_init_thread, NULL);
    spartanportgpu_main_loop(); /* returns only when the window closes */
    return 0;
}
