// SPDX-License-Identifier: GPL-2.0-or-later
// ps4mac_port step 3.1: the native arm64 graphics service's first light. Opens an SDL window,
// creates a Vulkan device on KosmicKrisp (arm64 build) and clears the swapchain for a few
// seconds, timing each frame. Proves window -> Vulkan -> Metal without Rosetta 2.
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <vulkan/vulkan.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <vector>

#define CHECK(x)                                                                                   \
    do {                                                                                           \
        VkResult r_ = (x);                                                                         \
        if (r_ != VK_SUCCESS) {                                                                    \
            std::fprintf(stderr, "%s failed: %d\n", #x, r_);                                       \
            std::exit(1);                                                                          \
        }                                                                                          \
    } while (0)

int main(int argc, char** argv) {
    const double seconds = argc > 1 ? std::atof(argv[1]) : 3.0;
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    SDL_Window* window = SDL_CreateWindow("ps4mac_port service (arm64)", 960, 540, SDL_WINDOW_VULKAN);
    if (!window) {
        std::fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError());
        return 1;
    }
    Uint32 ext_count = 0;
    const char* const* sdl_exts = SDL_Vulkan_GetInstanceExtensions(&ext_count);
    std::vector<const char*> exts(sdl_exts, sdl_exts + ext_count);
    exts.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "ps4mac_service_smoke";
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.flags = VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
    ici.pApplicationInfo = &app;
    ici.enabledExtensionCount = uint32_t(exts.size());
    ici.ppEnabledExtensionNames = exts.data();
    VkInstance instance;
    CHECK(vkCreateInstance(&ici, nullptr, &instance));

    uint32_t gpu_count = 1;
    VkPhysicalDevice gpu;
    CHECK(vkEnumeratePhysicalDevices(instance, &gpu_count, &gpu));
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(gpu, &props);
    std::printf("GPU: %s, Vulkan %u.%u.%u, driver %#x\n", props.deviceName,
                VK_API_VERSION_MAJOR(props.apiVersion), VK_API_VERSION_MINOR(props.apiVersion),
                VK_API_VERSION_PATCH(props.apiVersion), props.driverVersion);

    VkSurfaceKHR surface;
    if (!SDL_Vulkan_CreateSurface(window, instance, nullptr, &surface)) {
        std::fprintf(stderr, "SDL_Vulkan_CreateSurface: %s\n", SDL_GetError());
        return 1;
    }
    const float priority = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = 0;
    qci.queueCount = 1;
    qci.pQueuePriorities = &priority;
    const char* dev_exts[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME}; // KosmicKrisp is conformant: no portability subset
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = 1;
    dci.ppEnabledExtensionNames = dev_exts;
    VkDevice device;
    CHECK(vkCreateDevice(gpu, &dci, nullptr, &device));
    VkQueue queue;
    vkGetDeviceQueue(device, 0, 0, &queue);

    VkSurfaceCapabilitiesKHR caps;
    CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(gpu, surface, &caps));
    uint32_t format_count = 1;
    VkSurfaceFormatKHR format;
    vkGetPhysicalDeviceSurfaceFormatsKHR(gpu, surface, &format_count, &format);
    VkSwapchainCreateInfoKHR sci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    sci.surface = surface;
    sci.minImageCount = caps.maxImageCount ? std::min(caps.minImageCount + 1, caps.maxImageCount) : caps.minImageCount + 1;
    sci.imageFormat = format.format;
    sci.imageColorSpace = format.colorSpace;
    sci.imageExtent = caps.currentExtent;
    sci.imageArrayLayers = 1;
    sci.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    sci.preTransform = caps.currentTransform;
    sci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    sci.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    sci.clipped = VK_TRUE;
    VkSwapchainKHR swapchain;
    CHECK(vkCreateSwapchainKHR(device, &sci, nullptr, &swapchain));
    uint32_t image_count = 0;
    vkGetSwapchainImagesKHR(device, swapchain, &image_count, nullptr);
    std::vector<VkImage> images(image_count);
    vkGetSwapchainImagesKHR(device, swapchain, &image_count, images.data());

    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    VkCommandPool pool;
    CHECK(vkCreateCommandPool(device, &pci, nullptr, &pool));
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = pool;
    cai.commandBufferCount = 1;
    VkCommandBuffer cmd;
    CHECK(vkAllocateCommandBuffers(device, &cai, &cmd));
    VkSemaphoreCreateInfo semi{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VkSemaphore acquired, rendered;
    CHECK(vkCreateSemaphore(device, &semi, nullptr, &acquired));
    CHECK(vkCreateSemaphore(device, &semi, nullptr, &rendered));
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    VkFence fence;
    CHECK(vkCreateFence(device, &fci, nullptr, &fence));

    const auto start = std::chrono::steady_clock::now();
    unsigned frames = 0;
    double worst_ms = 0;
    auto last = start;
    for (bool running = true; running;) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_QUIT) running = false;
        }
        const double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        if (t >= seconds) break;
        CHECK(vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX));
        CHECK(vkResetFences(device, 1, &fence));
        uint32_t index;
        CHECK(vkAcquireNextImageKHR(device, swapchain, UINT64_MAX, acquired, VK_NULL_HANDLE, &index));
        vkResetCommandBuffer(cmd, 0);
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        vkBeginCommandBuffer(cmd, &bi);
        VkImageMemoryBarrier to_dst{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        to_dst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        to_dst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        to_dst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        to_dst.image = images[index];
        to_dst.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                             0, nullptr, 0, nullptr, 1, &to_dst);
        VkClearColorValue color{{0.1f, 0.3f + 0.3f * float(frames % 60) / 60.0f, 0.6f, 1.0f}};
        vkCmdClearColorImage(cmd, images[index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &color, 1,
                             &to_dst.subresourceRange);
        VkImageMemoryBarrier to_present = to_dst;
        to_present.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        to_present.dstAccessMask = 0;
        to_present.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        to_present.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &to_present);
        vkEndCommandBuffer(cmd);
        const VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.waitSemaphoreCount = 1;
        si.pWaitSemaphores = &acquired;
        si.pWaitDstStageMask = &wait_stage;
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cmd;
        si.signalSemaphoreCount = 1;
        si.pSignalSemaphores = &rendered;
        CHECK(vkQueueSubmit(queue, 1, &si, fence));
        VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        present.waitSemaphoreCount = 1;
        present.pWaitSemaphores = &rendered;
        present.swapchainCount = 1;
        present.pSwapchains = &swapchain;
        present.pImageIndices = &index;
        CHECK(vkQueuePresentKHR(queue, &present));
        const auto now = std::chrono::steady_clock::now();
        if (frames) worst_ms = std::max(worst_ms, std::chrono::duration<double, std::milli>(now - last).count());
        last = now;
        ++frames;
    }
    const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    std::printf("Presented %u frames in %.2f s (%.1f FPS, worst frame %.1f ms)\n", frames, elapsed,
                frames / elapsed, worst_ms);
    vkDeviceWaitIdle(device);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
