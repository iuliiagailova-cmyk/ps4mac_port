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

#define PS4MAC_BRIDGE_VERSION 5u

/* Guest -> graphics, in order (submissions and memory changes share one ring: a submission
 * must see the mappings made before it). Messages with PS4MAC_FLAG_REPLY get a PS4MAC_REPLY
 * with the same sequence. Addresses are guest addresses (memory both processes map). */
enum Ps4macToGraphics {
    PS4MAC_SUBMIT_GFX = 1,       /* a dcb, b dcb dwords, c ccb, d ccb dwords */
    PS4MAC_SUBMIT_COMPUTE,       /* a acb, b acb dwords, c vqid */
    PS4MAC_SUBMIT_DONE,          /* sceGnmSubmitDone: the frame's submissions are complete */
    PS4MAC_MAP_COMPUTE_QUEUE,    /* a ring base, b read pointer address, c size dw, d pipe -> reply a vqid */
    PS4MAC_UNMAP_COMPUTE_QUEUE,  /* a vqid */
    PS4MAC_FLIP,                 /* (unused) */
    PS4MAC_MEMORY_MAP,           /* guest mapping appeared: a address, b size, c pool offset, d host prot */
    PS4MAC_MEMORY_UNMAP,         /* a address, b size */
    PS4MAC_MEMORY_INVALIDATE,    /* guest wrote memory without a fault (e.g. file reads) */
    PS4MAC_WRITE_FAULT,          /* a address, b size of a store the guest can perform itself
                                  * (0: none), sequence: the guest wrote a page the GPU protects;
                                  * result 2: perform the store, the page stays protected;
                                  * the result goes to the shared block (fault_done, fault_result) */
    PS4MAC_READ_FAULT,           /* the same for a read (a readback) */
    PS4MAC_MEMORY_PROTECT,       /* the guest changed protection: a address, b size, d host prot */
    PS4MAC_GPU_INIT,             /* the video core may start: configuration in the shared block */
    /* Ps4mac::Gfx (gpu/shim/ps4mac_graphics.h), step 3.3c: */
    PS4MAC_GFX_IS_IDLE,          /* reply a: no submission queued or in flight */
    PS4MAC_GFX_RESERVE_COPY,
    PS4MAC_GFX_RUN_TASK,         /* reply once the graphics thread reaches it (RunOnGraphicsThread) */
    PS4MAC_GFX_SET_VO_PORT,      /* a VideoOutPort (in shared memory) */
    PS4MAC_GFX_REGISTER_BUFFER,  /* a BufferAttributeGroup (in the shared port), b display buffer address */
    PS4MAC_GFX_PREPARE_FLIP,     /* a group, b address, c frame id: on the graphics thread */
    PS4MAC_GFX_PREPARE_BLANK,    /* a present_thread, c frame id */
    PS4MAC_GFX_PREPARE_LAST,     /* c frame id (may prepare nothing) */
    PS4MAC_GFX_PRESENT,          /* a frame id, b reusing | game << 1, c hdr + 1 or 0; reply when shown */
    PS4MAC_GFX_SET_GAMMA,        /* a float bits */
    /* Step 3.5: */
    PS4MAC_GFX_UI_ACTION,        /* a Ps4macUiAction: a pad shortcut the service's menu and window act on */
};

enum Ps4macUiAction {
    PS4MAC_UI_MOUSE_PAUSE = 1,   /* the pause key: the window lets the mouse go or takes it back */
    PS4MAC_UI_ENGINE_FPS_CYCLE,  /* R3 + L1 + R1: the next engine frame rate */
};

/* Graphics -> guest. */
enum Ps4macToGuest {
    PS4MAC_IRQ = 1,              /* a Platform::InterruptId: signalled to the guest's IrqC (equeue) */
    PS4MAC_PROTECT,              /* a address, b size, c read | write << 1, sequence: the guest runs
                                  * runtime_memory_gpu_protect, then sets protect_done = sequence */
    PS4MAC_FAULT_DONE,           /* step 3.4 */
    PS4MAC_FLIP_DONE,            /* (unused) */
    PS4MAC_QUEUE_MAPPED,         /* (unused: PS4MAC_REPLY) */
    PS4MAC_REPLY,                /* sequence of the request, a value */
};

#define PS4MAC_FLAG_REPLY 1u

typedef struct Ps4macMessage {
    uint32_t kind;     /* Ps4macToGraphics or Ps4macToGuest */
    uint32_t flags;
    uint64_t sequence; /* replies carry the request's */
    uint64_t a, b, c, d; /* per kind: addresses, sizes, ids (documented at each use) */
} Ps4macMessage;

#ifndef __cplusplus
/* Single-producer single-consumer ring in shared memory (the one in use: ps4mac_ring.h). */
typedef struct Ps4macRing {
    _Alignas(64) uint64_t head; /* written by the producer */
    _Alignas(64) uint64_t tail; /* written by the consumer */
    _Alignas(64) uint32_t capacity; /* power of two */
    uint32_t reserved;
    /* Ps4macMessage slots[capacity] follow */
} Ps4macRing;
#endif

/* Atomics both languages lay out alike (clang: std::atomic<T> wraps _Atomic T for these types). */
#ifdef __cplusplus
}
#include <atomic>
#define PS4MAC_ATOMIC(T) std::atomic<T>
extern "C" {
#else
#define PS4MAC_ATOMIC(T) _Atomic T
#endif

/* Step 3.5: the service's window input and in-game menu state, written by the service (window
 * thread) and read by the guest's pad (src/runtime_pad.c) and gpu library getters. Counters only
 * grow: the reader keeps the last values it saw. */
typedef struct Ps4macInput {
    PS4MAC_ATOMIC(uint32_t) ready;            /* the service publishes input */
    PS4MAC_ATOMIC(uint32_t) overlay_captures; /* the menu, a choice list or a text dialog has the input */
    PS4MAC_ATOMIC(uint32_t) mouse_buttons;    /* SDL_MouseButtonFlags */
    PS4MAC_ATOMIC(uint32_t) wheel_notches[2]; /* up, down */
    PS4MAC_ATOMIC(int32_t) motion[2];         /* x, y in 1/16 px */
    /* Settings the menu changes that the guest reads (SpartanSettings). */
    PS4MAC_ATOMIC(int32_t) vibration, mouse_prompt_distance, mouse_aim_style, mouse_aim_sensitivity;
    PS4MAC_ATOMIC(uint8_t) keys[512];         /* SDL scancodes: 1 while down */
} Ps4macInput;

/* Channels that already are narrow C seams in one process; the two-process build implements
 * each with the messages above (src/runtime.h, gpu/shim/spartanportgpu.cpp):
 *   guest -> graphics  PS4MAC_MEMORY_MAP/UNMAP/INVALIDATE  hooks the graphics side registers with
 *                      runtime_memory_set_gpu_hooks(map, unmap, invalidate)
 *   guest -> graphics  PS4MAC_WRITE_FAULT/READ_FAULT       spartanportgpu_handle_fault(context, addr)
 *   graphics -> guest  PS4MAC_PROTECT                      runtime_memory_gpu_protect(addr, size, r, w)
 *   graphics -> guest  (reads)                             runtime_memory_region/clamp/is_mapped,
 *                      runtime_memory_write_backing: with guest memory shared, the graphics side
 *                      keeps a read-only copy of the mapping table instead of calling across.
 * GnmDriver and VideoOut reach the video core through Ps4mac::Gfx (gpu/shim/ps4mac_graphics.h). */

#ifdef __cplusplus
}
#endif

#endif /* PS4MAC_BRIDGE_H */
