// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "spartanport_trace.h"
#include <chrono>
#include <array>
#include <map>
#include <unordered_map>
#include <algorithm>
#include <bit>
#include <cstdlib>
#include <magic_enum/magic_enum.hpp>
#include "spartanport_copy.h"
#include "spartanport_toggles.h"
#include "core/emulator_settings.h"
#include "common/alignment.h"
#include "core/memory.h"
#include "video_core/amdgpu/liverpool.h"
#include "video_core/buffer_cache/buffer.h"
#include "video_core/buffer_cache/buffer_cache.h"
#include "video_core/buffer_cache/memory_tracker.h"
#include "video_core/renderer_vulkan/vk_graphics_pipeline.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/texture_cache/texture_cache.h"

#include <vk_mem_alloc.h>

namespace VideoCore {

static constexpr size_t GDS_BUFFER_SIZE = 64_KB;
static constexpr size_t STREAM_BUFFER_SIZE = 128_MB;

static constexpr auto ARENA_USAGE =
    vk::BufferUsageFlagBits::eTransferSrc | vk::BufferUsageFlagBits::eTransferDst |
    vk::BufferUsageFlagBits::eUniformBuffer | vk::BufferUsageFlagBits::eStorageBuffer |
    vk::BufferUsageFlagBits::eIndexBuffer | vk::BufferUsageFlagBits::eVertexBuffer |
    vk::BufferUsageFlagBits::eIndirectBuffer | vk::BufferUsageFlagBits::eShaderDeviceAddress;

std::optional<u32> FindMemoryType(const vk::PhysicalDeviceMemoryProperties& properties,
                                  vk::MemoryPropertyFlags wanted, u32 memory_type_bits) {
    for (u32 i = 0; i < properties.memoryTypeCount; ++i) {
        if (((memory_type_bits >> i) & 1) == 0) {
            continue;
        }
        const auto flags = properties.memoryTypes[i].propertyFlags;
        if ((flags & wanted) == wanted) {
            return i;
        }
    }
    return std::nullopt;
}

BufferCache::BufferCache(const Vulkan::Instance& instance_, Vulkan::Scheduler& scheduler_,
                         Vulkan::Runtime& runtime_, AmdGpu::Liverpool* liverpool_,
                         TextureCache& texture_cache_, PageManager& tracker)
    : instance{instance_}, scheduler{scheduler_}, runtime{runtime_},
      staging_pool{runtime_.GetStagingPool()}, liverpool{liverpool_},
      memory{Core::Memory::Instance()}, texture_cache{texture_cache_},
      memory_tracker{std::make_unique<MemoryTracker>(tracker)},
      stream_buffer{instance, scheduler, MemoryType::Stream, STREAM_BUFFER_SIZE},
      gds_buffer{instance, 0, GDS_BUFFER_SIZE, MemoryType::Stream, "GDS Buffer"},
      memory_semaphore{instance} {
    while (arena_page_bits > MIN_ARENA_PAGE_BITS &&
           (u64{2} << arena_page_bits) > instance.GetMaxBufferSize()) {
        --arena_page_bits;
    }
    arena_page_size = u64{1} << arena_page_bits;
    num_arena_pages = u64{1} << (ADDRESS_SPACE_BITS - arena_page_bits);
    if (arena_page_bits != MAX_ARENA_PAGE_BITS) {
        LOG_INFO(Render_Vulkan, "Buffer arenas of {} MiB (driver buffer limit {} MiB)",
                 arena_page_size >> 20, instance.GetMaxBufferSize() >> 20);
    }
    const vk::BufferCreateInfo probe_ci = {
        .flags =
            vk::BufferCreateFlagBits::eSparseBinding | vk::BufferCreateFlagBits::eSparseResidency,
        .size = arena_page_size,
        .usage = ARENA_USAGE,
        .sharingMode = vk::SharingMode::eExclusive,
    };
    const vk::DeviceBufferMemoryRequirements req_info = {
        .pCreateInfo = &probe_ci,
    };
    const auto device = instance.GetDevice();
    const auto reqs = device.getBufferMemoryRequirements(req_info).memoryRequirements;
    block_size = Common::AlignUp(std::max<u64>(reqs.alignment, MIN_BLOCK_SIZE), reqs.alignment);
    ASSERT_MSG(std::popcount(block_size) == 1, "Sparse block size {} is not a power of 2",
               block_size);
    block_shift = std::bit_width(block_size) - 1;
    blocks_per_arena_page = arena_page_size / block_size;
    blocks_per_arena_page_shift = arena_page_bits - block_shift;
    arena_memory_type_index =
        FindMemoryType(instance.GetMemoryProperties(), vk::MemoryPropertyFlagBits::eDeviceLocal,
                       reqs.memoryTypeBits)
            .value();
    {
        const auto flags = instance.GetMemoryProperties().memoryTypes[arena_memory_type_index].propertyFlags;
        arena_coherent = (flags & vk::MemoryPropertyFlagBits::eHostVisible) &&
                         (flags & vk::MemoryPropertyFlagBits::eHostCoherent);
    }

    // spartanport: shaders read the page table only with direct memory access
    // (SPARTAN_DIRECT_MEMORY_ACCESS, off by default); without it a token table saves 512 MiB of
    // GPU memory, which Apple GPUs commit at allocation.
    dma_enabled = EmulatorSettings.IsDirectMemoryAccessEnabled();
    // Measured with the arm64 graphics service only; the x86 one-process build keeps copies.
#if defined(__aarch64__)
    if (const char* env = std::getenv("SPARTAN_HOST_BUFFERS"); env && env[0] == '1') {
        host_buffers = instance.IsExternalMemoryHostSupported() && !dma_enabled;
        std::printf("Host buffers: %s\n", host_buffers ? "on (guest memory used in place)"
                                                       : "unavailable (no VK_EXT_external_memory_host or DMA on)");
    }
#endif
    if (const char* env = std::getenv("SPARTAN_HOST_BUFFERS_MIN_MB")) {
        host_min_mapping = std::strtoull(env, nullptr, 10) * 1_MB;
    }
    const u64 bda_pagetable_size =
        dma_enabled ? (blocks_per_arena_page * num_arena_pages) * sizeof(vk::DeviceAddress)
                    : u64{64_KB};
    fault_manager = std::make_unique<FaultManager>(instance, scheduler, *this, block_shift,
                                                   blocks_per_arena_page * num_arena_pages);
    bda_pagetable_buffer = std::make_unique<Buffer>(
        instance, 0, bda_pagetable_size, MemoryType::DeviceLocal, "BDA Page Table Buffer");
    runtime.FillBuffer(bda_pagetable_buffer.get(), 0u, bda_pagetable_size, 0u);
}

BufferCache::~BufferCache() = default;

void BufferCache::InvalidateMemory(VAddr device_addr, u64 size, bool assume_locks) {
    memory_tracker->InvalidateRegion(device_addr, size, [this, device_addr, size, assume_locks] {
        ReadMemory(device_addr, size, true, assume_locks);
    });
}

// spartanport: the game fills its per-frame buffers (constants, skinning output) sequentially and
// every 4 KiB page cost a protection fault (~110k/s in the test scene, a fifth of each
// render worker's time in the kernel). A fault unprotects the aligned window around it instead;
// pages marked CPU-modified without being written only cost an upload when bound.
// Item: context = BufferCache, source = guest address, destination = host pointer, size,
// extra = the Buffer whose mapping holds the destination (flushed after the copy).
void BufferCache::RunGuestCopy(const SpartanCopy::Item& item) {
    auto* cache = static_cast<BufferCache*>(item.context);
    auto* dst = reinterpret_cast<u8*>(item.destination);
    cache->memory->CopySparseMemory(item.source, dst, item.size);
    auto* buffer = reinterpret_cast<Buffer*>(item.extra);
    buffer->Flush(dst - buffer->mapped_data.data(), item.size);
}

// Small guest copies run on the recording thread (it spins for work: no wakeup, and it is
// idle most of the time); PoolSmallCopies (toggle 524288) batches them for the copy threads.
void BufferCache::SmallGuestCopy(const SpartanCopy::Item& item) {
    if (scheduler.IsRecordingDeferred() && !SpartanToggle::Disabled(SpartanToggle::PoolSmallCopies)) {
        scheduler.RecordHostCopy([item] { item.run(item); });
        return;
    }
    SpartanCopy::QueueCopy(item);
}

void BufferCache::ExtendWriteFault(VAddr device_addr) {
    static const u64 window = [] {
        const char* env = std::getenv("SPARTAN_FAULT_WINDOW");
        const u64 kib = env ? std::strtoull(env, nullptr, 10) : 256;
        return std::bit_ceil(std::clamp<u64>(kib, 4, 1024)) * 1024;
    }();
    if (window <= TRACKER_BYTES_PER_PAGE || SpartanToggle::Disabled(SpartanToggle::FaultWindow)) {
        return;
    }
    memory_tracker->ExtendWriteFault(Common::AlignDown(device_addr, window), window);
}

// ps4mac_port (SPARTAN_STORE_BYPASS=1, off by default: no FPS gain measured, see below): a guest store to a page the GPU writes, but
// not to the GPU's bytes, need not wait for that GPU work. R&C's render thread bumps a counter
// 0x20 bytes from 16 bytes a shader writes every frame, and each bump waited ~25 ms for the
// frame's GPU work. The guest performs the store itself (through the backing view) and the page
// stays protected: a later read of the GPU's bytes still faults and waits, as it must (fences are
// written when the work is recorded, not done, so that protection is what keeps reads correct).
// Measured in R&C's fight (21.11.2026): slow faults 25 -> 6-13 ms a frame, frame time unchanged
// (36-46 ms either way): the render thread then spins in its own wait for the GPU's fence.
bool BufferCache::StoreMayBypass(VAddr address, u64 size) {
    static const bool enabled = [] {
        const char* env = std::getenv("SPARTAN_STORE_BYPASS");
        return env && env[0] == '1';
    }();
    if (!enabled) {
        return false;
    }
    const VAddr page_start = Common::AlignDown(address, TRACKER_BYTES_PER_PAGE);
    const VAddr page_end = Common::AlignUp(address + size, TRACKER_BYTES_PER_PAGE);
    bool any = false, overlaps = false, unknown = false;
    u64 newest = 0;
    gpu_modified_ranges.ForEachInRange(page_start, page_end - page_start, [&](VAddr start, VAddr end) {
        if (start >= end) {
            return;
        }
        any = true;
        overlaps |= start < address + size && address < end;
        for (u64 block = start >> WRITE_TICK_SHIFT; block <= (end - 1) >> WRITE_TICK_SHIFT; ++block) {
            const auto it = write_ticks.find(block);
            if (it == write_ticks.end()) {
                unknown = true;
            } else {
                newest = std::max(newest, it->second);
            }
        }
    });
    // Nothing known, the GPU's own bytes, or a writer already done (the download is quick and
    // the page becomes the CPU's, as before): the usual path.
    if (!any || overlaps || unknown || scheduler.IsFree(newest)) {
        return false;
    }
    // A writer still being recorded is submitted now (without waiting): the download's wait
    // used to submit it, and work left unsubmitted kept a guest storing in a loop at start-up
    // faulting ~20k times a second. This runs on the GPU command thread with the pipe drained.
    if (newest >= scheduler.CurrentTick()) {
        SpartanTrace::Scope trace_scope{"store bypass: submit writer"};
        scheduler.Flush();
    }
    SpartanTrace::Mark("guest store bypasses GPU download", address);
    return true;
}

void BufferCache::ReadMemory(VAddr device_addr, u64 size, bool is_write, bool assume_locks) {
    const u32 store_size = is_write ? fault_store_size : 0;
    bool store_bypassed = false;
    const auto flush_request = [this, device_addr, size, is_write, store_size, &store_bypassed] {
        if (store_size && StoreMayBypass(device_addr, store_size)) {
            store_bypassed = true; // the page stays GPU-modified and protected
            return;
        }
        if (host_buffers) {
            SyncHostRegions();
            if (const HostRegion* region = FindHostRegion(device_addr, size)) {
                DownloadHost(device_addr, size, *region);
                if (is_write) {
                    memory_tracker->MarkRegionAsCpuModified(device_addr, size);
                }
                return;
            }
        }
        const u32 first_block = device_addr >> block_shift;
        const u32 last_block = (device_addr + size - 1) >> block_shift;
        const auto* arena = GetArena(first_block, last_block);

        // GPU-modified ranges come as many small scattered islands,
        // so the download is widened to a window around the request
        constexpr u64 WindowSize = 512_KB;
        const VAddr arena_end = arena->cpu_addr + arena->size_bytes;
        const VAddr window_start =
            std::max<VAddr>(Common::AlignDown(device_addr, WindowSize), arena->cpu_addr);
        const VAddr window_end = std::min<VAddr>(
            std::max<VAddr>(window_start + WindowSize, device_addr + size), arena_end);
        DownloadMemory(arena, window_start, window_end - window_start);
        if (is_write) {
            memory_tracker->MarkRegionAsCpuModified(device_addr, size);
        }
    };
    if (assume_locks) {
        flush_request();
    } else {
        liverpool->SendCommand<true>(std::move(flush_request));
    }
    if (store_bypassed) {
        fault_store_emulated = true;
    }
}

bool BufferCache::DownloadMapped(VAddr device_addr, u64 size) {
    static const bool enabled = [] {
        const char* env = std::getenv("SPARTAN_MAPPED_DOWNLOAD");
        return !(env && env[0] == '0');
    }();
    if (!enabled || mapped_backing.empty()) {
        return false;
    }
    // The ranges to download, and the newest tick that wrote them.
    boost::container::small_vector<std::pair<VAddr, VAddr>, 8> ranges;
    u64 newest = 0;
    bool unknown = false;
    memory_tracker->ForEachDownloadRange<false>(device_addr, size, [&](u64 address, u64 length) {
        gpu_modified_ranges.ForEachInRange(address, length, [&](VAddr start, VAddr end) {
            ranges.emplace_back(start, end);
            for (u64 block = start >> WRITE_TICK_SHIFT; block <= (end - 1) >> WRITE_TICK_SHIFT;
                 ++block) {
                const auto it = write_ticks.find(block);
                if (it == write_ticks.end()) {
                    unknown = true;
                } else {
                    newest = std::max(newest, it->second);
                }
            }
        });
    });
    if (ranges.empty()) {
        return true;
    }
    // A writer still being recorded needs the slow path (submit and wait for everything).
    if (unknown || newest >= scheduler.CurrentTick()) {
        return false;
    }
    // Every range must be backed by mapped memory.
    const auto host_of = [&](VAddr address) -> u8* {
        u8* host = nullptr;
        const u64 block = address >> block_shift;
        resident_ranges.ForEachInRange(block, block + 1, [&](const Backing& backing) {
            const auto it = mapped_backing.find(static_cast<VkDeviceMemory>(backing.memory));
            if (it != mapped_backing.end()) {
                host = it->second + backing.offset + ((block - backing.start) << block_shift) +
                       (address & ((u64{1} << block_shift) - 1));
            }
        });
        return host;
    };
    for (const auto& [start, end] : ranges) {
        for (VAddr a = start; a < end;) {
            const VAddr block_end = std::min<VAddr>(end, ((a >> block_shift) + 1) << block_shift);
            if (!host_of(a)) {
                return false;
            }
            a = block_end;
        }
    }
    if (!scheduler.IsFree(newest)) {
        scheduler.Wait(newest);
    }
    SpartanStats::mapped_downloads.fetch_add(1, std::memory_order_relaxed);
    for (const auto& [start, end] : ranges) {
        for (VAddr a = start; a < end;) {
            const VAddr block_end = std::min<VAddr>(end, ((a >> block_shift) + 1) << block_shift);
            memory->TryWriteBacking(std::bit_cast<u8*>(a), host_of(a), block_end - a);
            a = block_end;
        }
        gpu_modified_ranges.Subtract(start, end - start);
    }
    memory_tracker->UnmarkRegionAsGpuModified(device_addr, size);
    return true;
}

void BufferCache::DownloadMemory(const Buffer* arena, VAddr device_addr, u64 size) {
    if (DownloadMapped(device_addr, size)) {
        return;
    }
    SpartanStats::finish_downloads.fetch_add(1, std::memory_order_relaxed);
    boost::container::small_vector<vk::BufferCopy, 1> copies;
    u64 total_size_bytes = 0;
    const VAddr arena_base = arena->cpu_addr;
    memory_tracker->ForEachDownloadRange<false>(device_addr, size, [&](u64 address, u64 size) {
        const auto add_download = [&](VAddr start, VAddr end) {
            const u64 new_offset = start - arena_base;
            const u64 new_size = end - start;
            copies.push_back(vk::BufferCopy{
                .srcOffset = new_offset,
                .dstOffset = total_size_bytes,
                .size = new_size,
            });
            // Align up to avoid cache conflicts
            constexpr u64 align = 64ULL;
            constexpr u64 mask = ~(align - 1ULL);
            total_size_bytes += (new_size + align - 1) & mask;
        };
        gpu_modified_ranges.ForEachInRange(address, size, add_download);
        gpu_modified_ranges.Subtract(address, size);
    });
    if (total_size_bytes == 0) {
        return;
    }
    const auto download = staging_pool.Request(total_size_bytes, VideoCore::MemoryType::HostCached);
    for (auto& copy : copies) {
        copy.dstOffset += download.offset;
    }
    runtime.CopyBuffer(arena, download.buffer, copies);
    scheduler.Finish();

    download.buffer->Invalidate(download.offset, download.size);
    for (const auto& copy : copies) {
        auto* dst_addr = std::bit_cast<u8*>(arena_base + copy.srcOffset);
        memory->TryWriteBacking(dst_addr, download.mapped + (copy.dstOffset - download.offset),
                                copy.size);
    }
    memory_tracker->UnmarkRegionAsGpuModified(device_addr, size);
}

namespace {
// spartanport: SPARTAN_BUFFER_STATS=1 — how buffer bindings reach the GPU, by guest region (256 MiB),
// printed every 5 s: small read-only copies into the stream buffer, arena bindings, and the
// bytes those re-upload after CPU writes. Input for engine-level short paths.
struct BufferStats {
    struct Region {
        u64 stream_count{}, stream_bytes{}, arena_count{}, arena_bytes{}, upload_bytes{};
    };
    std::map<u64, Region> regions;
    /// Hot window: frame of the last binding per 64 KiB block, and the distribution of the
    /// frames between uses (1, 2, 3, 4, 5-8, 9-16, 17+): the ring's reuse distance.
    std::unordered_map<u64, u64> last_use;
    std::array<u64, 7> reuse{};
    std::chrono::steady_clock::time_point window = std::chrono::steady_clock::now();
    u64 calls = 0;
};
/// 256 MiB regions; 1 MiB inside the hot 0x104xxxxxxx window (the engine's frame data).
BufferStats& Stats();
void NoteHotUse(VAddr address, u32 size) {
    if ((address >> 28) != 0x104) {
        return;
    }
    auto& st = Stats();
    const u64 frame = SpartanStats::gpu_frames.load(std::memory_order_relaxed);
    for (u64 block = address >> 16; block <= (address + size - 1) >> 16; ++block) {
        auto [it, inserted] = st.last_use.try_emplace(block, frame);
        if (!inserted && it->second != frame) {
            const u64 d = frame - it->second;
            ++st.reuse[d <= 4 ? d - 1 : d <= 8 ? 4 : d <= 16 ? 5 : 6];
            it->second = frame;
        }
    }
}
u64 RegionKey(VAddr address) {
    return (address >> 28) == 0x104 ? (address >> 20) | (1ull << 40) : address >> 28;
}
bool BufferStatsEnabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("SPARTAN_BUFFER_STATS");
        return value && value[0] == '1';
    }();
    return enabled;
}
BufferStats& Stats() {
    static BufferStats stats;
    return stats;
}
void PrintBufferStats() {
    auto& st = Stats();
    if ((++st.calls & 4095) != 0) {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    const double seconds = std::chrono::duration<double>(now - st.window).count();
    if (seconds < 5.0) {
        return;
    }
    std::printf("Buffer stats (%.1f s, per second): region, stream copies/bytes, arena bindings, "
                "re-uploaded bytes\n", seconds);
    std::printf("  hot window reuse distance in frames (1,2,3,4,5-8,9-16,17+): %llu %llu %llu %llu "
                "%llu %llu %llu\n",
                (unsigned long long)st.reuse[0], (unsigned long long)st.reuse[1],
                (unsigned long long)st.reuse[2], (unsigned long long)st.reuse[3],
                (unsigned long long)st.reuse[4], (unsigned long long)st.reuse[5],
                (unsigned long long)st.reuse[6]);
    st.reuse = {};
    for (const auto& [region, r] : st.regions) {
        const u64 base = (region >> 40) ? (region & ((1ull << 40) - 1)) << 20 : region << 28;
        if ((region >> 40) && r.stream_bytes + r.upload_bytes < 1e6 * seconds) {
            continue; // quiet MiB of the hot window
        }
        std::printf("  %#012llx: %8.0f copies %8.2f MB, %8.0f arena %8.2f MB bound, %8.2f MB uploads\n",
                    static_cast<unsigned long long>(base), r.stream_count / seconds,
                    r.stream_bytes / seconds / 1e6, r.arena_count / seconds,
                    r.arena_bytes / seconds / 1e6,
                    r.upload_bytes / seconds / 1e6);
    }
    st.regions.clear();
    st.window = now;
}
} // namespace

