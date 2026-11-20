// SPDX-License-Identifier: GPL-2.0-or-later
// ps4mac_port: the graphics side of the bridge as the guest-side HLE code sees it (GnmDriver now;
// VideoOut next). In one process it calls the video core (AmdGpu::Liverpool) directly; the
// two-process build implements the same functions with bridge/ps4mac_bridge.h messages.
#pragma once

#include <cstdint>
#include <span>

namespace AmdGpu {
class Liverpool;
}

namespace Ps4mac::Gfx {

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

} // namespace Ps4mac::Gfx
