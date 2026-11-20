/* SPDX-License-Identifier: GPL-2.0-or-later
 * ps4mac_port: the bridge between the guest side (x86-64 under Rosetta 2: game code, PS4 runtime,
 * thin HLE fronts of GnmDriver/VideoOut) and the graphics service (shadPS4 video core,
 * KosmicKrisp, Metal; native arm64 in the two-process build). See docs/architecture.md, §3.2.
 *
 * Step 2 (in one process): the guest side calls Ps4mac::Gfx (gpu/shim/ps4mac_graphics.h), which
 * calls the video core directly. These messages are the contract the two-process build will carry
 * over shared memory; their layout is fixed here so both sides compile against one definition.
 * All addresses are guest virtual addresses: both processes map guest memory at the same ones.
 */
#ifndef PS4MAC_BRIDGE_H
#define PS4MAC_BRIDGE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PS4MAC_BRIDGE_VERSION 1u

/* Guest -> graphics, in order (submissions and memory changes share one ring: a submission
 * must see the mappings made before it). */
enum Ps4macToGraphics {
    PS4MAC_SUBMIT_GFX = 1,       /* dcb/ccb command buffers (one pair) */
    PS4MAC_SUBMIT_COMPUTE,       /* acb span of a mapped compute queue */
    PS4MAC_SUBMIT_DONE,          /* sceGnmSubmitDone: the frame's submissions are complete */
    PS4MAC_MAP_COMPUTE_QUEUE,    /* ring base, read pointer address, size, pipe -> vqid */
    PS4MAC_UNMAP_COMPUTE_QUEUE,
    PS4MAC_FLIP,                 /* VideoOut: show a registered buffer */
    PS4MAC_MEMORY_MAP,           /* guest mapping appeared (GPU may access it) */
    PS4MAC_MEMORY_UNMAP,
    PS4MAC_MEMORY_INVALIDATE,    /* guest wrote memory without a fault (e.g. file reads) */
    PS4MAC_WRITE_FAULT,          /* guest wrote a protected page: graphics answers PS4MAC_FAULT_DONE */
    PS4MAC_READ_FAULT,
};

/* Graphics -> guest. */
enum Ps4macToGuest {
    PS4MAC_IRQ = 1,              /* end of pipe / flip / GPU idle: delivered through equeue */
    PS4MAC_PROTECT,              /* track CPU writes (and reads) of a range: guest runs mprotect */
    PS4MAC_FAULT_DONE,           /* the faulting access may proceed */
    PS4MAC_FLIP_DONE,
    PS4MAC_QUEUE_MAPPED,         /* reply to PS4MAC_MAP_COMPUTE_QUEUE */
};

typedef struct Ps4macMessage {
    uint32_t kind;     /* Ps4macToGraphics or Ps4macToGuest */
    uint32_t flags;
    uint64_t sequence; /* replies carry the request's */
    uint64_t a, b, c, d; /* per kind: addresses, sizes, ids (documented at each use) */
} Ps4macMessage;

/* Single-producer single-consumer ring in shared memory. */
typedef struct Ps4macRing {
    _Alignas(64) uint64_t head; /* written by the producer */
    _Alignas(64) uint64_t tail; /* written by the consumer */
    _Alignas(64) uint32_t capacity; /* power of two */
    uint32_t reserved;
    /* Ps4macMessage slots[capacity] follow */
} Ps4macRing;

#ifdef __cplusplus
}
#endif

#endif /* PS4MAC_BRIDGE_H */
