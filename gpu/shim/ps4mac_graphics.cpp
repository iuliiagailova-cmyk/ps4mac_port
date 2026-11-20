// SPDX-License-Identifier: GPL-2.0-or-later
// ps4mac_port: the graphics bridge as the guest-side HLE code sees it (see ps4mac_graphics.h).
// In one process the functions call the video core directly. With the graphics service rendering
// (step 3.3c: PS4MAC_SERVICE, runtime_bridge_remote_graphics) they are messages on the bridge
// ring (bridge/ps4mac_bridge.h), answered by ps4mac_graphics_server.cpp in the service.
#include "ps4mac_graphics.h"

#include <atomic>
#include <bit>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <unordered_map>

#include "../../bridge/ps4mac_bridge.h"
#include "core/platform.h"
#include "video_core/amdgpu/liverpool.h"
#include "video_core/renderer_vulkan/vk_presenter.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"

extern std::unique_ptr<Vulkan::Presenter> presenter;

extern "C" {
// src/runtime_bridge.c (the x86 runtime; the service has stand-ins that say "not remote")
int runtime_bridge_remote_graphics(void);
void runtime_bridge_send(const Ps4macMessage* m);
void runtime_bridge_set_handler(void (*handler)(const Ps4macMessage*));
int runtime_bridge_is_shared(uintptr_t address, uint64_t size);
int runtime_bridge_wait_graphics(void);
void* runtime_low_map(size_t size, int prot);
}