std::pair<const Buffer*, u64> BufferCache::ObtainBuffer(VAddr device_addr, u32 size,
                                                        bool is_written, bool is_texel_buffer) {
    // ps4mac_port: waiting here only where the buffer itself needs the readback froze R&C in a
    // fight (21.11.2026): this wait also retires completed readbacks the game spins on.
    texture_cache.ResolveReadbacks(device_addr, size, true, ReadbackSource::BufferObtain);
    const bool stats = BufferStatsEnabled();
    if (stats) {
        PrintBufferStats();
        NoteHotUse(device_addr, size);
        // ps4mac_port diagnostic: memory the resident arena blocks would cover at coarser grains.
        static auto last_cover = std::chrono::steady_clock::now();
        if (const auto now = std::chrono::steady_clock::now(); now - last_cover >= std::chrono::seconds(5)) {
            last_cover = now;
            std::printf("Arena cover:");
            for (const u32 shift : {14u, 20u, 22u, 24u, 26u}) {
                u64 chunks = 0, last_chunk = ~0ULL;
                for (const auto& range : resident_ranges) {
                    const u64 first = (range.start << block_shift) >> shift;
                    const u64 last = (((range.end) << block_shift) - 1) >> shift;
                    chunks += last - first + 1 - (first == last_chunk ? 1 : 0);
                    last_chunk = last;
                }
                std::printf(" %u KiB grain %llu MiB;", 1u << (shift - 10),
                            (unsigned long long)((chunks << shift) >> 20));
            }
            std::printf("\n");
        }
    }
    // For read-only buffers use device local stream buffer to reduce renderpass breaks.
    if (!is_written && size <= STREAM_THRESHOLD && !IsRegionGpuModified(device_addr, size)) {
        if (stats) {
            auto& region = Stats().regions[RegionKey(device_addr)];
            ++region.stream_count;
            region.stream_bytes += size;
        }
        // spartanport: the guest data is copied on a copy thread, started now; submission and
        // guest-visible fences wait for it (Scheduler::WaitHostCopies).
        if (!stream_buffer.mapped_data.empty() &&
            !SpartanToggle::Disabled(SpartanToggle::DeferredStreamCopies)) {
            if (const auto offset = stream_buffer.Reserve(size, instance.UniformMinAlignment())) {
                SmallGuestCopy({
                    .run = &RunGuestCopy,
                    .context = this,
                    .source = device_addr,
                    .destination = reinterpret_cast<u64>(stream_buffer.mapped_data.data() + *offset),
                    .size = size,
                    .extra = reinterpret_cast<u64>(static_cast<Buffer*>(&stream_buffer)),
                });
                return {&stream_buffer, *offset};
            }
        }
        const auto [data, offset] = stream_buffer.Map(size, instance.UniformMinAlignment());
        memory->CopySparseMemory(device_addr, data, size);
        stream_buffer.Commit();
        return {&stream_buffer, offset};
    }
    if (host_buffers) {
        if (const auto [host, host_offset] = ObtainHostBuffer(device_addr, size); host) {
            if (stats) {
                auto& region = Stats().regions[RegionKey(device_addr)];
                ++region.arena_count;
                region.arena_bytes += size;
            }
            if (is_written) {
                // GPU-modified and protected: a guest access waits for the writer (DownloadHost).
                memory_tracker->ForEachUploadRange(device_addr, size, true, [](u64, u64) {}, [] {});
                gpu_modified_ranges.Add(device_addr, size);
                const u64 tick = scheduler.CurrentTick();
                for (u64 block = device_addr >> WRITE_TICK_SHIFT;
                     block <= (device_addr + size - 1) >> WRITE_TICK_SHIFT; ++block) {
                    u64& t = write_ticks[block];
                    t = std::max(t, tick);
                }
            } else if (is_texel_buffer) {
                SynchronizeMemoryFromImage(host, device_addr, size);
            }
            return {host, host_offset};
        }
    }
    const u64 first_block = device_addr >> block_shift;
    const u64 last_block = (device_addr + size - 1) >> block_shift;
    const auto* arena = GetArena(first_block, last_block);
    EnsureResident(arena, first_block, last_block);
    const u64 uploaded_before = SpartanStats::buffer_upload_bytes.load(std::memory_order_relaxed);
    SynchronizeMemory(arena, device_addr, size, is_written, is_texel_buffer);
    if (stats) {
        auto& region = Stats().regions[RegionKey(device_addr)];
        ++region.arena_count;
        region.arena_bytes += size;
        region.upload_bytes +=
            SpartanStats::buffer_upload_bytes.load(std::memory_order_relaxed) - uploaded_before;
    }
    if (is_written) {
        // ps4mac_port diagnostic (SPARTAN_LAT_PAGE=<hex>, with SPARTAN_TRACE): GPU writes over a page.
        static const VAddr lat_page = [] {
            const char* env = std::getenv("SPARTAN_LAT_PAGE");
            return env ? VAddr(std::strtoull(env, nullptr, 16)) : VAddr{0};
        }();
        if (lat_page && SpartanTrace::recording.load(std::memory_order_relaxed) &&
            device_addr < lat_page + 4096 && lat_page < device_addr + size) {
            SpartanTrace::Mark("GPU write over watched page: address", device_addr);
            SpartanTrace::Mark("GPU write over watched page: size", size);
            SpartanTrace::Mark("GPU write over watched page: texel", is_texel_buffer);
        }
        gpu_modified_ranges.Add(device_addr, size);
        const u64 tick = scheduler.CurrentTick();
        for (u64 block = device_addr >> WRITE_TICK_SHIFT;
             block <= (device_addr + size - 1) >> WRITE_TICK_SHIFT; ++block) {
            u64& t = write_ticks[block];
            t = std::max(t, tick);
        }
    }
    return {arena, arena->Offset(device_addr)};
}

