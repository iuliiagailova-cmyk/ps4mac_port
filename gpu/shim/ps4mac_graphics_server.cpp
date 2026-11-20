// SPDX-License-Identifier: GPL-2.0-or-later
// ps4mac_port step 3.3c: the graphics service's side of Ps4mac::Gfx. The guest's calls arrive as
// bridge messages (bridge/ps4mac_bridge.h) on the service's ring thread and run against the
// video core here, as ps4mac_graphics.cpp runs them in one process:
//  - submissions, compute queues, idle queries: on the ring thread (Liverpool queues the work);
//  - frame preparation: on the graphics command thread (Liverpool::SendCommand), as in-process
//    flips are prepared there;
//  - presentation: on a present thread of its own, so a swapchain wait never holds up the ring;
//  - interrupts (EOP, flip, GPU idle, compute): forwarded to the guest's IrqC.
#include <bit>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <unordered_map>

#include "../../bridge/ps4mac_bridge.h"
#include "common/thread.h"
#include "core/libraries/videoout/driver.h"
#include "core/platform.h"
#include "ps4mac_graphics.h"
#include "spartanport_toggles.h"
#include "video_core/amdgpu/liverpool.h"
#include "video_core/renderer_vulkan/vk_presenter.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"

extern std::unique_ptr<Vulkan::Presenter> presenter;
extern std::unique_ptr<AmdGpu::Liverpool> liverpool;

namespace {
using Group = Libraries::VideoOut::BufferAttributeGroup;

void (*reply_to_guest)(const Ps4macMessage*) = nullptr;

void Reply(u64 sequence, u64 value) {
    const Ps4macMessage m{.kind = PS4MAC_REPLY, .sequence = sequence, .a = value};
    reply_to_guest(&m);
}

// Prepared frames by the guest's frame id (a Frame* may be null: no last frame to show).
std::mutex frames_mutex;
std::condition_variable frames_cv;
std::unordered_map<u64, Vulkan::Frame*> frames;

void FramePrepared(u64 id, Vulkan::Frame* frame) {
    {
        std::scoped_lock lock{frames_mutex};
        frames[id] = frame;
    }
    frames_cv.notify_all();
}

Vulkan::Frame* TakeFrame(u64 id) {
    std::unique_lock lock{frames_mutex};
    frames_cv.wait(lock, [&] { return frames.contains(id); });
    Vulkan::Frame* frame = frames[id];
    frames.erase(id);
    return frame;
}

// Presents in the last 5 s, for SPARTAN_FRAME_STATS (printed by the present thread).
u64 presented_game, presented_blank, repeats_skipped;

void PrintPresentStats() {
    static const bool stats = std::getenv("SPARTAN_FRAME_STATS") != nullptr;
    static auto window = std::chrono::steady_clock::now();
    const auto now = std::chrono::steady_clock::now();
    if (!stats || now - window < std::chrono::seconds(5)) {
        return;
    }
    const double seconds = std::chrono::duration<double>(now - window).count();
    std::fprintf(stderr, "Service present: %.1f game frames/s, %.1f blank/s, %.1f repeats/s skipped\n",
                presented_game / seconds, presented_blank / seconds, repeats_skipped / seconds);
    presented_game = presented_blank = repeats_skipped = 0;
    window = now;
}

// The present thread's work, in message order.
std::mutex present_mutex;
std::condition_variable present_cv;
std::deque<Ps4macMessage> present_queue;

void PresentThread() {
    Common::SetCurrentThreadName("ps4mac:present");
    for (;;) {
        Ps4macMessage m;
        {
            std::unique_lock lock{present_mutex};
            present_cv.wait(lock, [] { return !present_queue.empty(); });
            m = present_queue.front();
            present_queue.pop_front();
        }
        switch (m.kind) {
        case PS4MAC_GFX_PREPARE_BLANK: // from the guest's present thread (DrawBlankFrame)
            FramePrepared(m.c, presenter->PrepareBlankFrame(true));
            break;
        case PS4MAC_GFX_PREPARE_LAST:
            // The guest repeats the last frame when no flip came by a vblank. Not here: that
            // Frame is already back in the free pool, and the graphics thread may be rendering
            // the next game frame into it (flips reach the guest a bridge round trip later, so
            // repeats are frequent) - showing it flickers. The window keeps its last image.
            FramePrepared(m.c, nullptr);
            ++repeats_skipped;
            break;
        case PS4MAC_GFX_PRESENT: {
            Vulkan::Frame* frame = TakeFrame(m.a);
            if (m.c) {
                presenter->SetHDR(m.c == 2);
            }
            if (frame) {
                presenter->Present(frame, (m.b & 1) != 0, (m.b & 2) != 0);
                ++((m.b & 2) ? presented_game : presented_blank);
            }
            Reply(m.sequence, 0);
            PrintPresentStats();
            break;
        }
        default:
            break;
        }
    }
}

void ToPresentThread(const Ps4macMessage& m) {
    {
        std::scoped_lock lock{present_mutex};
        present_queue.push_back(m);
    }
    present_cv.notify_one();
}
} // namespace

