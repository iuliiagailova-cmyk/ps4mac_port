// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cstdlib>

#include "video_core/renderer_vulkan/vk_gpu_profiler.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"

namespace Vulkan {

void GpuProfiler::Init(const Instance& instance, Scheduler& scheduler) {
    const char* env = std::getenv("SPARTAN_GPU_PROFILE");
    if (!env || (env[0] != '1' && env[0] != 'r' && env[0] != 'l') || instance_ptr) {
        return;
    }
    instance_ptr = new GpuProfiler(instance, scheduler);
    instance_ptr->labels_only = env[0] == 'l';
    if (instance_ptr->labels_only) {
        std::printf("GPU profile: labels for Metal System Trace (KK_LABEL_ENCODERS=1)\n");
    }
}

GpuProfiler::GpuProfiler(const Instance& instance, Scheduler& scheduler_)
    : device{instance.GetDevice()}, scheduler{scheduler_} {
    const vk::QueryPoolCreateInfo info = {
        .queryType = vk::QueryType::eTimestamp,
        .queryCount = NumSlices * SliceQueries,
    };
    pool = Check<"create timestamp query pool">(device.createQueryPoolUnique(info));
    device.resetQueryPool(*pool, 0, NumSlices * SliceQueries);
    period_ns = instance.GetPhysicalDevice().getProperties().limits.timestampPeriod;
    for (auto& k : keys) {
        k.reserve(SliceQueries);
    }
    std::printf("GPU profile: on (timestamp period %.2f ns)\n", period_ns);
}

void GpuProfiler::WriteTimestamp(u64 key) {
    if (paused || used[slice] + 1 >= SliceQueries) {
        return; // the frame's slice is full: the rest of the frame goes to the last label
    }
    // Outside render passes: radv_CmdWriteTimestamp2 crashed inside some. Marks sit where a
    // pass, dispatch or submission ends anyway.
    scheduler.EndRendering();
    const u32 query = slice * SliceQueries + used[slice]++;
    keys[slice].push_back(key);
    current = key;
    scheduler.Record([pool = *pool, query](vk::CommandBuffer cmdbuf) {
        cmdbuf.writeTimestamp2(vk::PipelineStageFlagBits2::eAllCommands, pool, query);
    });
}

void GpuProfiler::SetLabel(const std::string& label) {
    scheduler.SetPendingLabel(label);
}

void GpuProfiler::BeginFrame() {
    if (labels_only) {
        return;
    }
    // Close the frame: one more timestamp without a label.
    if (!paused && used[slice] > 0 && used[slice] < SliceQueries) {
        scheduler.EndRendering();
        const u32 query = slice * SliceQueries + used[slice]++;
        scheduler.Record([pool = *pool, query](vk::CommandBuffer cmdbuf) {
            cmdbuf.writeTimestamp2(vk::PipelineStageFlagBits2::eAllCommands, pool, query);
        });
        pending[slice] = true;
        ticks[slice] = scheduler.CurrentTick();
    }
    // The oldest slice, four frames back. While the GPU has not finished it (or its command
    // buffer is not even submitted), its queries can be neither read nor reset: skip this
    // frame's timestamps instead of waiting, which deadlocked during video playback.
    const u32 next = (slice + 1) % NumSlices;
    paused = pending[next] && !scheduler.IsFree(ticks[next]);
    if (paused) {
        Print();
        return;
    }
    slice = next;
    if (pending[slice]) {
        Collect(slice);
    }
    if (used[slice]) {
        device.resetQueryPool(*pool, slice * SliceQueries, used[slice]);
    }
    used[slice] = 0;
    keys[slice].clear();
    pending[slice] = false;
    Print();
}

void GpuProfiler::Collect(u32 which) {
    const u32 count = used[which];
    std::vector<u64> stamps(count);
    // BeginFrame only collects a slice the GPU has finished, so the results are there.
    const auto result = device.getQueryPoolResults(
        *pool, which * SliceQueries, count, count * sizeof(u64), stamps.data(), sizeof(u64),
        vk::QueryResultFlagBits::e64);
    if (result != vk::Result::eSuccess) {
        return;
    }
    // ps4mac_port: KosmicKrisp leaves some timestamps unwritten (0 or its unavailable sentinel)
    // and others out of order; such segments are counted apart instead of summed.
    static int raw_dumps = [] {
        const char* env = std::getenv("SPARTAN_GPU_PROFILE");
        return env && env[0] == 'r' ? 2 : 0;
    }();
    if (raw_dumps > 0) {
        --raw_dumps;
        std::printf("GPU profile raw stamps (%u):", count);
        for (u32 i = 0; i < count; ++i) {
            std::printf(" %llx", static_cast<unsigned long long>(stamps[i]));
        }
        std::printf("\n");
    }
    for (u32 i = 0; i + 1 < count; ++i) {
        const double ms = double(s64(stamps[i + 1] - stamps[i])) * period_ns / 1e6;
        if (!stamps[i] || !stamps[i + 1] || ms < 0 || ms > 1000) {
            ++invalid_segments;
            continue;
        }
        auto& total = totals[keys[which][i]];
        total.ms += ms;
        ++total.segments;
    }
    ++frames;
}

void GpuProfiler::Print() {
    const auto now = std::chrono::steady_clock::now();
    if (now - window < std::chrono::seconds(5) || frames == 0) {
        return;
    }
    window = now;
    std::vector<std::pair<u64, Total>> sorted(totals.begin(), totals.end());
    std::ranges::sort(sorted, [](const auto& a, const auto& b) { return a.second.ms > b.second.ms; });
    double sum = 0;
    for (const auto& [key, total] : sorted) {
        sum += total.ms;
    }
    std::printf("GPU profile: %.2f ms/frame over %llu frames, %zu labels, %.1f invalid segments/frame\n",
                sum / frames, static_cast<unsigned long long>(frames), sorted.size(),
                double(invalid_segments) / frames);
    invalid_segments = 0;
    for (size_t i = 0; i < std::min<size_t>(sorted.size(), 30); ++i) {
        const auto& [key, total] = sorted[i];
        std::printf("  %6.3f ms/frame %5.1f/frame  %s\n", total.ms / frames,
                    double(total.segments) / frames, described[key].c_str());
    }
    totals.clear();
    frames = 0;
}

} // namespace Vulkan
