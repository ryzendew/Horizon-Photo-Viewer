#pragma once

#include "render/vulkan_context.hpp"

#include <vulkan/vulkan.h>

#include <cstdint>
#include <memory>
#include <vector>

struct wl_surface;
struct wl_buffer;

namespace hpv {

struct VulkanSurfaceFrame {
    VkFence fence = VK_NULL_HANDLE;
    VkSemaphore sem_acquire = VK_NULL_HANDLE;
    VkSemaphore sem_present = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkBuffer staging = VK_NULL_HANDLE;
    VkDeviceMemory staging_mem = VK_NULL_HANDLE;
    void* staging_map = nullptr;
    size_t staging_size = 0;
};

struct VulkanSurfaceSwapchain {
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    std::vector<VkImage> images;
    std::vector<VkImageView> views;
    VkFormat format = VK_FORMAT_B8G8R8A8_SRGB;
    VkExtent2D extent{};
};

class VulkanSurface {
public:
    VulkanSurface() = default;
    ~VulkanSurface();
    VulkanSurface(const VulkanSurface&) = delete;
    VulkanSurface& operator=(const VulkanSurface&) = delete;

    bool init(VulkanContext& ctx, wl_surface* wl_surface);
    void destroy();
    bool valid() const { return swapchain_.swapchain != VK_NULL_HANDLE; }

    void present(const uint8_t* rgba_data, int img_w, int img_h,
                 int out_w, int out_h);
    void resize(int width, int height);

    wl_buffer* buffer() const { return nullptr; } // We use Vulkan swapchain directly

private:
    void create_swapchain(VulkanContext& ctx, int width, int height);
    void destroy_swapchain(VulkanContext& ctx);
    bool ensure_staging(VulkanContext& ctx, size_t size, int idx);
    void record_command(VulkanContext& ctx, int frame_idx, uint32_t image_idx);
    void transition_image(VulkanContext& ctx, VkCommandBuffer cmd,
                           VkImage image, VkImageLayout old, VkImageLayout new_layout);

    VulkanContext* ctx_ = nullptr;
    wl_surface* wl_surface_ = nullptr;
    VkSurfaceKHR vk_surface_ = VK_NULL_HANDLE;
    VulkanSurfaceSwapchain swapchain_;
    std::vector<VulkanSurfaceFrame> frames_;
    int current_frame_ = 0;
    int window_width_ = 1280;
    int window_height_ = 720;
    bool initialized_ = false;
};

}