namespace Ps4mac::Gfx {
bool IsServer() {
    return reply_to_guest != nullptr;
}
} // namespace Ps4mac::Gfx

extern "C" void ps4mac_gfx_server_start(void (*reply)(const Ps4macMessage*)) {
    reply_to_guest = reply;
    using Platform::InterruptId;
    static int uid;
    for (const InterruptId id :
         {InterruptId::Compute0RelMem, InterruptId::Compute1RelMem, InterruptId::Compute2RelMem,
          InterruptId::Compute3RelMem, InterruptId::Compute4RelMem, InterruptId::Compute5RelMem,
          InterruptId::Compute6RelMem, InterruptId::GfxEop, InterruptId::GfxFlip, InterruptId::GpuIdle}) {
        Platform::IrqC::Instance()->Register(
            id,
            [](InterruptId irq) {
                const Ps4macMessage m{.kind = PS4MAC_IRQ, .a = static_cast<u64>(irq)};
                reply_to_guest(&m);
            },
            &uid);
    }
    std::thread(PresentThread).detach();
}

/// One guest message (the service's ring thread). Returns 0 for kinds that are not Ps4mac::Gfx.
extern "C" int ps4mac_gfx_server_handle(const Ps4macMessage* m) {
    auto* core = liverpool.get();
    switch (m->kind) {
    case PS4MAC_SUBMIT_GFX:
        core->SubmitGfx({reinterpret_cast<const u32*>(m->a), size_t(m->b)},
                        {reinterpret_cast<const u32*>(m->c), size_t(m->d)});
        return 1;
    case PS4MAC_SUBMIT_COMPUTE:
        core->SubmitAsc(u32(m->c), {reinterpret_cast<const u32*>(m->a), size_t(m->b)});
        return 1;
    case PS4MAC_SUBMIT_DONE:
        core->SubmitDone();
        return 1;
    case PS4MAC_MAP_COMPUTE_QUEUE:
        Reply(m->sequence, core->asc_queues
                               .insert(VAddr(m->a), reinterpret_cast<u32*>(m->b), u32(m->c), u32(m->d))
                               .index);
        return 1;
    case PS4MAC_UNMAP_COMPUTE_QUEUE:
        core->asc_queues.erase(Common::SlotId{u32(m->a)});
        return 1;
    case PS4MAC_GFX_IS_IDLE:
        Reply(m->sequence, core->IsGpuIdle() ? 1 : 0);
        return 1;
    case PS4MAC_GFX_RESERVE_COPY:
        core->ReserveCopyBufferSpace();
        return 1;
    case PS4MAC_GFX_RUN_TASK:
        core->SendCommand([sequence = m->sequence] { Reply(sequence, 0); });
        return 1;
    case PS4MAC_GFX_SET_VO_PORT:
        core->SetVoPort(reinterpret_cast<Libraries::VideoOut::VideoOutPort*>(m->a));
        return 1;
    case PS4MAC_GFX_REGISTER_BUFFER:
        presenter->RegisterVideoOutSurface(*reinterpret_cast<const Group*>(m->a), m->b);
        return 1;
    case PS4MAC_GFX_PREPARE_FLIP: {
        // The group as it is now: the guest may change the port's attributes later.
        const Group group = *reinterpret_cast<const Group*>(m->a);
        core->SendCommand([group, address = m->b, id = m->c] {
            FramePrepared(id, presenter->PrepareFrame(group, address));
        });
        return 1;
    }
    case PS4MAC_GFX_PREPARE_BLANK:
        if (m->a) {
            ToPresentThread(*m);
        } else {
            core->SendCommand([id = m->c] { FramePrepared(id, presenter->PrepareBlankFrame(false)); });
        }
        return 1;
    case PS4MAC_GFX_PREPARE_LAST:
    case PS4MAC_GFX_PRESENT:
        ToPresentThread(*m);
        return 1;
    case PS4MAC_GFX_SET_GAMMA:
        presenter->GetPPSettingsRef().gamma = std::bit_cast<float>(u32(m->a));
        return 1;
    default:
        return 0;
    }
}