std::pair<const Buffer*, u64> BufferCache::ObtainBufferForImage(VAddr device_addr, u32 size) {
    texture_cache.ResolveReadbacks(device_addr, size, true, ReadbackSource::BufferForImage);
    if (IsRegionGpuModified(device_addr, size)) {
        return ObtainBuffer(device_addr, size, false);
    }
    const auto staging = staging_pool.Request(size, VideoCore::MemoryType::HostUncached,
                                              instance.StorageMinAlignment());
    if (!SpartanToggle::Disabled(SpartanToggle::DeferredUploads)) {
        // spartanport: texture data is copied on the copy threads (streaming: 100+ MB per frame);
        // the upload reads the staging only after submission, which waits for the copies.
        constexpr u64 Chunk = 1_MB;
        for (u64 offset = 0; offset < staging.size; offset += Chunk) {
            const SpartanCopy::Item item{
                .run = &RunGuestCopy,
                .context = this,
                .source = device_addr + offset,
                .destination = reinterpret_cast<u64>(staging.mapped + offset),
                .size = std::min(Chunk, staging.size - offset),
                .extra = reinterpret_cast<u64>(staging.buffer),
            };
            if (staging.size < Chunk) {
                SmallGuestCopy(item);
            } else {
                SpartanCopy::Async([item] { item.run(item); });
            }
        }
        return {staging.buffer, staging.offset};
    }
    memory->CopySparseMemory(device_addr, staging.mapped, staging.size);
    staging.Flush();
    return {staging.buffer, staging.offset};
}

