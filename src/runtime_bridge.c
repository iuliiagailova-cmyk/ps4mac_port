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
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include "runtime.h"
#include "../bridge/ps4mac_ring.h"

int runtime_memory_region(uintptr_t address, uintptr_t *start, uintptr_t *end, int *mapped);

static Ps4macSharedBlock *block;
static pid_t service_pid;
static pthread_mutex_t push_lock = PTHREAD_MUTEX_INITIALIZER; /* the ring has one producer */

static void send_message(const Ps4macMessage *m) {
    pthread_mutex_lock(&push_lock);
    if (ps4mac_ring_push(&block->to_graphics, m, 2000000000ull))
        fputs("Bridge: the graphics service does not take messages\n", stderr);
    pthread_mutex_unlock(&push_lock);
}

static void observe_mapping(uintptr_t address, uint64_t size, int kind, uint64_t phys) {
    if (!block || kind == 1) return; /* the low object is mapped whole by the service */
    const Ps4macMessage m = {
        .kind = kind == 2 ? PS4MAC_MEMORY_UNMAP : PS4MAC_MEMORY_MAP, .a = address, .b = size, .c = phys,
    };
    send_message(&m);
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

static void *reply_thread(void *unused) {
    (void)unused;
    unsigned ok = 0, bad = 0;
    uint64_t last_report = ps4mac_now_ns();
    for (;;) {
        Ps4macMessage m;
        if (!ps4mac_ring_pop(&block->to_guest, &m, 50000, 1000000000ull)) {
            if (!atomic_load(&block->service_alive)) return NULL;
            continue;
        }
        if (m.kind == PS4MAC_CHECKSUM_REPLY) {
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
    block->page_size = (uint32_t)getpagesize();
    atomic_store(&block->guest_alive, 1);

    int sockets[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets)) { perror("socketpair"); exit(21); }
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, sockets[1], 3);
    char *argv[] = {(char *)service, "--socket", "3", NULL};
    extern char **environ;
    if (posix_spawn(&service_pid, service, &actions, NULL, argv, environ)) {
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
    pthread_detach(thread);
    pthread_create(&thread, NULL, reply_thread, NULL);
    pthread_detach(thread);
    const char *check = getenv("PS4MAC_BRIDGE_CHECK");
    if (check && *check == '1') {
        pthread_create(&thread, NULL, check_thread, NULL);
        pthread_detach(thread);
    }
}
#else
void runtime_bridge_start(void) {}
#endif
