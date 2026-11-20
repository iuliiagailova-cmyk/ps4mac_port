// SPDX-License-Identifier: GPL-2.0-or-later
// ps4mac_port: in-process implementation of the graphics bridge (see ps4mac_graphics.h).
#include "ps4mac_graphics.h"

#include "video_core/amdgpu/liverpool.h"
#include "video_core/renderer_vulkan/vk_presenter.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"

extern std::unique_ptr<Vulkan::Presenter> presenter;

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

void SetVideoOutPort(Libraries::VideoOut::VideoOutPort* port) {
    core->SetVoPort(port);
}

void RegisterDisplayBuffer(const Libraries::VideoOut::BufferAttributeGroup& group,
                           std::uintptr_t address) {
    presenter->RegisterVideoOutSurface(group, address);
}

FrameHandle PrepareFlip(const Libraries::VideoOut::BufferAttributeGroup& group,
                        std::uintptr_t address) {
    return presenter->PrepareFrame(group, address);
}

FrameHandle PrepareBlankFrame(bool present_thread) {
    return presenter->PrepareBlankFrame(present_thread);
}

FrameHandle PrepareLastFrame() {
    return presenter->PrepareLastFrame();
}

void SetHdr(bool enable) {
    presenter->SetHDR(enable);
}

void Present(FrameHandle frame, bool is_reusing_frame, bool is_game_frame) {
    presenter->Present(static_cast<Vulkan::Frame*>(frame), is_reusing_frame, is_game_frame);
}

bool IsHdrSupported() {
    return presenter->IsHDRSupported();
}

void SetGamma(float gamma) {
    presenter->GetPPSettingsRef().gamma = gamma;
}

void RunOnGraphicsThread(std::function<void()> task) {
    core->SendCommand(std::move(task));
}

void ReportCompilerStats() {
    presenter->GetRasterizer().GetPipelineCache().ReportCompilerStats();
}

} // namespace Ps4mac::Gfx