bool BufferCache::IsRegionCpuModified(VAddr addr, size_t size) {
    return memory_tracker->IsRegionCpuModified(addr, size);
}

bool BufferCache::IsRegionGpuModified(VAddr addr, size_t size) {
    return memory_tracker->IsRegionGpuModified(addr, size);
}

void BufferCache::ProcessFaultBuffer() {
    fault_manager->ProcessFaultBuffer();
}

void BufferCache::SynchronizeDmaBuffers() {
    for (const auto& range : resident_ranges) {
        const u64 page = range.start >> (arena_page_bits - block_shift);
        const VAddr device_addr = range.start << block_shift;
        const u64 size = (range.end - range.start) << block_shift;
        SynchronizeMemory(address_space[page], device_addr, size, false, false);
    }
}

const Buffer* BufferCache::GetArena(u64 first_block, u64 last_block) {
    const u64 first_page = first_block >> blocks_per_arena_page_shift;
    const u64 last_page = last_block >> blocks_per_arena_page_shift;
    ASSERT_MSG(last_page - first_page <= 1,
               "Buffer request cannot span more than two VA arena pages");

    const auto* first_arena = address_space[first_page];
    const auto* last_arena = address_space[last_page];
    if (first_arena == last_arena) {
        if (!first_arena) {
            const u64 base_block = Common::AlignDownPow2<u64>(first_block, blocks_per_arena_page);
            const u64 num_pages = last_page - first_page + 1;
            const auto* new_arena =
                &arenas.emplace_back(instance, base_block << block_shift,
                                     num_pages << arena_page_bits, MemoryType::Sparse);
            address_space[first_page] = new_arena;
            address_space[last_page] = new_arena;
        }
        return address_space[first_page];
    }

    LOG_WARNING(Render, "Migrating arena");

    const u64 first_addr = first_arena ? first_arena->cpu_addr : (first_page << arena_page_bits);
    const u64 first_size = first_arena ? first_arena->size_bytes : arena_page_size;
    const u64 last_size = last_arena ? last_arena->size_bytes : arena_page_size;

    const u64 base_block = first_addr >> block_shift;
    const u64 total_size = first_size + last_size;
    const u64 end_block = (first_addr + total_size) >> block_shift;
    auto* new_arena = &arenas.emplace_back(instance, first_addr, total_size, MemoryType::Sparse);
    auto* bind = BindsForArena(new_arena);
    resident_ranges.ForEachInRange(base_block, end_block, [&](const Backing& backing) {
        const u64 start = std::max(base_block, backing.start);
        const u64 end = std::min(end_block, backing.end);
        bind->binds.push_back(vk::SparseMemoryBind{
            .resourceOffset = (start - base_block) << block_shift,
            .size = (end - start) << block_shift,
            .memory = backing.memory,
            .memoryOffset = backing.offset + ((start - backing.start) << block_shift),
        });
    });

    u64 base_page = first_addr >> arena_page_bits;
    for (u32 page = 0; page < (first_size >> arena_page_bits); ++page) {
        address_space[base_page + page] = new_arena;
    }
    base_page = last_page;
    for (u32 page = 0; page < (last_size >> arena_page_bits); ++page) {
        address_space[base_page + page] = new_arena;
    }
    return new_arena;
}

