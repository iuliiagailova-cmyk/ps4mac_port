// SPDX-License-Identifier: GPL-2.0-or-later
// ps4mac_port: the graphics side of the bridge as the guest-side HLE code sees it (GnmDriver now;
// VideoOut next). In one process it calls the video core (AmdGpu::Liverpool) directly; the
// two-process build implements the same functions with bridge/ps4mac_bridge.h messages.
#pragma once

#include <cstdint>
#include <functional>
#include <span>

namespace AmdGpu {
class Liverpool;
}
namespace Libraries::VideoOut {
struct BufferAttributeGroup;
struct VideoOutPort;
} // namespace Libraries::VideoOut

struct Ps4macInput; // bridge/ps4mac_bridge.h (step 3.5)

namespace Ps4mac::Gfx {

/// The graphics service renders (step 3.3c): calls go over the bridge; this process has no
/// video core, presenter or window.
bool Remote();
/// Remote: receives the service's messages and waits until its video core answers.
void StartRemote();
/// This process is the graphics service, answering a remote guest (ps4mac_graphics_server.cpp).
bool IsServer();

/// The video core this process talks to (set when GnmDriver creates it).
void Attach(AmdGpu::Liverpool* liverpool);

void SubmitGfx(std::span<const std::uint32_t> dcb, std::span<const std::uint32_t> ccb);
void SubmitCompute(std::uint32_t gnm_vqid, std::span<const std::uint32_t> acb);
/// sceGnmSubmitDone: the frame's submissions are complete.
void SubmitDone();
/// No submission queued or being processed (and, with work retirement, none in flight).
bool IsIdle();
void ReserveCopyBufferSpace();

/// A guest compute ring (sceGnmMapComputeQueue); returns the 0-based queue id.
struct ComputeQueue {
    std::uintptr_t ring_base;
    std::uint32_t* read_pointer;
    std::uint32_t ring_size_dw;
};
std::uint32_t MapComputeQueue(std::uintptr_t ring_base, std::uint32_t* read_pointer,
                              std::uint32_t ring_size_dw, std::uint32_t pipe_id);
void UnmapComputeQueue(std::uint32_t vqid);
ComputeQueue GetComputeQueue(std::uint32_t vqid);

// --- Presentation (VideoOut). A prepared frame stays on the graphics side: the guest side only
// holds its handle until it is presented.
using FrameHandle = void*;
/// The GPU end-of-pipe flips (EventWriteEop with a flip) report to this port.
void SetVideoOutPort(Libraries::VideoOut::VideoOutPort* port);
void RegisterDisplayBuffer(const Libraries::VideoOut::BufferAttributeGroup& group,
                           std::uintptr_t address);
FrameHandle PrepareFlip(const Libraries::VideoOut::BufferAttributeGroup& group,
                        std::uintptr_t address);
FrameHandle PrepareBlankFrame(bool present_thread);
FrameHandle PrepareLastFrame();
void SetHdr(bool enable);
void Present(FrameHandle frame, bool is_reusing_frame = false, bool is_game_frame = true);
bool IsHdrSupported();
void SetGamma(float gamma);
/// Runs `task` on the graphics command thread, in order with submissions.
void RunOnGraphicsThread(std::function<void()> task);
void ReportCompilerStats();

// --- Step 3.5: input and the in-game menu live with the service's window.
/// Guest: the service's window input in the shared block once the service publishes it, else
/// nullptr (one process, or not ready yet).
const Ps4macInput* GuestInput();
/// Guest: a pad shortcut the service's window and menu act on (Ps4macUiAction).
void UiAction(int action);
/// Service: the window thread publishes its input after handling its events.
void PublishInput(void* sdl_window);

} // namespace Ps4mac::Gfx
