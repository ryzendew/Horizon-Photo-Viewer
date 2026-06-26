#pragma once

#define VK_USE_PLATFORM_WAYLAND_KHR
#include <vulkan/vulkan.h>

#include <memory>
#include <vector>

struct wl_display;

namespace hpv {

class VulkanContext {
public:
    VulkanContext() = default;
    ~VulkanContext();
    VulkanContext(const VulkanContext&) = delete;
    VulkanContext& operator=(const VulkanContext&) = delete;

    bool init(wl_display* display);
    void shutdown();

    bool valid() const { return device_ != VK_NULL_HANDLE; }

    VkInstance instance() const { return instance_; }
    VkPhysicalDevice phys_device() const { return phys_device_; }
    VkDevice device() const { return device_; }
    VkQueue queue() const { return queue_; }
    uint32_t queue_family() const { return queue_family_; }
    wl_display* display() const { return display_; }

private:
    bool create_instance();
    bool pick_physical_device();
    bool create_device();

    wl_display* display_ = nullptr;
    VkInstance instance_ = VK_NULL_HANDLE;
    VkPhysicalDevice phys_device_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    uint32_t queue_family_ = UINT32_MAX;
};

}
