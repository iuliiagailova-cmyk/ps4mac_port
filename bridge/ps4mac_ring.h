/* SPDX-License-Identifier: GPL-2.0-or-later
 * ps4mac_port: the shared block of the bridge and its single-producer single-consumer rings
 * (C11 atomics; included by the x86 runtime and the arm64 service). Wake-ups use
 * os_sync_wait_on_address on shared memory (macOS 14.4+); a consumer polls briefly first
 * (step 3.0: a wake-up costs ~25 us, a poll 0.2 us).
 */
#ifndef PS4MAC_RING_H
#define PS4MAC_RING_H

#include <stdatomic.h>
#include <stdint.h>
#include <string.h>
#include <os/os_sync_wait_on_address.h>
#include <mach/mach_time.h>

#include "ps4mac_bridge.h"

/* The platform needs macOS 26 (KosmicKrisp); the guest runtime is built for an older minimum, and
 * these calls run only with the graphics service. */
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunguarded-availability-new"

#define PS4MAC_RING_SLOTS 4096u

typedef struct {
    _Alignas(64) _Atomic uint64_t head;
    _Alignas(64) _Atomic uint64_t tail;
    _Alignas(64) _Atomic uint32_t sleeping;
    _Atomic uint32_t head_word; /* low 32 bits of head: the wait address */
    Ps4macMessage slots[PS4MAC_RING_SLOTS];
} Ps4macSharedRing;

/* The video core's start-up settings (SpartanGpuConfig), written before PS4MAC_GPU_INIT. */
typedef struct {
    char title[256], serial[32], user_dir[1024];
    uint32_t sdk_version, psf_attributes;
    int32_t width, height;
} Ps4macGpuConfig;

/* Set up by the guest side before the service starts; sizes and bases tell the service how to
 * map guest memory at the same addresses. */
typedef struct {
    uint32_t version;       /* PS4MAC_BRIDGE_VERSION */
    uint32_t page_size;
    uint64_t pool_size;     /* direct + flexible memory: the pool object, mapped per message */
    uint64_t low_base;      /* low memory (images, runtime allocations): one object at low_base */
    uint64_t low_size;
    uint64_t user_min, user_max; /* the PS4 user range both sides reserve */
    _Atomic uint32_t guest_alive, service_alive;
    uint64_t clock_start_ns; /* the guest's process clock origin (CLOCK_MONOTONIC, ns) */
    Ps4macGpuConfig gpu_config;
    /* Step 3.3c: the guest's VideoOutPort lives in shared memory and the video core reads it, so
     * both builds must lay it out alike (checked by the service). */
    uint32_t vo_port_size, vo_labels_offset, vo_groups_offset, vo_slots_offset;
    _Atomic uint32_t gfx_ready;     /* the service's video core is up (it answers Ps4mac::Gfx) */
    uint32_t gfx_hdr_supported;
    /* Step 3.4, page tracking. Faults are answered in sequence order: fault_done is the last
     * sequence handled, fault_result[sequence % 256] whether it was the GPU's page. */
    _Alignas(64) _Atomic uint32_t fault_done;
    uint8_t fault_result[256];
    _Alignas(64) _Atomic uint32_t protect_done; /* the last PS4MAC_PROTECT the guest applied */
    /* Step 3.6: the bridge's cost (the guest prints it every 5 s with SPARTAN_FRAME_STATS). */
    _Alignas(64) _Atomic uint64_t stat_protects, stat_protect_wait_ns; /* service: waits for the guest */
    _Atomic uint64_t stat_faults, stat_fault_handle_ns;              /* service: handling a fault */
    _Atomic uint64_t stat_fault_wait_ns;                              /* guest: a fault's round trip */
    _Atomic uint64_t stat_fault_drain_ns;                             /* service: of it, the draw pipe drain */
    Ps4macSharedRing to_graphics;
    Ps4macSharedRing to_guest;
} Ps4macSharedBlock;

/* Bridge-level message kinds beyond ps4mac_bridge.h (diagnostics). */
enum {
    PS4MAC_CHECKSUM = 100, /* guest -> graphics: a = address, b = size, c = guest's hash */
    PS4MAC_CHECKSUM_REPLY, /* graphics -> guest: a, b as asked, c = guest's hash, d = service's */
    PS4MAC_QUIT,
};

static inline uint64_t ps4mac_now_ns(void) {
    static mach_timebase_info_data_t tb;
    if (!tb.denom) mach_timebase_info(&tb);
    return mach_absolute_time() * tb.numer / tb.denom;
}