namespace Ps4mac::Gfx {
namespace {
AmdGpu::Liverpool* core = nullptr;

// ---- The client side of the bridge.
std::mutex calls_mutex;
std::condition_variable calls_cv;
struct Pending {
    bool done = false;
    u64 value = 0;
    std::function<void()> task; // RunOnGraphicsThread: run on reply instead of waking a caller
};
std::unordered_map<u64, Pending> pending;
std::atomic<u64> next_sequence{1};
std::atomic<u64> next_frame{1};
bool hdr_supported = false;
int pending_hdr = 0; // set by SetHdr, sent with the next Present (both on the swap thread)

struct QueueMirror {
    std::uintptr_t ring_base;
    std::uint32_t* read_pointer;
    std::uint32_t ring_size_dw;
};
std::mutex queues_mutex;
std::unordered_map<u32, QueueMirror> queues;

void Post(Ps4macMessage m) {
    runtime_bridge_send(&m);
}

u64 Call(Ps4macMessage m) {
    const u64 sequence = next_sequence.fetch_add(1);
    m.sequence = sequence;
    m.flags |= PS4MAC_FLAG_REPLY;
    {
        std::scoped_lock lock{calls_mutex};
        pending.try_emplace(sequence);
    }
    runtime_bridge_send(&m);
    std::unique_lock lock{calls_mutex};
    calls_cv.wait(lock, [&] { return pending[sequence].done; });
    const u64 value = pending[sequence].value;
    pending.erase(sequence);
    return value;
}

// On the bridge's reply thread (src/runtime_bridge.c).
void OnMessage(const Ps4macMessage* m) {
    if (m->kind == PS4MAC_IRQ) {
        Platform::IrqC::Instance()->Signal(static_cast<Platform::InterruptId>(m->a));
        return;
    }
    if (m->kind != PS4MAC_REPLY) {
        return;
    }
    std::function<void()> task;
    {
        std::scoped_lock lock{calls_mutex};
        const auto it = pending.find(m->sequence);
        if (it == pending.end()) {
            return;
        }
        if (it->second.task) {
            task = std::move(it->second.task);
            pending.erase(it);
        } else {
            it->second.done = true;
            it->second.value = m->a;
        }
    }
    if (task) {
        task();
    } else {
        calls_cv.notify_all();
    }
}

/// Command buffers outside guest memory (GnmDriver's static init sequences) are copied once
/// into shared memory, so the service can read them.
const u32* SharedSpan(std::span<const u32> span) {
    if (span.empty() ||
        runtime_bridge_is_shared(reinterpret_cast<uintptr_t>(span.data()), span.size_bytes())) {
        return span.data();
    }
    static std::mutex mutex;
    static std::unordered_map<const u32*, u32*> copies;
    std::scoped_lock lock{mutex};
    auto& copy = copies[span.data()];
    if (!copy) {
        copy = static_cast<u32*>(runtime_low_map(span.size_bytes(), 3 /* read | write */));
        ASSERT_MSG(copy, "no shared memory for a command buffer copy");
        std::memcpy(copy, span.data(), span.size_bytes());
    }
    return copy;
}
} // namespace

bool Remote() {
    // Not cached: the window's main loop may ask before the runtime has started the bridge.
    return runtime_bridge_remote_graphics() != 0;
}

void StartRemote() {
    runtime_bridge_set_handler(&OnMessage);
    hdr_supported = runtime_bridge_wait_graphics() != 0;
}

void Attach(AmdGpu::Liverpool* liverpool) {
    core = liverpool;
}

void SubmitGfx(std::span<const std::uint32_t> dcb, std::span<const std::uint32_t> ccb) {
    if (Remote()) {
        Post({.kind = PS4MAC_SUBMIT_GFX,
              .a = u64(SharedSpan(dcb)),
              .b = dcb.size(),
              .c = u64(SharedSpan(ccb)),
              .d = ccb.size()});
        return;
    }
    core->SubmitGfx(dcb, ccb);
}

void SubmitCompute(std::uint32_t gnm_vqid, std::span<const std::uint32_t> acb) {
    if (Remote()) {
        Post({.kind = PS4MAC_SUBMIT_COMPUTE, .a = u64(SharedSpan(acb)), .b = acb.size(), .c = gnm_vqid});
        return;
    }
    core->SubmitAsc(gnm_vqid, acb);
}

void SubmitDone() {
    if (Remote()) {
        Post({.kind = PS4MAC_SUBMIT_DONE});
        return;
    }
    core->SubmitDone();
}

bool IsIdle() {
    if (Remote()) {
        return Call({.kind = PS4MAC_GFX_IS_IDLE}) != 0;
    }
    return core->IsGpuIdle();
}

void ReserveCopyBufferSpace() {
    if (Remote()) {
        Post({.kind = PS4MAC_GFX_RESERVE_COPY});
        return;
    }
    core->ReserveCopyBufferSpace();
}

std::uint32_t MapComputeQueue(std::uintptr_t ring_base, std::uint32_t* read_pointer,
                              std::uint32_t ring_size_dw, std::uint32_t pipe_id) {
    if (Remote()) {
        const u32 vqid = u32(Call({.kind = PS4MAC_MAP_COMPUTE_QUEUE,
                                   .a = ring_base,
                                   .b = u64(read_pointer),
                                   .c = ring_size_dw,
                                   .d = pipe_id}));
        std::scoped_lock lock{queues_mutex};
        queues[vqid] = {ring_base, read_pointer, ring_size_dw};
        return vqid;
    }
    return core->asc_queues.insert(VAddr(ring_base), read_pointer, ring_size_dw, pipe_id).index;
}

void UnmapComputeQueue(std::uint32_t vqid) {
    if (Remote()) {
        {
            std::scoped_lock lock{queues_mutex};
            queues.erase(vqid);
        }
        Post({.kind = PS4MAC_UNMAP_COMPUTE_QUEUE, .a = vqid});
        return;
    }
    core->asc_queues.erase(Common::SlotId{vqid});
}

ComputeQueue GetComputeQueue(std::uint32_t vqid) {
    if (Remote()) {
        std::scoped_lock lock{queues_mutex};
        const auto& queue = queues[vqid];
        return {queue.ring_base, queue.read_pointer, queue.ring_size_dw};
    }
    const auto& queue = core->asc_queues[Common::SlotId{vqid}];
    return {queue.map_addr, queue.read_addr, queue.ring_size_dw};
}

void SetVideoOutPort(Libraries::VideoOut::VideoOutPort* port) {
    if (Remote()) {
        Post({.kind = PS4MAC_GFX_SET_VO_PORT, .a = u64(port)});
        return;
    }
    core->SetVoPort(port);
}

void RegisterDisplayBuffer(const Libraries::VideoOut::BufferAttributeGroup& group,
                           std::uintptr_t address) {
    if (Remote()) {
        Post({.kind = PS4MAC_GFX_REGISTER_BUFFER, .a = u64(&group), .b = address});
        return;
    }
    presenter->RegisterVideoOutSurface(group, address);
}

FrameHandle PrepareFlip(const Libraries::VideoOut::BufferAttributeGroup& group,
                        std::uintptr_t address) {
    if (Remote()) {
        const u64 frame = next_frame.fetch_add(1);
        Post({.kind = PS4MAC_GFX_PREPARE_FLIP, .a = u64(&group), .b = address, .c = frame});
        return reinterpret_cast<FrameHandle>(frame);
    }
    return presenter->PrepareFrame(group, address);
}

FrameHandle PrepareBlankFrame(bool present_thread) {
    if (Remote()) {
        const u64 frame = next_frame.fetch_add(1);
        Post({.kind = PS4MAC_GFX_PREPARE_BLANK, .a = present_thread, .c = frame});
        return reinterpret_cast<FrameHandle>(frame);
    }
    return presenter->PrepareBlankFrame(present_thread);
}

FrameHandle PrepareLastFrame() {
    if (Remote()) {
        const u64 frame = next_frame.fetch_add(1);
        Post({.kind = PS4MAC_GFX_PREPARE_LAST, .c = frame});
        return reinterpret_cast<FrameHandle>(frame);
    }
    return presenter->PrepareLastFrame();
}

void SetHdr(bool enable) {
    if (Remote()) {
        pending_hdr = enable ? 2 : 1;
        return;
    }
    presenter->SetHDR(enable);
}

void Present(FrameHandle frame, bool is_reusing_frame, bool is_game_frame) {
    if (Remote()) {
        Call({.kind = PS4MAC_GFX_PRESENT,
              .a = u64(frame),
              .b = u64(is_reusing_frame) | u64(is_game_frame) << 1,
              .c = u64(pending_hdr)});
        pending_hdr = 0;
        return;
    }
    presenter->Present(static_cast<Vulkan::Frame*>(frame), is_reusing_frame, is_game_frame);
}

bool IsHdrSupported() {
    if (Remote()) {
        return hdr_supported;
    }
    return presenter->IsHDRSupported();
}

void SetGamma(float gamma) {
    if (Remote()) {
        Post({.kind = PS4MAC_GFX_SET_GAMMA, .a = std::bit_cast<u32>(gamma)});
        return;
    }
    presenter->GetPPSettingsRef().gamma = gamma;
}

void RunOnGraphicsThread(std::function<void()> task) {
    if (Remote()) {
        // The service answers when its graphics thread reaches this point; the task then runs
        // here, on the bridge's reply thread, in order with the submissions before it.
        const u64 sequence = next_sequence.fetch_add(1);
        {
            std::scoped_lock lock{calls_mutex};
            pending[sequence].task = std::move(task);
        }
        Post({.kind = PS4MAC_GFX_RUN_TASK, .flags = PS4MAC_FLAG_REPLY, .sequence = sequence});
        return;
    }
    core->SendCommand(std::move(task));
}

void ReportCompilerStats() {
    if (Remote()) {
        return; // the shaders are compiled in the service
    }
    presenter->GetRasterizer().GetPipelineCache().ReportCompilerStats();
}

} // namespace Ps4mac::Gfx
