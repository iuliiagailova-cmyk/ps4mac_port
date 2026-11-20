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
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "runtime.h"
#include "../bridge/ps4mac_ring.h"

int runtime_memory_region(uintptr_t address, uintptr_t *start, uintptr_t *end, int *mapped);
void runtime_memory_gpu_protect(uintptr_t address, uint64_t size, int read, int write);

static Ps4macSharedBlock *block;
static pid_t service_pid;
static pthread_mutex_t push_lock = PTHREAD_MUTEX_INITIALIZER; /* the ring has one producer */

static void send_message(const Ps4macMessage *m) {
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
int runtime_bridge_fault(uintptr_t address, int is_write) {
    if (!block || !remote_graphics || !runtime_bridge_is_shared(address, 1)) return 0;
    pthread_mutex_lock(&push_lock); /* sequences in ring order: the service answers in order */
    const uint32_t sequence = ++fault_sequence;
    const Ps4macMessage m = {.kind = is_write ? PS4MAC_WRITE_FAULT : PS4MAC_READ_FAULT,
                             .sequence = sequence, .a = address};
    const int pushed = !ps4mac_ring_push(&block->to_graphics, &m, 2000000000ull);
    pthread_mutex_unlock(&push_lock);
    if (!pushed) return 0;
    ps4mac_counter_wait(&block->fault_done, sequence, 200000);
    return block->fault_result[sequence % 256];
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
int runtime_bridge_fault(uintptr_t address, int is_write) { (void)address; (void)is_write; return 0; }
int runtime_bridge_gpu_init(const char *title, const char *serial, const char *user_dir, uint32_t sdk_version,
                            uint32_t psf_attributes, int32_t width, int32_t height) {
    (void)title; (void)serial; (void)user_dir; (void)sdk_version; (void)psf_attributes; (void)width; (void)height;
    return 0;
}
#endif
