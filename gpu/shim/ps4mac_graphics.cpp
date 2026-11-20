// SPDX-License-Identifier: GPL-2.0-or-later
// ps4mac_port: in-process implementation of the graphics bridge (see ps4mac_graphics.h).
#include "ps4mac_graphics.h"

#include "video_core/amdgpu/liverpool.h"

namespace Ps4mac::Gfx {
namespace {
AmdGpu::Liverpool* core = nullptr;
}

void Attach(AmdGpu::Liverpool* liverpool) {
    core = liverpool;
}

void SubmitGfx(std::span<const std::uint32_t> dcb, std::span<const std::uint32_t> ccb) {
    core->SubmitGfx(dcb, ccb);
}

void SubmitCompute(std::uint32_t gnm_vqid, std::span<const std::uint32_t> acb) {
    core->SubmitAsc(gnm_vqid, acb);
}

void SubmitDone() {
    core->SubmitDone();
}

bool IsIdle() {
    return core->IsGpuIdle();
}

void ReserveCopyBufferSpace() {
    core->ReserveCopyBufferSpace();
}

std::uint32_t MapComputeQueue(std::uintptr_t ring_base, std::uint32_t* read_pointer,
                              std::uint32_t ring_size_dw, std::uint32_t pipe_id) {
    return core->asc_queues.insert(VAddr(ring_base), read_pointer, ring_size_dw, pipe_id).index;
}

void UnmapComputeQueue(std::uint32_t vqid) {
    core->asc_queues.erase(Common::SlotId{vqid});
}

ComputeQueue GetComputeQueue(std::uint32_t vqid) {
    const auto& queue = core->asc_queues[Common::SlotId{vqid}];
    return {queue.map_addr, queue.read_addr, queue.ring_size_dw};
}

} // namespace Ps4mac::Gfx