void BufferCache::EnsureResident(const Buffer* arena, u64 first_block, u64 last_block) {
    u32 resident_blocks{};
    IntervalList bind_ranges;
    resident_ranges.ForEachGap(first_block, last_block + 1, [&](u64 start, u64 end) {
        resident_blocks += end - start;
        bind_ranges.Add({start, end});
    });

    if (bind_ranges.Empty()) {
        return;
    }
    SpartanStats::Timer timer{SpartanStats::t_resident};

    const vk::MemoryAllocateInfo alloc_info = {
        .allocationSize = resident_blocks << block_shift,
        .memoryTypeIndex = arena_memory_type_index,
    };
    const auto device_memory = Vulkan::Check(instance.GetDevice().allocateMemory(alloc_info));
    if (instance.GetMemoryProperties().memoryTypes[arena_memory_type_index].propertyFlags &
        vk::MemoryPropertyFlagBits::eHostVisible) {
        void* host = nullptr;
        if (instance.GetDevice().mapMemory(device_memory, 0, alloc_info.allocationSize, {}, &host) ==
                vk::Result::eSuccess &&
            host) {
            mapped_backing[static_cast<VkDeviceMemory>(device_memory)] = static_cast<u8*>(host);
        }
    }
    SpartanStats::arena_resident_bytes.fetch_add(alloc_info.allocationSize, std::memory_order_relaxed);

    boost::container::small_vector<vk::BufferCopy, 8> copies;
    const auto staging =
        staging_pool.Request((dma_enabled ? resident_blocks : 1) * sizeof(vk::DeviceAddress),
                             MemoryType::HostUncached);

    u64 memory_offset{};
    ArenaBinds* binds = BindsForArena(arena);
    auto* bda_addrs = reinterpret_cast<vk::DeviceAddress*>(staging.mapped);
    u64 offset = staging.offset;
    for (const auto& range : bind_ranges) {
        Backing backing;
        backing.start = range.start;
        backing.end = range.end;
        backing.memory = device_memory;
        backing.offset = memory_offset;
        resident_ranges.Add(backing);

        LOG_INFO(Render, "Making range start={}, end={} resident", backing.start, backing.end);

        const auto& bind = binds->binds.emplace_back(vk::SparseMemoryBind{
            .resourceOffset = (range.start << block_shift) - arena->cpu_addr,
            .size = (range.end - range.start) << block_shift,
            .memory = device_memory,
            .memoryOffset = memory_offset,
        });
        memory_offset += bind.size;

        if (!dma_enabled) {
            continue;
        }
        for (u32 block = 0; block < bind.size; block += block_size) {
            *(bda_addrs++) = arena->BufferDeviceAddress() + bind.resourceOffset + block;
        }
        const u64 copy_size = (backing.end - backing.start) * sizeof(vk::DeviceAddress);
        copies.emplace_back(offset, backing.start * sizeof(vk::DeviceAddress), copy_size);
        offset += copy_size;
    }

    if (!copies.empty()) {
        staging.Flush();
        runtime.CopyBuffer(staging.buffer, bda_pagetable_buffer.get(), copies);
    }
}