/// The guest's VideoOutPort layout must match this build's (the port is shared memory).
extern "C" int ps4mac_gfx_server_check_port(uint32_t size, uint32_t labels, uint32_t groups, uint32_t slots) {
    alignas(Libraries::VideoOut::VideoOutPort) static unsigned char storage[sizeof(Libraries::VideoOut::VideoOutPort)];
    const auto* port = reinterpret_cast<const Libraries::VideoOut::VideoOutPort*>(storage);
    const auto offset = [&](const void* field) {
        return uint32_t(static_cast<const unsigned char*>(field) - storage);
    };
    const bool ok = size == sizeof(*port) && labels == offset(&port->buffer_labels) &&
                    groups == offset(&port->groups) && slots == offset(&port->buffer_slots);
    if (!ok) {
        std::fprintf(stderr,
                     "ps4mac_service: VideoOutPort layout differs: guest size %u labels %u groups %u "
                     "slots %u, service %zu %u %u %u\n",
                     size, labels, groups, slots, sizeof(*port), offset(&port->buffer_labels),
                     offset(&port->groups), offset(&port->buffer_slots));
    }
    return ok;
}

extern "C" int ps4mac_gfx_server_hdr_supported(void) {
    return presenter->IsHDRSupported() ? 1 : 0;
}

std::atomic<u64> fault_drain_ns{0}, gpu_modified_faults{0};
extern "C" uint64_t ps4mac_gfx_server_gpu_modified_faults(void) {
    return gpu_modified_faults.load(std::memory_order_relaxed);
}
extern "C" uint64_t ps4mac_gfx_server_fault_drain_ns(void) {
    return fault_drain_ns.load(std::memory_order_relaxed);
}

/// Step 3.4: a guest access to a page the GPU protects (PS4MAC_WRITE_FAULT/READ_FAULT), handled as
/// page_manager.cpp's fault handler does in one process. Returns whether it was such a page.
extern "C" int ps4mac_gfx_server_fault(uintptr_t address, int is_write) {
    auto& rasterizer = presenter->GetRasterizer();
    if (is_write) {
        SpartanStats::Timer timer{SpartanStats::t_write_faults};
        // Step 3.6: faults on pages the GPU wrote (they download first, on the graphics thread),
        // timed apart from the others.
        rasterizer.DrainDrawPipe();
        const bool gpu_modified = rasterizer.IsRegionGpuModified(VAddr(address), 8);
        const auto start = std::chrono::steady_clock::now();
        const int handled = rasterizer.OnWriteFault(VAddr(address), false) ? 1 : 0;
        if (gpu_modified) {
            fault_drain_ns.fetch_add(u64(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                             std::chrono::steady_clock::now() - start).count()),
                                     std::memory_order_relaxed);
            gpu_modified_faults.fetch_add(1, std::memory_order_relaxed);
        }
        return handled;
    }
    SpartanStats::read_faults.fetch_add(1, std::memory_order_relaxed);
    SpartanStats::Timer timer{SpartanStats::t_read_faults};
    return rasterizer.ReadMemory(VAddr(address), 8, false) ? 1 : 0;
}
