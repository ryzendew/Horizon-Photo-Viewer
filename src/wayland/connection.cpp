#include "wayland/connection.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>

#include <wayland-client.h>

// Generated protocol headers
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

namespace hpv {

namespace {

void xdg_wm_base_ping_handler(void* /*data*/, xdg_wm_base* shell, uint32_t serial) {
    xdg_wm_base_pong(shell, serial);
}

constexpr xdg_wm_base_listener xdg_wm_base_listener_ = {
    .ping = xdg_wm_base_ping_handler,
};

}

void WaylandConnection::registry_global(void* data, wl_registry* registry, uint32_t name,
                                         const char* iface, uint32_t version) {
    auto& self = *static_cast<WaylandConnection*>(data);
    std::string_view ifc(iface ? iface : "");

    if (ifc == wl_compositor_interface.name) {
        self.compositor_ = static_cast<wl_compositor*>(
            wl_registry_bind(registry, name, &wl_compositor_interface,
                             std::min(version, 4u)));
    } else if (ifc == wl_shm_interface.name) {
        self.shm_ = static_cast<wl_shm*>(
            wl_registry_bind(registry, name, &wl_shm_interface, 1));
    } else if (ifc == wl_seat_interface.name) {
        if (!self.seat_) {
            self.seat_ = static_cast<wl_seat*>(
                wl_registry_bind(registry, name, &wl_seat_interface,
                                 std::min(version, 5u)));
        }
    } else if (ifc == xdg_wm_base_interface.name) {
        self.xdg_base_ = static_cast<xdg_wm_base*>(
            wl_registry_bind(registry, name, &xdg_wm_base_interface,
                             std::min(version, 5u)));
        xdg_wm_base_add_listener(self.xdg_base_, &xdg_wm_base_listener_, nullptr);
    } else if (ifc == wp_viewporter_interface.name) {
        self.viewporter_ = static_cast<wp_viewporter*>(
            wl_registry_bind(registry, name, &wp_viewporter_interface, 1));
    } else if (ifc == wp_fractional_scale_manager_v1_interface.name) {
        self.fractional_scale_mgr_ = static_cast<wp_fractional_scale_manager_v1*>(
            wl_registry_bind(registry, name,
                &wp_fractional_scale_manager_v1_interface, 1));
    } else if (ifc == wp_tearing_control_manager_v1_interface.name) {
        self.tearing_control_mgr_ = static_cast<wp_tearing_control_manager_v1*>(
            wl_registry_bind(registry, name,
                &wp_tearing_control_manager_v1_interface, 1));
    } else if (ifc == wp_color_manager_v1_interface.name) {
        self.color_mgr_ = static_cast<wp_color_manager_v1*>(
            wl_registry_bind(registry, name,
                &wp_color_manager_v1_interface,
                std::min(version, 2u)));
    } else if (ifc == zwp_idle_inhibit_manager_v1_interface.name) {
        self.idle_inhibit_mgr_ = static_cast<zwp_idle_inhibit_manager_v1*>(
            wl_registry_bind(registry, name,
                &zwp_idle_inhibit_manager_v1_interface, 1));
    } else if (ifc == zwp_keyboard_shortcuts_inhibit_manager_v1_interface.name) {
        self.keyboard_shortcuts_inhibit_mgr_ =
            static_cast<zwp_keyboard_shortcuts_inhibit_manager_v1*>(
                wl_registry_bind(registry, name,
                    &zwp_keyboard_shortcuts_inhibit_manager_v1_interface, 1));
    } else if (ifc == zwp_linux_dmabuf_v1_interface.name) {
        self.linux_dmabuf_ = static_cast<zwp_linux_dmabuf_v1*>(
            wl_registry_bind(registry, name,
                &zwp_linux_dmabuf_v1_interface,
                std::min(version, 4u)));
    } else if (ifc == wp_single_pixel_buffer_manager_v1_interface.name) {
        self.single_pixel_buffer_mgr_ =
            static_cast<wp_single_pixel_buffer_manager_v1*>(
                wl_registry_bind(registry, name,
                    &wp_single_pixel_buffer_manager_v1_interface, 1));
    } else if (ifc == zxdg_output_manager_v1_interface.name) {
        self.xdg_output_mgr_ = static_cast<zxdg_output_manager_v1*>(
            wl_registry_bind(registry, name,
                &zxdg_output_manager_v1_interface,
                std::min(version, 3u)));
    } else if (ifc == wl_data_device_manager_interface.name) {
        self.data_device_mgr_ = static_cast<wl_data_device_manager*>(
            wl_registry_bind(registry, name,
                &wl_data_device_manager_interface,
                std::min(version, 3u)));
    }
}

void WaylandConnection::registry_global_remove(void* /*data*/, wl_registry* /*registry*/,
                                                uint32_t /*name*/) {}

bool WaylandConnection::connect() {
    display_ = wl_display_connect(nullptr);
    if (!display_) {
        std::cerr << "wl_display_connect failed\n";
        return false;
    }

    registry_ = wl_display_get_registry(display_);
    if (!registry_) {
        std::cerr << "wl_display_get_registry failed\n";
        return false;
    }

    wl_registry_add_listener(registry_, &registry_listener_, this);
    wl_display_roundtrip(display_);
    wl_display_roundtrip(display_);

    if (!compositor_ || !xdg_base_) {
        std::cerr << "Missing required Wayland globals\n";
        return false;
    }

    return true;
}

void WaylandConnection::disconnect() {
    if (xdg_output_mgr_) zxdg_output_manager_v1_destroy(xdg_output_mgr_);
    if (single_pixel_buffer_mgr_) wp_single_pixel_buffer_manager_v1_destroy(single_pixel_buffer_mgr_);
    if (linux_dmabuf_) zwp_linux_dmabuf_v1_destroy(linux_dmabuf_);
    if (keyboard_shortcuts_inhibit_mgr_) zwp_keyboard_shortcuts_inhibit_manager_v1_destroy(keyboard_shortcuts_inhibit_mgr_);
    if (idle_inhibit_mgr_) zwp_idle_inhibit_manager_v1_destroy(idle_inhibit_mgr_);
    if (color_mgr_) wp_color_manager_v1_destroy(color_mgr_);
    if (tearing_control_mgr_) wp_tearing_control_manager_v1_destroy(tearing_control_mgr_);
    if (fractional_scale_mgr_) wp_fractional_scale_manager_v1_destroy(fractional_scale_mgr_);
    if (viewporter_) wp_viewporter_destroy(viewporter_);
    if (data_device_mgr_) wl_data_device_manager_destroy(data_device_mgr_);
    if (xdg_base_) xdg_wm_base_destroy(xdg_base_);
    if (seat_) wl_seat_destroy(seat_);
    if (shm_) wl_shm_destroy(shm_);
    if (compositor_) wl_compositor_destroy(compositor_);
    if (registry_) wl_registry_destroy(registry_);
    if (display_) {
        wl_display_disconnect(display_);
        display_ = nullptr;
    }
}

WaylandConnection::~WaylandConnection() {
    disconnect();
}

}