bool BufferCache::SynchronizeMemory(const Buffer* arena, VAddr device_addr, u32 size,
                                    bool is_written, bool is_texel_buffer) {
    boost::container::small_vector<vk::BufferCopy, 4> copies;
    size_t total_size_bytes{};
    const Buffer* src_buffer{};
    memory_tracker->ForEachUploadRange(
        device_addr, size, is_written,
        [&](u64 addr, u64 size) {
            copies.emplace_back(total_size_bytes, addr, size);
            total_size_bytes += size;
        },
        [&] { src_buffer = UploadCopies(arena, copies, total_size_bytes); });

    if (src_buffer) {
        runtime.CopyBuffer(src_buffer, arena, copies);
    }
    if (is_texel_buffer && !is_written) {
        return SynchronizeMemoryFromImage(arena, device_addr, size);
    }
    return false;
}

u8* BufferCache::ArenaHost(VAddr address) {
    u8* host = nullptr;
    const u64 block = address >> block_shift;
    resident_ranges.ForEachInRange(block, block + 1, [&](const Backing& backing) {
        const auto it = mapped_backing.find(static_cast<VkDeviceMemory>(backing.memory));
        if (it != mapped_backing.end()) {
            host = it->second + backing.offset + ((block - backing.start) << block_shift) +
                   (address & ((u64{1} << block_shift) - 1));
        }
    });
    return host;
}

void BufferCache::RunArenaCopy(const SpartanCopy::Item& item) {
    auto* cache = static_cast<BufferCache*>(item.context);
    cache->memory->CopySparseMemory(item.source, reinterpret_cast<u8*>(item.destination), item.size);
}

// ps4mac_port: on Apple's unified memory the arena is mapped for the CPU, so guest data goes
// straight into it. The staging path allocated a buffer as large as the upload (R&C binds huge
// buffers: 128-896 MiB at once, each allocation a stutter of 150-670 ms on 8 GB Macs) and copied
// it again on the GPU. The copies land before the next submission (host copies, as before), and
// work already submitted reads the new data the way the PS4's GPU reads guest memory directly.
// SPARTAN_DIRECT_UPLOAD=0 uses staging as before.
bool BufferCache::UploadDirect(std::span<vk::BufferCopy> copies, size_t total_size_bytes) {
    static const bool enabled = [] {
        const char* env = std::getenv("SPARTAN_DIRECT_UPLOAD");
        return !(env && env[0] == '0');
    }();
    // Only uploads larger than a staging ring block: smaller ones reuse the ring's buffers, which
    // measured faster than writing the arena (R&C fight: every upload direct was 2 FPS slower).
    static const u64 min_bytes = [] {
        const char* env = std::getenv("SPARTAN_DIRECT_UPLOAD_MIN_MB");
        return (env ? std::strtoull(env, nullptr, 10) : 16) * 1_MB;
    }();
    if (!enabled || !arena_coherent || mapped_backing.empty() || total_size_bytes < min_bytes ||
        SpartanToggle::Disabled(SpartanToggle::DeferredUploads)) {
        return false;
    }
    struct Piece {
        VAddr source;
        u8* destination;
        u64 size;
    };
    boost::container::small_vector<Piece, 16> pieces;
    for (const auto& copy : copies) {
        const VAddr end = copy.dstOffset + copy.size;
        for (VAddr a = copy.dstOffset; a < end;) {
            const VAddr block_end = std::min<VAddr>(end, ((a >> block_shift) + 1) << block_shift);
            u8* host = ArenaHost(a);
            if (!host) {
                return false;
            }
            pieces.push_back({a, host, block_end - a});
            a = block_end;
        }
    }
    SpartanStats::buffer_upload_bytes.fetch_add(total_size_bytes, std::memory_order_relaxed);
    SpartanStats::direct_upload_bytes.fetch_add(total_size_bytes, std::memory_order_relaxed);
    if (total_size_bytes < 1_MB) {
        for (const Piece& piece : pieces) {
            SmallGuestCopy({
                .run = &RunArenaCopy,
                .context = this,
                .source = piece.source,
                .destination = reinterpret_cast<u64>(piece.destination),
                .size = piece.size,
            });
        }
        return true;
    }
    using Group = boost::container::small_vector<Piece, 8>;
    auto group = std::make_shared<Group>();
    u64 group_bytes = 0;
    const auto launch = [&] {
        SpartanCopy::Async([group, memory = memory] {
            for (const Piece& piece : *group) {
                memory->CopySparseMemory(piece.source, piece.destination, piece.size);
            }
        });
    };
    for (const Piece& piece : pieces) {
        // Pieces over 1 MiB are split so that the copy threads share a large upload.
        for (u64 done = 0; done < piece.size;) {
            const u64 n = std::min<u64>(piece.size - done, 1_MB);
            group->push_back({piece.source + done, piece.destination + done, n});
            group_bytes += n;
            done += n;
            if (group_bytes >= 1_MB) {
                launch();
                group = std::make_shared<Group>();
                group_bytes = 0;
            }
        }
    }
    if (!group->empty()) {
        launch();
    }
    return true;
}

