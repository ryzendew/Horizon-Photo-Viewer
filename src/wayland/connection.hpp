#pragma once

#include <wayland-client.h>

// Generated protocol headers — these define the Wayland protocol types
#include "xdg-shell-client-protocol.h"
#include "viewporter-client-protocol.h"
#include "fractional-scale-v1-client-protocol.h"
#include "tearing-control-v1-client-protocol.h"
#include "color-management-v1-client-protocol.h"
#include "idle-inhibit-unstable-v1-client-protocol.h"
#include "keyboard-shortcuts-inhibit-unstable-v1-client-protocol.h"
#include "linux-dmabuf-v1-client-protocol.h"
#include "single-pixel-buffer-v1-client-protocol.h"
#include "xdg-output-unstable-v1-client-protocol.h"

#include <string>
#include <vector>
#include <memory>
struct wp_fractional_scale_manager_v1;
struct wp_tearing_control_manager_v1;
struct wp_color_manager_v1;
struct zwp_idle_inhibit_manager_v1;
struct zwp_keyboard_shortcuts_inhibit_manager_v1;
struct zwp_linux_dmabuf_v1;
struct wp_single_pixel_buffer_manager_v1;
struct zxdg_output_manager_v1;
struct wl_data_device_manager;

namespace hpv {

class WaylandConnection {
public:
    WaylandConnection() = default;
    ~WaylandConnection();
    WaylandConnection(const WaylandConnection&) = delete;
    WaylandConnection& operator=(const WaylandConnection&) = delete;

    bool connect();
    void disconnect();

    wl_display* display() const { return display_; }
    wl_compositor* compositor() const { return compositor_; }
    wl_shm* shm() const { return shm_; }
    wl_seat* seat() const { return seat_; }
    xdg_wm_base* xdg_base() const { return xdg_base_; }

    wp_viewporter* viewporter() const { return viewporter_; }
    wp_fractional_scale_manager_v1* fractional_scale_manager() const { return fractional_scale_mgr_; }
    wp_tearing_control_manager_v1* tearing_control_manager() const { return tearing_control_mgr_; }
    wp_color_manager_v1* color_manager() const { return color_mgr_; }
    zwp_idle_inhibit_manager_v1* idle_inhibit_manager() const { return idle_inhibit_mgr_; }
    zwp_keyboard_shortcuts_inhibit_manager_v1* keyboard_shortcuts_inhibit_manager() const { return keyboard_shortcuts_inhibit_mgr_; }
    zwp_linux_dmabuf_v1* linux_dmabuf() const { return linux_dmabuf_; }
    wp_single_pixel_buffer_manager_v1* single_pixel_buffer_manager() const { return single_pixel_buffer_mgr_; }
    zxdg_output_manager_v1* xdg_output_manager() const { return xdg_output_mgr_; }

    wl_data_device_manager* data_device_manager() const { return data_device_mgr_; }

private:
    static void registry_global(void* data, wl_registry* registry, uint32_t name,
                                const char* iface, uint32_t version);
    static void registry_global_remove(void* data, wl_registry* registry, uint32_t name);
    static constexpr wl_registry_listener registry_listener_ = {
        .global = registry_global,
        .global_remove = registry_global_remove,
    };

    wl_display* display_ = nullptr;
    wl_registry* registry_ = nullptr;

    wl_compositor* compositor_ = nullptr;
    wl_shm* shm_ = nullptr;
    wl_seat* seat_ = nullptr;

    xdg_wm_base* xdg_base_ = nullptr;

    wp_viewporter* viewporter_ = nullptr;
    wp_fractional_scale_manager_v1* fractional_scale_mgr_ = nullptr;
    wp_tearing_control_manager_v1* tearing_control_mgr_ = nullptr;
    wp_color_manager_v1* color_mgr_ = nullptr;
    zwp_idle_inhibit_manager_v1* idle_inhibit_mgr_ = nullptr;
    zwp_keyboard_shortcuts_inhibit_manager_v1* keyboard_shortcuts_inhibit_mgr_ = nullptr;
    zwp_linux_dmabuf_v1* linux_dmabuf_ = nullptr;
    wp_single_pixel_buffer_manager_v1* single_pixel_buffer_mgr_ = nullptr;
    zxdg_output_manager_v1* xdg_output_mgr_ = nullptr;
    wl_data_device_manager* data_device_mgr_ = nullptr;
};

}