/* Returns 0, or -1 when the ring stays full (the consumer is gone) for `timeout_ns`. */
static inline int ps4mac_ring_push(Ps4macSharedRing *r, const Ps4macMessage *m, uint64_t timeout_ns) {
    const uint64_t head = atomic_load_explicit(&r->head, memory_order_relaxed);
    if (head - atomic_load_explicit(&r->tail, memory_order_acquire) >= PS4MAC_RING_SLOTS) {
        const uint64_t start = ps4mac_now_ns();
        while (head - atomic_load_explicit(&r->tail, memory_order_acquire) >= PS4MAC_RING_SLOTS)
            if (ps4mac_now_ns() - start > timeout_ns) return -1;
    }
    r->slots[head % PS4MAC_RING_SLOTS] = *m;
    atomic_store_explicit(&r->head, head + 1, memory_order_release);
    atomic_store_explicit(&r->head_word, (uint32_t)(head + 1), memory_order_release);
    if (atomic_load_explicit(&r->sleeping, memory_order_seq_cst))
        os_sync_wake_by_address_any((void *)&r->head_word, 4, OS_SYNC_WAKE_BY_ADDRESS_SHARED);
    return 0;
}

/* Waits up to `timeout_ns` (polling for `spin_ns` first); returns 1 with *m filled, or 0. */
static inline int ps4mac_ring_pop(Ps4macSharedRing *r, Ps4macMessage *m, uint64_t spin_ns,
                                  uint64_t timeout_ns) {
    const uint64_t tail = atomic_load_explicit(&r->tail, memory_order_relaxed);
    const uint64_t start = ps4mac_now_ns();
    for (uint32_t n = 0;; ++n) {
        if (atomic_load_explicit(&r->head, memory_order_acquire) != tail) break;
        if ((n & 63) != 0) continue;
        const uint64_t waited = ps4mac_now_ns() - start;
        if (waited >= timeout_ns) return 0;
        if (waited < spin_ns) continue;
        atomic_store_explicit(&r->sleeping, 1, memory_order_seq_cst);
        if (atomic_load_explicit(&r->head, memory_order_seq_cst) == tail) {
            const uint64_t left = timeout_ns - waited;
            os_sync_wait_on_address_with_timeout((void *)&r->head_word, (uint32_t)tail, 4,
                                                 OS_SYNC_WAIT_ON_ADDRESS_SHARED,
                                                 OS_CLOCK_MACH_ABSOLUTE_TIME, left);
        }
        atomic_store_explicit(&r->sleeping, 0, memory_order_relaxed);
    }
    *m = r->slots[tail % PS4MAC_RING_SLOTS];
    atomic_store_explicit(&r->tail, tail + 1, memory_order_release);
    return 1;
}

/* Waits until the 32-bit sequence counter reaches `target` (wrapping), polling for `spin_ns`
 * first; the other side stores the counter, then calls ps4mac_counter_wake. */
static inline void ps4mac_counter_wait(_Atomic uint32_t *counter, uint32_t target, uint64_t spin_ns) {
    const uint64_t start = ps4mac_now_ns();
    for (uint32_t n = 0;; ++n) {
        const uint32_t now = atomic_load_explicit(counter, memory_order_acquire);
        if ((int32_t)(now - target) >= 0) return;
        if ((n & 63) || ps4mac_now_ns() - start < spin_ns) continue;
        os_sync_wait_on_address_with_timeout((void *)counter, now, 4, OS_SYNC_WAIT_ON_ADDRESS_SHARED,
                                             OS_CLOCK_MACH_ABSOLUTE_TIME, 1000000);
    }
}

static inline void ps4mac_counter_set(_Atomic uint32_t *counter, uint32_t value) {
    atomic_store_explicit(counter, value, memory_order_release);
    os_sync_wake_by_address_all((void *)counter, 4, OS_SYNC_WAKE_BY_ADDRESS_SHARED);
}

/* FNV-1a over guest memory, for the checksum check. */
static inline uint64_t ps4mac_hash(const void *data, uint64_t size) {
    const unsigned char *p = (const unsigned char *)data;
    uint64_t h = 1469598103934665603ull;
    for (uint64_t i = 0; i < size; ++i) h = (h ^ p[i]) * 1099511628211ull;
    return h;
}

#pragma clang diagnostic pop

#endif /* PS4MAC_RING_H */