const Buffer* BufferCache::UploadCopies(const Buffer* arena, std::span<vk::BufferCopy> copies,
                                        size_t total_size_bytes) {
    if (copies.empty()) {
        return nullptr;
    }
    // Resolve before launching host copies: a copy-thread fault could deadlock submission.
    for (const auto& copy : copies) {
        texture_cache.ResolveReadbacks(copy.dstOffset, copy.size, true, ReadbackSource::BufferUpload);
    }
    if (UploadDirect(copies, total_size_bytes)) {
        return nullptr;
    }
    SpartanStats::buffer_upload_bytes.fetch_add(total_size_bytes, std::memory_order_relaxed);
    const auto staging = staging_pool.Request(total_size_bytes, MemoryType::HostUncached);
    // spartanport: the guest memory is copied into staging on the copy threads, started now in
    // groups of about 1 MiB; submission and guest-visible fences wait for them
    // (Scheduler::WaitHostCopies).
    if (!SpartanToggle::Disabled(SpartanToggle::DeferredUploads) && total_size_bytes < 1_MB) {
        // Small uploads join the calling thread's batch.
        for (auto& copy : copies) {
            SmallGuestCopy({
                .run = &RunGuestCopy,
                .context = this,
                .source = copy.dstOffset,
                .destination = reinterpret_cast<u64>(staging.mapped + copy.srcOffset),
                .size = copy.size,
                .extra = reinterpret_cast<u64>(staging.buffer),
            });
            copy.srcOffset += staging.offset;
            copy.dstOffset -= arena->cpu_addr;
        }
        return staging.buffer;
    }
    if (!SpartanToggle::Disabled(SpartanToggle::DeferredUploads)) {
        struct HostCopy {
            VAddr source;
            u8* destination;
            u64 size;
        };
        using Group = boost::container::small_vector<HostCopy, 8>;
        auto group = std::make_shared<Group>();
        u64 group_bytes = 0;
        const auto launch = [&] {
            SpartanCopy::Async([group, memory = memory, staging] {
                for (const auto& copy : *group) {
                    memory->CopySparseMemory(copy.source, copy.destination, copy.size);
                }
                staging.Flush();
            });
        };
        for (auto& copy : copies) {
            group->push_back({copy.dstOffset, staging.mapped + copy.srcOffset, copy.size});
            group_bytes += copy.size;
            copy.srcOffset += staging.offset;
            copy.dstOffset -= arena->cpu_addr;
            if (group_bytes >= 1_MB) {
                launch();
                group = std::make_shared<Group>();
                group_bytes = 0;
            }
        }
        if (!group->empty()) {
            launch();
        }
        return staging.buffer;
    }
    const auto copy = [&](std::size_t i) {
        memory->CopySparseMemory(copies[i].dstOffset, staging.mapped + copies[i].srcOffset,
                                 copies[i].size);
    };
    if (total_size_bytes >= 2_MB && copies.size() > 1) {
        SpartanCopy::ParallelFor(copies.size(), copy);
    } else {
        for (std::size_t i = 0; i < copies.size(); ++i) {
            copy(i);
        }
    }
    for (auto& copy : copies) {
        copy.srcOffset += staging.offset;
        copy.dstOffset -= arena->cpu_addr;
    }
    staging.Flush();
    return staging.buffer;
}

bool BufferCache::SynchronizeMemoryFromImage(const Buffer* arena, VAddr device_addr, u32 size) {
    if (auto type = texture_cache.IsMeta(device_addr)) {
        if (*type == TextureCache::MetaType::HTile) {
            static constexpr u32 ZmaskUncompressed = 0xf;
            runtime.FillBuffer(arena, arena->Offset(device_addr), size, ZmaskUncompressed);
            return true;
        } else {
            LOG_WARNING(Render_Vulkan, "Unhandled metadata type {}", magic_enum::enum_name(*type));
        }
    }
    // spartanport: most texel buffers alias no image; remember misses until images change.
    const u64 generation = texture_cache.RegistryGeneration();
    auto& miss = image_miss_cache[((device_addr >> 6) ^ size * 0x9E3779B1u) % image_miss_cache.size()];
    if (miss.address == device_addr && miss.size == size && miss.generation == generation &&
        !SpartanToggle::Disabled(SpartanToggle::TextureBindingMemo)) {
        return false;
    }
    const ImageId image_id = texture_cache.FindImageFromRange(device_addr, size);
    if (!image_id) {
        miss = {device_addr, size, generation};
        return false;
    }
    // spartanport: the lookups above only read what the texture binding helper leaves alone; the
    // copy below changes image state, so the helper finishes first.
    runtime.BeforeImageAccess();
    Image& image = texture_cache.GetImage(image_id);
    ASSERT_MSG(device_addr == image.info.guest_address,
               "Texel buffer aliases image subresources {:x} : {:x}", device_addr,
               image.info.guest_address);
    const u64 arena_offset = arena->Offset(device_addr);
    boost::container::small_vector<vk::BufferImageCopy, 8> buffer_copies;
    for (u32 mip = 0; mip < image.info.resources.levels; mip++) {
        const auto& mip_info = image.info.mips_layout[mip];
        const u32 width = std::max(image.info.size.width >> mip, 1u);
        const u32 height = std::max(image.info.size.height >> mip, 1u);
        const u32 depth = std::max(image.info.size.depth >> mip, 1u);
        if (arena_offset + mip_info.offset + mip_info.size > arena->size_bytes) {
            break;
        }
        buffer_copies.push_back(vk::BufferImageCopy{
            .bufferOffset = mip_info.offset,
            .bufferRowLength = mip_info.pitch,
            .bufferImageHeight = mip_info.height,
            .imageSubresource{
                .aspectMask = image.aspect_mask & ~vk::ImageAspectFlagBits::eStencil,
                .mipLevel = mip,
                .baseArrayLayer = 0,
                .layerCount = image.info.resources.layers,
            },
            .imageOffset = {0, 0, 0},
            .imageExtent = {width, height, depth},
        });
    }
    if (buffer_copies.empty()) {
        return false;
    }
    auto& tile_manager = texture_cache.GetTileManager();
    tile_manager.TileImage(image, buffer_copies, arena, arena_offset);
    return true;
}

