#include "render/vulkan_surface.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <vector>

#include <wayland-client.h>

namespace hpv {

namespace {

constexpr int kMaxFramesInFlight = 2;

VkSurfaceFormatKHR pick_format(const std::vector<VkSurfaceFormatKHR>& formats) {
    for (const auto& f : formats) {
        if (f.format == VK_FORMAT_B8G8R8A8_SRGB &&
            f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            return f;
        }
    }
    for (const auto& f : formats) {
        if (f.format == VK_FORMAT_B8G8R8A8_UNORM &&
            f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            return f;
        }
    }
    return formats[0];
}

VkPresentModeKHR pick_present_mode(const std::vector<VkPresentModeKHR>& modes,
                                    bool allow_tearing) {
    if (allow_tearing) {
        for (auto m : modes) {
            if (m == VK_PRESENT_MODE_IMMEDIATE_KHR) return m;
        }
    }
    for (auto m : modes) {
        if (m == VK_PRESENT_MODE_MAILBOX_KHR) return m;
    }
    return VK_PRESENT_MODE_FIFO_KHR;
}

}

VulkanSurface::~VulkanSurface() {
    destroy();
}

bool VulkanSurface::init(VulkanContext& ctx, wl_surface* wl_surface) {
    ctx_ = &ctx;
    wl_surface_ = wl_surface;

    // Create Vulkan surface from wl_surface
    VkWaylandSurfaceCreateInfoKHR wsci{};
    wsci.sType = VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR;
    wsci.display = ctx.display();
    wsci.surface = wl_surface;

    VkResult res = vkCreateWaylandSurfaceKHR(ctx.instance(), &wsci, nullptr, &vk_surface_);
    if (res != VK_SUCCESS) {
        std::cerr << "vkCreateWaylandSurfaceKHR failed: " << res << "\n";
        return false;
    }

    // Create swapchain
    create_swapchain(ctx, window_width_, window_height_);

    // Create per-frame state
    frames_.resize(kMaxFramesInFlight);
    VkCommandPoolCreateInfo cpci{};
    cpci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    cpci.queueFamilyIndex = ctx.queue_family();
    cpci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;

    for (int i = 0; i < kMaxFramesInFlight; i++) {
        auto& fr = frames_[i];

        VkCommandPool pool;
        res = vkCreateCommandPool(ctx.device(), &cpci, nullptr, &pool);
        if (res != VK_SUCCESS) {
            std::cerr << "Failed to create command pool: " << res << "\n";
            return false;
        }

        VkCommandBufferAllocateInfo cbai{};
        cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cbai.commandPool = pool;
        cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cbai.commandBufferCount = 1;
        vkAllocateCommandBuffers(ctx.device(), &cbai, &fr.cmd);

        // Don't need the pool stored separately, cmd already allocated from it
        // We'll use the cmd and destroy the pool in cleanup

        VkFenceCreateInfo fci{};
        fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        vkCreateFence(ctx.device(), &fci, nullptr, &fr.fence);

        VkSemaphoreCreateInfo sci{};
        sci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        vkCreateSemaphore(ctx.device(), &sci, nullptr, &fr.sem_acquire);
        vkCreateSemaphore(ctx.device(), &sci, nullptr, &fr.sem_present);
    }

    initialized_ = true;
    return true;
}

void VulkanSurface::create_swapchain(VulkanContext& ctx, int width, int height) {
    VkSurfaceCapabilitiesKHR caps;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(ctx.phys_device(), vk_surface_, &caps);

    uint32_t nf = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(ctx.phys_device(), vk_surface_, &nf, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(nf);
    vkGetPhysicalDeviceSurfaceFormatsKHR(ctx.phys_device(), vk_surface_, &nf, formats.data());

    uint32_t nm = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(ctx.phys_device(), vk_surface_, &nm, nullptr);
    std::vector<VkPresentModeKHR> modes(nm);
    vkGetPhysicalDeviceSurfacePresentModesKHR(ctx.phys_device(), vk_surface_, &nm, modes.data());

    auto format = pick_format(formats);
    auto present_mode = pick_present_mode(modes, false);

    VkExtent2D extent = caps.currentExtent;
    if (extent.width == UINT32_MAX) {
        extent.width = std::clamp((uint32_t)width, caps.minImageExtent.width, caps.maxImageExtent.width);
        extent.height = std::clamp((uint32_t)height, caps.minImageExtent.height, caps.maxImageExtent.height);
    }

    uint32_t image_count = std::clamp(caps.minImageCount + 1,
                                       1u, caps.maxImageCount);

    VkSwapchainCreateInfoKHR sci{};
    sci.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    sci.surface = vk_surface_;
    sci.minImageCount = image_count;
    sci.imageFormat = format.format;
    sci.imageColorSpace = format.colorSpace;
    sci.imageExtent = extent;
    sci.imageArrayLayers = 1;
    sci.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    sci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    sci.preTransform = caps.currentTransform;
    sci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    sci.presentMode = present_mode;
    sci.clipped = VK_TRUE;
    sci.oldSwapchain = swapchain_.swapchain;

    VkSwapchainKHR old = swapchain_.swapchain;
    VkResult res = vkCreateSwapchainKHR(ctx.device(), &sci, nullptr, &swapchain_.swapchain);
    if (res != VK_SUCCESS) {
        std::cerr << "vkCreateSwapchainKHR failed: " << res << "\n";
        return;
    }

    // Destroy old swapchain
    if (old != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(ctx.device(), old, nullptr);
    }

    // Get images
    uint32_t ni = 0;
    vkGetSwapchainImagesKHR(ctx.device(), swapchain_.swapchain, &ni, nullptr);
    swapchain_.images.resize(ni);
    vkGetSwapchainImagesKHR(ctx.device(), swapchain_.swapchain, &ni, swapchain_.images.data());

    // Create image views
    swapchain_.views.clear();
    swapchain_.views.reserve(ni);
    for (auto img : swapchain_.images) {
        VkImageViewCreateInfo ivci{};
        ivci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        ivci.image = img;
        ivci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        ivci.format = format.format;
        ivci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        ivci.subresourceRange.levelCount = 1;
        ivci.subresourceRange.layerCount = 1;

        VkImageView view;
        vkCreateImageView(ctx.device(), &ivci, nullptr, &view);
        swapchain_.views.push_back(view);
    }

    swapchain_.format = format.format;
    swapchain_.extent = extent;
}

void VulkanSurface::destroy_swapchain(VulkanContext& ctx) {
    for (auto view : swapchain_.views) {
        vkDestroyImageView(ctx.device(), view, nullptr);
    }
    swapchain_.views.clear();
    swapchain_.images.clear();

    if (swapchain_.swapchain != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(ctx.device(), swapchain_.swapchain, nullptr);
        swapchain_.swapchain = VK_NULL_HANDLE;
    }
}

bool VulkanSurface::ensure_staging(VulkanContext& ctx, size_t size, int idx) {
    auto& fr = frames_[idx];
    if (fr.staging_size >= size && fr.staging_map) return true;

    if (fr.staging) {
        vkDestroyBuffer(ctx.device(), fr.staging, nullptr);
        fr.staging = VK_NULL_HANDLE;
    }
    if (fr.staging_mem) {
        vkFreeMemory(ctx.device(), fr.staging_mem, nullptr);
        fr.staging_mem = VK_NULL_HANDLE;
    }

    VkBufferCreateInfo bci{};
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = size;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VkResult res = vkCreateBuffer(ctx.device(), &bci, nullptr, &fr.staging);
    if (res != VK_SUCCESS) return false;

    VkMemoryRequirements mem_req;
    vkGetBufferMemoryRequirements(ctx.device(), fr.staging, &mem_req);

    VkPhysicalDeviceMemoryProperties mem_props;
    vkGetPhysicalDeviceMemoryProperties(ctx.phys_device(), &mem_props);

    uint32_t mem_type = UINT32_MAX;
    for (uint32_t i = 0; i < mem_props.memoryTypeCount; i++) {
        if ((mem_req.memoryTypeBits & (1 << i)) &&
            (mem_props.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) {
            mem_type = i;
            break;
        }
    }
    if (mem_type == UINT32_MAX) return false;

    VkMemoryAllocateInfo mai{};
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize = mem_req.size;
    mai.memoryTypeIndex = mem_type;

    res = vkAllocateMemory(ctx.device(), &mai, nullptr, &fr.staging_mem);
    if (res != VK_SUCCESS) return false;

    vkBindBufferMemory(ctx.device(), fr.staging, fr.staging_mem, 0);
    vkMapMemory(ctx.device(), fr.staging_mem, 0, size, 0, &fr.staging_map);

    fr.staging_size = size;
    return true;
}

void VulkanSurface::transition_image(VulkanContext&, VkCommandBuffer cmd,
                                      VkImage image, VkImageLayout old_layout,
                                      VkImageLayout new_layout) {
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = old_layout;
    barrier.newLayout = new_layout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.layerCount = 1;

    VkPipelineStageFlags src_stage, dst_stage;

    if (old_layout == VK_IMAGE_LAYOUT_UNDEFINED &&
        new_layout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
        barrier.srcAccessMask = 0;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        src_stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        dst_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    } else if (old_layout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL &&
               new_layout == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR) {
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = 0;
        src_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        dst_stage = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
    } else {
        return;
    }

    vkCmdPipelineBarrier(cmd, src_stage, dst_stage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
}

void VulkanSurface::record_command(VulkanContext& ctx, int frame_idx, uint32_t image_idx) {
    auto& fr = frames_[frame_idx];
    VkCommandBuffer cmd = fr.cmd;

    VkCommandBufferBeginInfo cbbi{};
    cbbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    vkBeginCommandBuffer(cmd, &cbbi);

    // Transition image to transfer dst
    transition_image(ctx, cmd, swapchain_.images[image_idx],
                     VK_IMAGE_LAYOUT_UNDEFINED,
                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

    // Copy staging buffer to image
    VkBufferImageCopy region{};
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageExtent.width = swapchain_.extent.width;
    region.imageExtent.height = swapchain_.extent.height;
    region.imageExtent.depth = 1;

    vkCmdCopyBufferToImage(cmd, fr.staging, swapchain_.images[image_idx],
                            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    // Transition image to present src
    transition_image(ctx, cmd, swapchain_.images[image_idx],
                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);

    vkEndCommandBuffer(cmd);
}

void VulkanSurface::present(const uint8_t* rgba_data, int img_w, int img_h,
                             int out_w, int out_h) {
    if (!ctx_ || !valid()) return;

    // Check if swapchain needs recreation
    if (out_w != (int)swapchain_.extent.width || out_h != (int)swapchain_.extent.height) {
        resize(out_w, out_h);
    }

    auto& ctx = *ctx_;
    auto& fr = frames_[current_frame_];

    // Wait for fence
    vkWaitForFences(ctx.device(), 1, &fr.fence, VK_TRUE, UINT64_MAX);
    vkResetFences(ctx.device(), 1, &fr.fence);

    // Acquire next image
    uint32_t image_idx;
    VkResult res = vkAcquireNextImageKHR(ctx.device(), swapchain_.swapchain,
                                          UINT64_MAX, fr.sem_acquire,
                                          VK_NULL_HANDLE, &image_idx);
    if (res == VK_ERROR_OUT_OF_DATE_KHR || res == VK_SUBOPTIMAL_KHR) {
        resize(window_width_, window_height_);
        return;
    }

    // Upload image to staging - center the image in the buffer
    size_t staging_size = (size_t)swapchain_.extent.width * swapchain_.extent.height * 4;
    if (!ensure_staging(ctx, staging_size, current_frame_)) return;

    // Simple nearest-neighbor scale + center
    auto* dst = static_cast<uint8_t*>(fr.staging_map);
    // Clear to black
    std::memset(dst, 0, staging_size);

    // Scale and center the image
    float scale = std::min((float)out_w / (float)img_w, (float)out_h / (float)img_h);
    int scaled_w = (int)(img_w * scale);
    int scaled_h = (int)(img_h * scale);
    int offset_x = (out_w - scaled_w) / 2;
    int offset_y = (out_h - scaled_h) / 2;

    for (int y = 0; y < scaled_h; y++) {
        int src_y = (int)((float)y / scale);
        if (src_y < 0) src_y = 0;
        if (src_y >= img_h) src_y = img_h - 1;

        for (int x = 0; x < scaled_w; x++) {
            int src_x = (int)((float)x / scale);
            if (src_x < 0) src_x = 0;
            if (src_x >= img_w) src_x = img_w - 1;

            int dst_x = offset_x + x;
            int dst_y = offset_y + y;
            if (dst_x < 0 || dst_x >= out_w || dst_y < 0 || dst_y >= out_h) continue;

            const uint8_t* src_px = rgba_data + (src_y * img_w + src_x) * 4;
            uint8_t* dst_px = dst + (dst_y * out_w + dst_x) * 4;
            dst_px[0] = src_px[2]; // R <- B (BGRA output)
            dst_px[1] = src_px[1]; // G
            dst_px[2] = src_px[0]; // B <- R
            dst_px[3] = src_px[3]; // A
        }
    }

    // Record and submit command buffer
    record_command(ctx, current_frame_, image_idx);

    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &fr.cmd;

    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    si.waitSemaphoreCount = 1;
    si.pWaitSemaphores = &fr.sem_acquire;
    si.pWaitDstStageMask = &wait_stage;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &fr.sem_present;

    vkQueueSubmit(ctx.queue(), 1, &si, fr.fence);

    // Present
    VkPresentInfoKHR pi{};
    pi.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &fr.sem_present;
    pi.swapchainCount = 1;
    pi.pSwapchains = &swapchain_.swapchain;
    pi.pImageIndices = &image_idx;

    res = vkQueuePresentKHR(ctx.queue(), &pi);
    if (res == VK_ERROR_OUT_OF_DATE_KHR || res == VK_SUBOPTIMAL_KHR) {
        resize(window_width_, window_height_);
    }

    current_frame_ = (current_frame_ + 1) % kMaxFramesInFlight;
}

void VulkanSurface::resize(int width, int height) {
    if (!ctx_) return;

    window_width_ = width;
    window_height_ = height;

    vkDeviceWaitIdle(ctx_->device());
    destroy_swapchain(*ctx_);
    create_swapchain(*ctx_, width, height);
}

void VulkanSurface::destroy() {
    if (!ctx_) return;
    if (initialized_) {
        vkDeviceWaitIdle(ctx_->device());

        for (auto& fr : frames_) {
            if (fr.fence) vkDestroyFence(ctx_->device(), fr.fence, nullptr);
            if (fr.sem_acquire) vkDestroySemaphore(ctx_->device(), fr.sem_acquire, nullptr);
            if (fr.sem_present) vkDestroySemaphore(ctx_->device(), fr.sem_present, nullptr);
            if (fr.staging_map) vkUnmapMemory(ctx_->device(), fr.staging_mem);
            if (fr.staging_mem) vkFreeMemory(ctx_->device(), fr.staging_mem, nullptr);
            if (fr.staging) vkDestroyBuffer(ctx_->device(), fr.staging, nullptr);
            // Command buffers freed with pool destruction
        }
        frames_.clear();

        destroy_swapchain(*ctx_);
        initialized_ = false;
    }

    if (vk_surface_ != VK_NULL_HANDLE) {
        vkDestroySurfaceKHR(ctx_->instance(), vk_surface_, nullptr);
        vk_surface_ = VK_NULL_HANDLE;
    }
}

}
