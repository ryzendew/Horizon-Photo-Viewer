#include "render/vulkan_context.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <vector>

#include <wayland-client.h>

#define VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME "VK_KHR_wayland_surface"

namespace hpv {

VulkanContext::~VulkanContext() {
    shutdown();
}

bool VulkanContext::init(wl_display* display) {
    display_ = display;
    if (!create_instance()) return false;
    if (!pick_physical_device()) return false;
    if (!create_device()) return false;
    return true;
}

bool VulkanContext::create_instance() {
    const char* inst_extensions[] = {
        VK_KHR_SURFACE_EXTENSION_NAME,
        VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME,
    };

    VkApplicationInfo app_info{};
    app_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app_info.pApplicationName = "Horizon Photo Viewer";
    app_info.applicationVersion = VK_MAKE_API_VERSION(0, 0, 1, 0);
    app_info.pEngineName = "Horizon";
    app_info.apiVersion = VK_API_VERSION_1_3;

    VkInstanceCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &app_info;
    ici.enabledExtensionCount = 2;
    ici.ppEnabledExtensionNames = inst_extensions;

    VkResult res = vkCreateInstance(&ici, nullptr, &instance_);
    if (res != VK_SUCCESS) {
        std::cerr << "vkCreateInstance failed: " << res << "\n";
        return false;
    }
    return true;
}

bool VulkanContext::pick_physical_device() {
    uint32_t count = 0;
    vkEnumeratePhysicalDevices(instance_, &count, nullptr);
    if (count == 0) {
        std::cerr << "No Vulkan physical devices found\n";
        return false;
    }

    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(instance_, &count, devices.data());

    // Prefer discrete GPU, fallback to any device with Wayland presentation support
    for (const auto& dev : devices) {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(dev, &props);

        uint32_t qcount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(dev, &qcount, nullptr);
        std::vector<VkQueueFamilyProperties> queues(qcount);
        vkGetPhysicalDeviceQueueFamilyProperties(dev, &qcount, queues.data());

        for (uint32_t qi = 0; qi < qcount; qi++) {
            if (queues[qi].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
                queue_family_ = qi;
                break;
            }
        }
        if (queue_family_ == UINT32_MAX) continue;

        if (phys_device_ == VK_NULL_HANDLE ||
            props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
            phys_device_ = dev;
            if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) break;
        }
    }

    if (phys_device_ == VK_NULL_HANDLE) {
        std::cerr << "No suitable Vulkan device found\n";
        return false;
    }

    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(phys_device_, &props);
    std::cout << "Using device: " << props.deviceName << "\n";
    return true;
}

bool VulkanContext::create_device() {
    const float queue_priority = 1.0f;
    VkDeviceQueueCreateInfo qci{};
    qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qci.queueFamilyIndex = queue_family_;
    qci.queueCount = 1;
    qci.pQueuePriorities = &queue_priority;

    const char* device_extensions[] = {
        VK_KHR_SWAPCHAIN_EXTENSION_NAME,
    };

    VkDeviceCreateInfo dci{};
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = 1;
    dci.ppEnabledExtensionNames = device_extensions;

    VkResult res = vkCreateDevice(phys_device_, &dci, nullptr, &device_);
    if (res != VK_SUCCESS) {
        std::cerr << "vkCreateDevice failed: " << res << "\n";
        return false;
    }

    vkGetDeviceQueue(device_, queue_family_, 0, &queue_);
    return true;
}

void VulkanContext::shutdown() {
    if (device_) {
        vkDeviceWaitIdle(device_);
        vkDestroyDevice(device_, nullptr);
        device_ = VK_NULL_HANDLE;
    }
    if (instance_) {
        vkDestroyInstance(instance_, nullptr);
        instance_ = VK_NULL_HANDLE;
    }
}

}