void BufferCache::NoteGuestMap(VAddr address, u64 size) {
    if (!host_buffers) {
        return;
    }
    const VAddr end = address + size;
    std::scoped_lock lock{host_mutex};
    // A mapping over an old one (a remap) replaces its pages: buffers over them must go.
    std::erase_if(host_regions_shared, [&](const HostRegion& r) {
        if (r.start < end && address < r.end) {
            host_dropped.push_back(r);
            return true;
        }
        return false;
    });
    // SPARTAN_HOST_BUFFERS_SKIP=<hex start>,...: mappings left to the copying path (bisecting).
    static const std::vector<VAddr> skip = [] {
        std::vector<VAddr> list;
        if (const char* env = std::getenv("SPARTAN_HOST_BUFFERS_SKIP")) {
            for (const char* p = env; *p;) {
                char* next = nullptr;
                list.push_back(std::strtoull(p, &next, 16));
                p = (*next == ',') ? next + 1 : next;
                if (next == p && *p == 0) break;
            }
        }
        return list;
    }();
    if (size >= host_min_mapping && (address & 16383) == 0 && (size & 16383) == 0 &&
        std::ranges::find(skip, address) == skip.end()) {
        const auto at = std::ranges::lower_bound(host_regions_shared, address, {}, &HostRegion::start);
        host_regions_shared.insert(at, HostRegion{address, end});
    }
    host_changed.store(true, std::memory_order_release);
}

void BufferCache::NoteGuestUnmap(VAddr address, u64 size) {
    if (!host_buffers) {
        return;
    }
    const VAddr end = address + size;
    std::scoped_lock lock{host_mutex};
    std::erase_if(host_regions_shared, [&](const HostRegion& r) {
        if (r.start < end && address < r.end) {
            host_dropped.push_back(r);
            return true;
        }
        return false;
    });
    host_dropped.push_back({address, end});
    host_changed.store(true, std::memory_order_release);
}

void BufferCache::SyncHostRegions() {
    if (!host_changed.exchange(false, std::memory_order_acq_rel)) {
        return;
    }
    std::vector<HostRegion> dropped;
    {
        std::scoped_lock lock{host_mutex};
        host_regions = host_regions_shared;
        dropped.swap(host_dropped);
    }
    // Buffers over dropped ranges live until the GPU is done with the work recorded so far.
    for (auto it = host_spans.begin(); it != host_spans.end();) {
        const VAddr start = it->second->CpuAddr();
        const VAddr end = start + it->second->SizeBytes();
        const bool gone = std::ranges::any_of(
            dropped, [&](const HostRegion& r) { return r.start < end && start < r.end; });
        if (gone) {
            scheduler.DeferOperation([buffer = it->second] {});
            it = host_spans.erase(it);
        } else {
            ++it;
        }
    }
}

const BufferCache::HostRegion* BufferCache::FindHostRegion(VAddr address, u64 size, bool* straddles) {
    auto it = std::ranges::upper_bound(host_regions, address, {}, &HostRegion::start);
    if (it == host_regions.begin()) {
        return nullptr;
    }
    --it;
    if (address >= it->end) {
        return nullptr;
    }
    if (address + size > it->end) {
        if (straddles) {
            *straddles = true;
        }
        return nullptr;
    }
    return &*it;
}

std::pair<const Buffer*, u64> BufferCache::ObtainHostBuffer(VAddr device_addr, u64 size) {
    SyncHostRegions();
    bool straddles = false;
    const HostRegion* region = FindHostRegion(device_addr, size, &straddles);
    if (!region) {
        host_straddles += straddles;
        return {nullptr, 0};
    }
    const VAddr start = std::max<VAddr>(region->start, Common::AlignDown(device_addr, 1ull << HOST_CHUNK_SHIFT));
    const VAddr end = std::min<VAddr>(region->end,
                                      Common::AlignUp(device_addr + size, 1ull << HOST_CHUNK_SHIFT));
    const u64 key = (start >> 14) << 24 | ((end - start) >> HOST_CHUNK_SHIFT);
    auto [it, inserted] = host_spans.try_emplace(key);
    if (inserted) {
        it->second = std::make_shared<Buffer>(instance, start, end - start,
                                              reinterpret_cast<void*>(start));
        if (++host_created <= 32 || (host_created & 255) == 0) {
            std::printf("Host buffers: %llu created (%#llx, %llu MiB), %llu bindings straddled\n",
                        (unsigned long long)host_created, (unsigned long long)start,
                        (unsigned long long)((end - start) >> 20),
                        (unsigned long long)host_straddles);
        }
    }
    return {it->second.get(), device_addr - start};
}

void BufferCache::DownloadHost(VAddr device_addr, u64 size, const HostRegion& region) {
    // The GPU wrote guest memory itself: wait for the newest writer of these pages, then they
    // are the guest's again (unprotected). Same window as the copying path.
    constexpr u64 WindowSize = 512_KB;
    const VAddr window_start = std::max<VAddr>(Common::AlignDown(device_addr, WindowSize), region.start);
    const VAddr window_end =
        std::min<VAddr>(std::max<VAddr>(window_start + WindowSize, device_addr + size), region.end);
    u64 newest = 0;
    gpu_modified_ranges.ForEachInRange(window_start, window_end - window_start,
                                       [&](VAddr start, VAddr end) {
        if (start >= end) {
            return;
        }
        for (u64 block = start >> WRITE_TICK_SHIFT; block <= (end - 1) >> WRITE_TICK_SHIFT; ++block) {
            if (const auto it = write_ticks.find(block); it != write_ticks.end()) {
                newest = std::max(newest, it->second);
            }
        }
    });
    if (newest && !scheduler.IsFree(newest)) {
        scheduler.Wait(newest);
    }
    gpu_modified_ranges.Subtract(window_start, window_end - window_start);
    memory_tracker->UnmarkRegionAsGpuModified(window_start, window_end - window_start);
}

void BufferCache::SubmitPendingArenaBinds(Vulkan::SubmitInfo& info) {
    if (pending_binds.empty()) {
        return;
    }

    std::vector<vk::SparseBufferMemoryBindInfo> buffer_binds;
    buffer_binds.reserve(pending_binds.size());

    for (const auto& binds : pending_binds) {
        buffer_binds.emplace_back(vk::SparseBufferMemoryBindInfo{
            .buffer = binds.arena->Handle(),
            .bindCount = static_cast<u32>(binds.binds.size()),
            .pBinds = binds.binds.data(),
        });
    }

    const u64 signal_tick = memory_semaphore.NextTick();
    const auto signal_sema = memory_semaphore.Handle();

    const vk::TimelineSemaphoreSubmitInfo timeline_si = {
        .signalSemaphoreValueCount = 1u,
        .pSignalSemaphoreValues = &signal_tick,
    };

    const vk::BindSparseInfo sparse_info = {
        .pNext = &timeline_si,
        .bufferBindCount = static_cast<u32>(buffer_binds.size()),
        .pBufferBinds = buffer_binds.data(),
        .signalSemaphoreCount = 1u,
        .pSignalSemaphores = &signal_sema,
    };

    info.AddWait(signal_sema, signal_tick);
    auto submit_result = instance.GetSparseQueue().bindSparse(sparse_info);
    ASSERT_MSG(submit_result != vk::Result::eErrorDeviceLost, "Device lost during submit");

    pending_binds.clear();
}

} // namespace VideoCore
