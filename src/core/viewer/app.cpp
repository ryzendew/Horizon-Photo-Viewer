#include "core/viewer/app.hpp"
#include "common/math/math.hpp"
#include "decode/common/svg_doc.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string_view>

#include <unistd.h>
#include <sys/stat.h>
#include <time.h>
#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>

#include <cairo.h>

#include "psimpl/psimpl.h"
#include "tinyspline/tinysplinecxx.h"
#include <nanosvg/nanosvg.h>
#include <nanosvg/nanosvgrast.h>

using namespace hpv::math;
namespace fs = std::filesystem;

// Listener trampolines
namespace {

const xdg_surface_listener kXdgSurfaceListener = {
    .configure = [](void*, xdg_surface* surf, uint32_t serial) {
        xdg_surface_ack_configure(surf, serial);
    },
};

const xdg_toplevel_listener kXdgToplevelListener = {
    .configure = [](void* data, xdg_toplevel*, int32_t w, int32_t h, wl_array*) {
        if (w > 0 && h > 0) {
            static_cast<hpv::App*>(data)->set_window_size(w, h);
        }
    },
    .close = [](void* data, xdg_toplevel*) {
        static_cast<hpv::App*>(data)->on_close();
    },
    .configure_bounds = [](void*, xdg_toplevel*, int32_t, int32_t) {},
    .wm_capabilities = [](void*, xdg_toplevel*, wl_array*) {},
};

}

namespace hpv {

App::App(int argc, char** argv) {
    config_ = Config::load();
    bg_alpha_ = config_.bg_alpha;
    if (argc > 1) {
        current_path_ = argv[1];
    }
}

App::~App() {
    if (data_device_) wl_data_device_release(data_device_);
    if (xdg_toplevel_) xdg_toplevel_destroy(xdg_toplevel_);
    if (xdg_surface_) xdg_surface_destroy(xdg_surface_);
    if (surface_) wl_surface_destroy(surface_);
    destroy_cached_strip();
    if (svg_vector_cache_) { cairo_surface_destroy(svg_vector_cache_); svg_vector_cache_ = nullptr; }
}

bool App::init() {
    if (!init_wayland()) return false;
    if (!create_window()) return false;
    init_font();

    // Load Material Symbols icons for the toolbar
    {
        const char* icon_paths[] = {
            "assets/fonts/MF/MaterialSymbolsRounded.ttf",
            "../assets/fonts/MF/MaterialSymbolsRounded.ttf",
#ifdef ICON_FONT_PATH_SYSTEM
            ICON_FONT_PATH_SYSTEM,
#endif
            nullptr,
        };
        bool icons_loaded = false;
        for (const char** ip = icon_paths; *ip; ip++) {
#ifdef CROP_SVG_PATH_SYSTEM
            if (overlay_.init_icons(*ip, CROP_SVG_PATH_SYSTEM,
                                   FLIP_SVG_PATH_SYSTEM)
                && overlay_.crop_icon_loaded()) {
                icons_loaded = true;
                break;
            }
#endif
            const char* base_paths[] = { "assets/", "../assets/", nullptr };
            for (const char** bp = base_paths; *bp; bp++) {
                std::string base = *bp;
                std::string crop_path = base + "crop.svg";
                std::string flip_path = base + "flip.svg";
                if (overlay_.init_icons(*ip, crop_path.c_str(), flip_path.c_str())
                    && overlay_.crop_icon_loaded()) {
                    icons_loaded = true;
                    break;
                }
            }
            if (icons_loaded) break;
        }
        if (!icons_loaded)
            std::cerr << "overlay: icons (font + svg) not found\n";
    }

    // Register decoders up front so that if set_window_size triggers a
    // render during the roundtrips below, load_image can actually decode.
    decoders_.register_decoder(std::make_unique<WuffsDecoder>());
#ifdef HAVE_LIBJPEG
    decoders_.register_decoder(std::make_unique<JpegDecoder>());
#endif
#ifdef HAVE_LIBWEBP
    decoders_.register_decoder(std::make_unique<WebPDecoder>());
#endif
#ifdef HAVE_LIBJXL
    decoders_.register_decoder(std::make_unique<JxlDecoder>());
#endif
    decoders_.register_decoder(std::make_unique<HeifDecoder>());
    decoders_.register_decoder(std::make_unique<AvifDecoder>());
    decoders_.register_decoder(std::make_unique<RawDecoder>());
    decoders_.register_decoder(std::make_unique<SvgDecoder>());
    decoders_.register_decoder(std::make_unique<StbDecoder>());

    // Init portal file dialog
    portal_dialog_.init();

    // First commit tells the compositor to configure the surface.
    // Without this, the roundtrips below get no configure events and
    // window_width_/height_ stay at the default (1280x720) — wrong.
    wl_surface_commit(surface_);

    // Roundtrips to process initial configure events.
    // xdg_surface_configure gives the serial (acked in the listener).
    // xdg_toplevel_configure gives the actual window size.
    wl_display_roundtrip(conn_.display());
    wl_display_roundtrip(conn_.display());

    // Create both buffers at the configured size
    for (auto& buf : bufs_) {
        buf.init(conn_.shm(), window_width_, window_height_);
        buf.set_release_callback([this]() { on_shm_release(); });
    }

    // Open initial file if provided
    if (!current_path_.empty()) {
        open_file(current_path_);
    }

    // Render the first frame and commit so the compositor maps the surface
    render();

    return true;
}

bool App::init_wayland() {
    if (!conn_.connect()) {
        std::cerr << "Failed to connect to Wayland display\n";
        return false;
    }

    if (!conn_.compositor()) {
        std::cerr << "Compositor not available\n";
        return false;
    }

    // Init seat for keyboard + pointer input
    if (conn_.seat()) {
        seat_.init(conn_.seat(), conn_.display());
        seat_.set_key_callback([this](const KeyEvent& ev) {
            on_key(ev);
        });
        seat_.set_pointer_callback([this](const PointerEvent& ev) {
            on_pointer(ev);
        });
        seat_.set_scroll_callback([this](const ScrollEvent& ev) {
            on_scroll(ev);
        });
        seat_.set_motion_callback([this](int x, int y) {
            on_motion(x, y);
        });
    }

    // Init data device for drag-and-drop
    if (conn_.data_device_manager() && conn_.seat()) {
        data_device_ = wl_data_device_manager_get_data_device(
            conn_.data_device_manager(), conn_.seat());
        static constexpr wl_data_device_listener listener = {
            .data_offer = handle_data_offer,
            .enter = handle_data_enter,
            .leave = handle_data_leave,
            .motion = handle_data_motion,
            .drop = handle_data_drop,
            .selection = handle_data_selection,
        };
        wl_data_device_add_listener(data_device_, &listener, this);
    }

    return true;
}

bool App::create_window() {
    surface_ = wl_compositor_create_surface(conn_.compositor());
    if (!surface_) {
        std::cerr << "Failed to create wl_surface\n";
        return false;
    }

    xdg_surface_ = xdg_wm_base_get_xdg_surface(conn_.xdg_base(), surface_);
    if (!xdg_surface_) {
        std::cerr << "Failed to create xdg_surface\n";
        return false;
    }
    xdg_surface_add_listener(xdg_surface_, &kXdgSurfaceListener, this);

    xdg_toplevel_ = xdg_surface_get_toplevel(xdg_surface_);
    if (!xdg_toplevel_) {
        std::cerr << "Failed to create xdg_toplevel\n";
        return false;
    }
    xdg_toplevel_add_listener(xdg_toplevel_, &kXdgToplevelListener, this);

    xdg_toplevel_set_title(xdg_toplevel_, "Horizon Photo Viewer");
    xdg_toplevel_set_app_id(xdg_toplevel_, "com.horizon.photo-viewer");
    xdg_toplevel_set_min_size(xdg_toplevel_, 320, 200);

    // Init surface extensions (viewporter, fractional-scale, tearing)
    surface_extensions_.init(conn_, surface_);

    return true;
}

void App::init_font() {
    const char* font_paths[] = {
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/dejavu/DejaVuSans.ttf",
        nullptr,
    };
    for (const char** fp = font_paths; *fp; fp++) {
        if (text_renderer_.init(*fp, 13.0f)) return;
    }
    std::cerr << "text: no font found\n";
}

void App::render() {
    if (loading_) return;
    if (decoded_image_.rgba.empty() && !current_path_.empty()) {
        load_image(current_path_);
        return;
    }
    present();
}

void App::present() {
    pending_redraw_ = false;

    int bi = paint_buf_;
    int win_w = std::max(1, window_width_);
    int win_h = std::max(1, window_height_);

    // If the current buffer is busy (compositor still reading it),
    // try the other buffer instead. If both are busy, process any
    // already-queued release events. If still both busy, defer redraw
    // rather than drawing into a buffer the compositor is still using,
    // which causes visible flickering.
    if (bufs_[bi].busy()) {
        int other = 1 - bi;
        if (!bufs_[other].busy()) {
            bi = other;
        } else {
            wl_display_dispatch_pending(conn_.display());
            if (!bufs_[bi].busy()) {
                // freed by pending release event
            } else if (!bufs_[other].busy()) {
                bi = other;
            } else {
                // Both still busy — defer redraw until a buffer is released
                pending_redraw_ = true;
                return;
            }
        }
    }

    // Ensure buffer at correct size (only if buffer already exists)
    if (bufs_[bi].buffer() && (win_w != bufs_[bi].width() || win_h != bufs_[bi].height())) {
        bufs_[bi].init(conn_.shm(), win_w, win_h);
    }

    if (!bufs_[bi].buffer()) return;

    // Wrap the SHM buffer in a Cairo surface
    cairo_surface_t* cs = cairo_image_surface_create_for_data(
        bufs_[bi].data(),
        CAIRO_FORMAT_ARGB32,
        win_w, win_h,
        bufs_[bi].stride()
    );
    cairo_t* cr = cairo_create(cs);

    // --- Apply theme before drawing chrome ---
    hpv::m3::apply_theme(config_.theme == "light");

    // --- Background (use SOURCE to overwrite stale buffer content) ---
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgba(cr, hpv::m3::surface_r, hpv::m3::surface_g,
                          hpv::m3::surface_b, bg_alpha_);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

    // --- Content area (shrunk when sidebar or thumbnail strip is visible) ---
    int content_w = win_w;
    int sidebar_w = 0;
    if (show_sidebar_) {
        sidebar_w = 320;
        content_w = win_w - sidebar_w;
    }
    bool hide_strip = fullscreen_ || slideshow_;
    bool strip_visible = show_thumbnails_ && (!hide_strip || show_thumbnails_hover_);
    int strip_h = (strip_visible && decoded_image_.width > 0 && dir_images_.size() > 1)
                      ? ThumbnailStrip::kHeight : 0;

    // --- Sidebar area background (painted before the image) ---
    if (show_sidebar_) {
        cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
        cairo_set_source_rgba(cr, hpv::m3::surface_container_r, hpv::m3::surface_container_g,
                              hpv::m3::surface_container_b, bg_alpha_);
        cairo_rectangle(cr, content_w, 0, sidebar_w, win_h);
        cairo_fill(cr);
        cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
    }

    // --- Opaque region: empty when bg_alpha < 1 so compositor respects alpha ---
    {
        wl_region* reg = wl_compositor_create_region(conn_.compositor());
        if (bg_alpha_ >= 1.0f) {
            wl_region_add(reg, 0, 0, win_w, win_h);
        }
        wl_surface_set_opaque_region(surface_, reg);
        wl_region_destroy(reg);
    }

    // --- Decoded image (constrained to content area) ---
    if (!decoded_image_.rgba.empty()) {
        float img_w = orig_img_w_ > 0 ? orig_img_w_ : (float)decoded_image_.width;
        float img_h = orig_img_h_ > 0 ? orig_img_h_ : (float)decoded_image_.height;

        // Swap dimensions for 90/270 rotation so fit calculation uses the correct aspect
        float layout_w = (rotation_ == 90 || rotation_ == 270) ? img_h : img_w;
        float layout_h = (rotation_ == 90 || rotation_ == 270) ? img_w : img_h;

        int avail_h = win_h - (show_toolbar_ ? Overlay::kToolbarHeight : 0) - strip_h;
        float fit_scale = std::min((float)content_w / layout_w, (float)avail_h / layout_h) * zoom_;
        int draw_w = std::max(1, (int)(layout_w * fit_scale));
        int draw_h = std::max(1, (int)(layout_h * fit_scale));

        int offset_x = (content_w - draw_w) / 2 + (int)pan_x_;
        int offset_y = (show_toolbar_ ? Overlay::kToolbarHeight : 0) + (avail_h - draw_h) / 2 + (int)pan_y_;

        cairo_save(cr);
        cairo_translate(cr, offset_x, offset_y);

        // Rotation / flip around center of draw area
        cairo_translate(cr, draw_w / 2.0, draw_h / 2.0);
        cairo_scale(cr, flip_h_ ? -1.0 : 1.0, flip_v_ ? -1.0 : 1.0);
        cairo_rotate(cr, rotation_ * M_PI / 180.0);
        cairo_translate(cr, -draw_w / 2.0, -draw_h / 2.0);

        std::cerr << "[present] svg_doc=" << (void*)svg_doc_
                  << " cache=" << (void*)svg_vector_cache_
                  << " cache_dims=" << svg_vector_w_ << "x" << svg_vector_h_
                  << " draw=" << draw_w << "x" << draw_h
                  << " native=" << orig_img_w_ << "x" << orig_img_h_
                  << " bgra=" << bgra_cache_.size()
                  << "\n";
        if (svg_doc_) {
            // --- SVG rendering: cache at max(draw, native), only rebuild when cache too small ---
            int nw = std::max(draw_w, (int)orig_img_w_);
            int nh = std::max(draw_h, (int)orig_img_h_);
            if (!svg_vector_cache_ || nw > svg_vector_w_ || nh > svg_vector_h_) {
                if (svg_vector_cache_) cairo_surface_destroy(svg_vector_cache_);
                nw = std::max(1, nw);
                nh = std::max(1, nh);
                svg_vector_cache_ = cairo_image_surface_create(
                    CAIRO_FORMAT_ARGB32, nw, nh);
                svg_vector_w_ = nw;
                svg_vector_h_ = nh;
                cairo_t* vcr = cairo_create(svg_vector_cache_);
                {
                    struct timespec tv0, tv1;
                    clock_gettime(CLOCK_MONOTONIC, &tv0);
                    svg_doc_render(vcr, svg_doc_, (float)nw, (float)nh);
                    clock_gettime(CLOCK_MONOTONIC, &tv1);
                    int64_t v_us = (tv1.tv_sec - tv0.tv_sec) * 1000000 +
                                   (tv1.tv_nsec - tv0.tv_nsec) / 1000;
                    std::cerr << "[svg] build: " << v_us << " us ("
                              << nw << "x" << nh << ")\n";
                }
                cairo_destroy(vcr);

                // Persist render to disk cache for instant full-quality recall
                if (nw >= 64 && nh >= 64 &&
                    ThumbCache::save_svg_surface(current_path_, svg_vector_cache_)) {
                    std::cerr << "[svg] disk cache saved: "
                              << nw << "x" << nh << "\n";
                }
            }

            // Display the cache scaled to fit the display rect (downscales when zoomed out)
            cairo_save(cr);
            cairo_scale(cr, (double)draw_w / svg_vector_w_,
                            (double)draw_h / svg_vector_h_);
            cairo_set_source_surface(cr, svg_vector_cache_, 0, 0);
            cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
            cairo_paint(cr);
            cairo_restore(cr);

            // Markup elements in image coords
            cairo_save(cr);
            cairo_scale(cr, (double)draw_w / orig_img_w_,
                            (double)draw_h / orig_img_h_);
            draw_markup_elements(cr);
            cairo_restore(cr);
        } else {
            cairo_scale(cr, (double)draw_w / decoded_image_.width,
                            (double)draw_h / decoded_image_.height);
            cairo_surface_t* img_surf = cairo_image_surface_create_for_data(
                bgra_cache_.data(),
                CAIRO_FORMAT_ARGB32,
                decoded_image_.width, decoded_image_.height,
                decoded_image_.width * 4
            );
            cairo_set_source_surface(cr, img_surf, 0, 0);
            cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_NEAREST);
            cairo_paint(cr);
            cairo_surface_destroy(img_surf);

            // Draw markup elements (in original image coords, within the same transform)
            draw_markup_elements(cr);
        }

        cairo_restore(cr);
    }

    // --- Overlay (toolbar only — info/placeholder drawn after thumbnail strip) ---
    OverlayState ov_state;
    ov_state.show_info = show_overlay_;
    ov_state.toolbar_visible = show_toolbar_;
    ov_state.fullscreen = fullscreen_;
    ov_state.slideshow = slideshow_;
    ov_state.show_settings = show_settings_;
    ov_state.show_sidebar = show_sidebar_;
    ov_state.crop_active = crop_active_;
    ov_state.markup_active = markup_active_;
    ov_state.bg_alpha = bg_alpha_;
    ov_state.zoom = zoom_;
    ov_state.image_width = decoded_image_.width;
    ov_state.image_height = decoded_image_.height;
    ov_state.image_index = dir_image_index_;
    ov_state.image_count = (int)dir_images_.size();
    ov_state.slideshow_interval_ms = config_.slideshow_interval_ms;
    ov_state.enable_color_management = config_.enable_color_management;
    ov_state.default_zoom = config_.default_zoom;
    ov_state.theme = config_.theme;
    ov_state.exif = exif_info_;
    if (decoded_image_.width <= 0) {
        ov_state.filename.clear();
    } else {
        ov_state.filename = fs::path(current_path_).filename().string();
    }

    if (show_toolbar_) {
        overlay_.render_toolbar(cr, win_w, win_h, toolbar_buttons_,
                                toolbar_hover_idx_, toolbar_press_idx_, bg_alpha_);
        // Fill in button actions after render_toolbar populates geometries
        for (auto& btn : toolbar_buttons_) {
            if (btn.label == "Open") btn.action = [this]() { open_file_dialog(); };
            else if (btn.label == "<") btn.action = [this]() { prev_image(); };
            else if (btn.label == ">") btn.action = [this]() { next_image(); };
            else if (btn.label == "+") btn.action = [this]() { zoom_in(); };
            else if (btn.label == "-") btn.action = [this]() { zoom_out(); };
            else if (btn.label == "Fit") btn.action = [this]() { zoom_fit(); };
            else if (btn.label == "Full") btn.action = [this]() { toggle_fullscreen(); };
            else if (btn.label == "Play") btn.action = [this]() { toggle_slideshow(); };
            else if (btn.label == "Gear") btn.action = [this]() { toggle_settings(); };
            else if (btn.label == "Info") btn.action = [this]() {
                show_sidebar_ = !show_sidebar_;
                render();
            };
            else if (btn.label == "Crop") btn.action = [this]() { toggle_crop(); };
            else if (btn.label == "Draw") btn.action = [this]() { toggle_markup(); };
            else if (btn.label == "RotR") btn.action = [this]() { rotate_90_cw(); };
            else if (btn.label == "RotL") btn.action = [this]() { rotate_90_ccw(); };
            else if (btn.label == "Flip") btn.action = [this]() { flip_horizontal(); };
            else if (btn.label == "Menu") btn.action = [this]() { toggle_menu(); };
        }
    }

    // --- Menu popup ---
    if (show_menu_) {
        std::vector<OverlayButton> menu_buttons;
        overlay_.render_menu_popup(cr, win_w, win_h, ov_state, menu_buttons);
        for (auto& btn : menu_buttons) {
            if (btn.label == "Save") btn.action = [this]() {
                show_menu_ = false;
                save_image();
            };
            else if (btn.label == "Save As") btn.action = [this]() {
                show_menu_ = false;
                save_as();
            };
            else if (btn.label == "Save As Copy") btn.action = [this]() {
                show_menu_ = false;
                save_as_copy();
            };
        }
        toolbar_buttons_.insert(toolbar_buttons_.end(),
                                menu_buttons.begin(), menu_buttons.end());
    }

    // --- Crop overlay ---
    if (crop_active_ && decoded_image_.width > 0) {
        std::vector<OverlayButton> crop_buttons;
        overlay_.render_crop_overlay(cr, win_w, win_h, ov_state, crop_buttons);
        // Draw the actual crop rectangle
        draw_crop_rect(cr, win_w, win_h);
        // Apply / Cancel floating buttons — always at the content area bottom so crop rect never obscures them
        int btn_w = 100, btn_h = 36;
        int btn_y = win_h - strip_h - btn_h - 20;
        int cx = (win_w - (btn_w * 2 + 10)) / 2;
        cairo_set_source_rgba(cr, m3::primary_r, m3::primary_g, m3::primary_b, 0.9);
        overlay_.draw_rounded_rect(cr, cx, btn_y, btn_w, btn_h, 8);
        cairo_fill(cr);
        cairo_set_source_rgba(cr, m3::on_primary_container_r, m3::on_primary_container_g,
                              m3::on_primary_container_b, 1.0);
        cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL,
                               CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(cr, 14);
        cairo_move_to(cr, cx + 30, btn_y + 24);
        cairo_show_text(cr, "Apply");
        crop_buttons.push_back({cx, btn_y, btn_w, btn_h, "CropApply", {}});

        cx += btn_w + 10;
        cairo_set_source_rgba(cr, m3::on_surface_variant_r, m3::on_surface_variant_g,
                              m3::on_surface_variant_b, 0.15);
        overlay_.draw_rounded_rect(cr, cx, btn_y, btn_w, btn_h, 8);
        cairo_fill(cr);
        cairo_set_source_rgba(cr, m3::on_surface_r, m3::on_surface_g,
                              m3::on_surface_b, 0.87);
        cairo_move_to(cr, cx + 26, btn_y + 24);
        cairo_show_text(cr, "Cancel");
        crop_buttons.push_back({cx, btn_y, btn_w, btn_h, "CropCancel", {}});

        for (auto& btn : crop_buttons) {
            if (btn.label == "CropApply") btn.action = [this]() { apply_crop(); };
            else if (btn.label == "CropCancel") btn.action = [this]() { cancel_crop(); };
        }
        toolbar_buttons_.insert(toolbar_buttons_.end(),
                                crop_buttons.begin(), crop_buttons.end());
    }

    // --- Markup overlay ---
    if (markup_active_ && decoded_image_.width > 0) {
        overlay_.render_markup_overlay(cr, win_w, win_h, ov_state);

        std::vector<OverlayButton> markup_buttons;

        // --- Tool submenu ---
        int sub_x = 10;
        int sub_y = Overlay::kToolbarHeight + 10;
        int tool_btn_w = 56, tool_btn_h = 28;
        const char* tool_names[] = {"Pen", "Line", "Arrow", "Rect", "Ellipse"};
        int num_tools = 5;
        cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL,
                               CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(cr, 12);
        for (int i = 0; i < num_tools; i++) {
            int tx = sub_x + i * (tool_btn_w + 4);
            bool active = (int)markup_tool_ == i;
            if (active) {
                cairo_set_source_rgba(cr, m3::primary_container_r, m3::primary_container_g,
                                      m3::primary_container_b, 1.0);
            } else {
                cairo_set_source_rgba(cr, m3::surface_container_high_r, m3::surface_container_high_g,
                                      m3::surface_container_high_b, 0.9);
            }
            overlay_.draw_rounded_rect(cr, tx, sub_y, tool_btn_w, tool_btn_h, 6);
            cairo_fill(cr);
            cairo_set_source_rgba(cr, active ? m3::on_primary_container_r : m3::on_surface_r,
                                  active ? m3::on_primary_container_g : m3::on_surface_g,
                                  active ? m3::on_primary_container_b : m3::on_surface_b, 0.87);
            cairo_move_to(cr, tx + 8, sub_y + 18);
            cairo_show_text(cr, tool_names[i]);
            markup_buttons.push_back({tx, sub_y, tool_btn_w, tool_btn_h,
                                      "MTool" + std::to_string(i), {}});
        }

        // --- Color hue bar ---
        hue_bar_x_ = (float)sub_x;
        hue_bar_y_ = (float)(Overlay::kToolbarHeight + 46);
        // Gradient stops: 7 stops across the hue wheel
        struct { float pos; float r, g, b; } stops[] = {
            {0.00f, 1,0,0}, {0.17f, 1,1,0}, {0.33f, 0,1,0},
            {0.50f, 0,1,1}, {0.67f, 0,0,1}, {0.83f, 1,0,1},
            {1.00f, 1,0,0},
        };
        cairo_save(cr);
        overlay_.draw_rounded_rect(cr, hue_bar_x_, hue_bar_y_, hue_bar_w_, hue_bar_h_, 4);
        cairo_clip(cr);
        for (int s = 0; s < 6; s++) {
            float x0 = hue_bar_x_ + stops[s].pos * hue_bar_w_;
            float x1 = hue_bar_x_ + stops[s + 1].pos * hue_bar_w_;
            cairo_pattern_t* pat = cairo_pattern_create_linear(x0, 0, x1, 0);
            cairo_pattern_add_color_stop_rgba(pat, 0, stops[s].r, stops[s].g, stops[s].b, 1);
            cairo_pattern_add_color_stop_rgba(pat, 1, stops[s+1].r, stops[s+1].g, stops[s+1].b, 1);
            cairo_rectangle(cr, x0, hue_bar_y_, x1 - x0, hue_bar_h_);
            cairo_set_source(cr, pat);
            cairo_fill(cr);
            cairo_pattern_destroy(pat);
        }
        cairo_restore(cr);
        // Thumb showing current hue
        {
            float hue = hue_from_rgb(markup_color_);
            float thumb_cx = hue_bar_x_ + hue * hue_bar_w_;
            float thumb_cy = hue_bar_y_ + hue_bar_h_ * 0.5f;
            cairo_set_source_rgba(cr, 1, 1, 1, 0.9);
            cairo_arc(cr, thumb_cx, thumb_cy, 8, 0, 2 * M_PI);
            cairo_fill(cr);
            cairo_set_source_rgba(cr, 0, 0, 0, 0.3);
            cairo_arc(cr, thumb_cx, thumb_cy, 8, 0, 2 * M_PI);
            cairo_set_line_width(cr, 1.5);
            cairo_stroke(cr);
        }

        // --- Thickness submenu ---
        sub_y = Overlay::kToolbarHeight + 80;
        int thick_btn_w = 56, thick_btn_h = 28;
        float thicknesses[] = {2.0f, 4.0f, 8.0f};
        const char* thick_labels[] = {"Thin", "Med", "Thick"};
        for (int i = 0; i < 3; i++) {
            int tx = sub_x + i * (thick_btn_w + 4);
            bool active = std::abs(markup_thickness_ - thicknesses[i]) < 0.1f;
            if (active) {
                cairo_set_source_rgba(cr, m3::primary_container_r, m3::primary_container_g,
                                      m3::primary_container_b, 1.0);
            } else {
                cairo_set_source_rgba(cr, m3::surface_container_high_r, m3::surface_container_high_g,
                                      m3::surface_container_high_b, 0.9);
            }
            overlay_.draw_rounded_rect(cr, tx, sub_y, thick_btn_w, thick_btn_h, 6);
            cairo_fill(cr);
            cairo_set_source_rgba(cr, active ? m3::on_primary_container_r : m3::on_surface_r,
                                  active ? m3::on_primary_container_g : m3::on_surface_g,
                                  active ? m3::on_primary_container_b : m3::on_surface_b, 0.87);
            cairo_move_to(cr, tx + 10, sub_y + 18);
            cairo_show_text(cr, thick_labels[i]);
            markup_buttons.push_back({tx, sub_y, thick_btn_w, thick_btn_h,
                                      "MThick_" + std::to_string(i), {}});
        }

        // --- Undo button ---
        {
            int ux = sub_x + 3 * (thick_btn_w + 4) + 12;
            int uy = Overlay::kToolbarHeight + 80;
            int uw = 56, uh = 28;
            if (!markup_elements_.empty()) {
                cairo_set_source_rgba(cr, m3::surface_container_high_r, m3::surface_container_high_g,
                                      m3::surface_container_high_b, 0.9);
            } else {
                cairo_set_source_rgba(cr, m3::surface_container_r, m3::surface_container_g,
                                      m3::surface_container_b, 0.5);
            }
            overlay_.draw_rounded_rect(cr, ux, uy, uw, uh, 6);
            cairo_fill(cr);
            cairo_set_source_rgba(cr, m3::on_surface_r, m3::on_surface_g,
                                  m3::on_surface_b, markup_elements_.empty() ? 0.3 : 0.87);
            cairo_move_to(cr, ux + 14, uy + 18);
            cairo_show_text(cr, "Undo");
            markup_buttons.push_back({ux, uy, uw, uh, "MUndo", {}});
        }

        // Apply / Cancel floating buttons
        int btn_w = 100, btn_h = 36;
        int btn_y = win_h - strip_h - btn_h - 20;
        int cx = (win_w - (btn_w * 2 + 10)) / 2;
        cairo_set_source_rgba(cr, m3::primary_r, m3::primary_g, m3::primary_b, 0.9);
        overlay_.draw_rounded_rect(cr, cx, btn_y, btn_w, btn_h, 8);
        cairo_fill(cr);
        cairo_set_source_rgba(cr, m3::on_primary_container_r, m3::on_primary_container_g,
                              m3::on_primary_container_b, 1.0);
        cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL,
                               CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(cr, 14);
        cairo_move_to(cr, cx + 30, btn_y + 24);
        cairo_show_text(cr, "Apply");
        markup_buttons.push_back({cx, btn_y, btn_w, btn_h, "MarkupApply", {}});

        cx += btn_w + 10;
        cairo_set_source_rgba(cr, m3::on_surface_variant_r, m3::on_surface_variant_g,
                              m3::on_surface_variant_b, 0.15);
        overlay_.draw_rounded_rect(cr, cx, btn_y, btn_w, btn_h, 8);
        cairo_fill(cr);
        cairo_set_source_rgba(cr, m3::on_surface_r, m3::on_surface_g,
                              m3::on_surface_b, 0.87);
        cairo_move_to(cr, cx + 26, btn_y + 24);
        cairo_show_text(cr, "Cancel");
        markup_buttons.push_back({cx, btn_y, btn_w, btn_h, "MarkupCancel", {}});

        for (auto& btn : markup_buttons) {
            if (btn.label == "MarkupApply") btn.action = [this]() { commit_markup(); };
            else if (btn.label == "MarkupCancel") btn.action = [this]() { cancel_markup(); };
            else if (btn.label == "MTool0") btn.action = [this]() { markup_tool_ = MarkupTool::kPen; render(); };
            else if (btn.label == "MTool1") btn.action = [this]() { markup_tool_ = MarkupTool::kLine; render(); };
            else if (btn.label == "MTool2") btn.action = [this]() { markup_tool_ = MarkupTool::kArrow; render(); };
            else if (btn.label == "MTool3") btn.action = [this]() { markup_tool_ = MarkupTool::kRect; render(); };
            else if (btn.label == "MTool4") btn.action = [this]() { markup_tool_ = MarkupTool::kEllipse; render(); };
            else if (btn.label == "MHueBar") btn.action = {}; // handled in on_pointer
            else if (btn.label == "MThick_0") btn.action = [this]() { markup_thickness_ = 2.0f; render(); };
            else if (btn.label == "MThick_1") btn.action = [this]() { markup_thickness_ = 4.0f; render(); };
            else if (btn.label == "MThick_2") btn.action = [this]() { markup_thickness_ = 8.0f; render(); };
            else if (btn.label == "MUndo") btn.action = [this]() { undo_markup(); };
        }
        toolbar_buttons_.insert(toolbar_buttons_.end(),
                                markup_buttons.begin(), markup_buttons.end());
    }

    // --- Thumbnail strip (pushed below content area) ---
    if (strip_visible && decoded_image_.width > 0 && dir_images_.size() > 1) {
        if (thumb_dirty_ || cached_strip_w_ != content_w) {
            destroy_cached_strip();
            cached_strip_w_ = content_w;
            cached_strip_ = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, content_w,
                                                       ThumbnailStrip::kHeight);
            cairo_t* strip_cr = cairo_create(cached_strip_);

            // Render with win_h = kHeight so strip_y = 0 within the strip surface
            ThumbnailStripState ts;
            ts.image_index = dir_image_index_;
            ts.image_count = (int)dir_images_.size();
            ts.win_w = content_w;
            ts.win_h = ThumbnailStrip::kHeight;
            ts.scroll_offset = thumb_scroll_;
            ts.cache = &thumb_cache_;
            ts.cache_w = &thumb_cache_w_;
            ts.cache_h = &thumb_cache_h_;
            thumbnail_strip_.render(strip_cr, ts, thumb_entries_);
            // Fill in click actions
            for (auto& e : thumb_entries_) {
                if (e.current) continue;
                int idx = e.index;
                e.action = [this, idx]() {
                    if (idx >= 0 && idx < (int)dir_images_.size()) {
                        dir_image_index_ = idx;
                        open_file(dir_images_[idx]);
                    }
                };
            }

            cairo_destroy(strip_cr);
            // Convert entry coords from strip-relative to window-relative
            // since hit-testing uses window coords
            {
                int strip_y_win = win_h - strip_h;
                for (auto& e : thumb_entries_) e.y += strip_y_win;
            }
            thumb_dirty_ = false;
        }
        // Blit cached strip
        cairo_set_source_surface(cr, cached_strip_, 0, win_h - strip_h);
        cairo_paint(cr);
    }

    // --- Overlay info / placeholder (drawn on top of thumbnail strip) ---
    overlay_.render_overlay(cr, win_w, win_h, ov_state);

    // --- Settings popup (drawn on top of everything) ---
    if (show_settings_) {
        // Sync M3 widget colors with current theme tokens
        bg_alpha_slider_.setAccentColor(m3::primary_r, m3::primary_g, m3::primary_b);
        bg_alpha_slider_.setSurfaceColor(m3::surface_r, m3::surface_g, m3::surface_b);
        default_zoom_slider_.setAccentColor(m3::primary_r, m3::primary_g, m3::primary_b);
        default_zoom_slider_.setSurfaceColor(m3::surface_r, m3::surface_g, m3::surface_b);
        ss_interval_slider_.setAccentColor(m3::primary_r, m3::primary_g, m3::primary_b);
        ss_interval_slider_.setSurfaceColor(m3::surface_r, m3::surface_g, m3::surface_b);
        theme_toggle_.setAccentColor(m3::primary_r, m3::primary_g, m3::primary_b);
        theme_toggle_.setOutlineColor(m3::outline_r, m3::outline_g, m3::outline_b);
        color_mgmt_toggle_.setAccentColor(m3::primary_r, m3::primary_g, m3::primary_b);
        color_mgmt_toggle_.setOutlineColor(m3::outline_r, m3::outline_g, m3::outline_b);

        // Sync M3 widget state with config
        bg_alpha_slider_.setValue(ov_state.bg_alpha);
        default_zoom_slider_.setValue(ov_state.default_zoom);
        ss_interval_slider_.setValue((float)ov_state.slideshow_interval_ms);
        theme_toggle_.setOn(ov_state.theme == "dark");
        color_mgmt_toggle_.setOn(ov_state.enable_color_management);

        std::vector<OverlayButton> settings_buttons;
        overlay_.render_settings_popup(cr, win_w, win_h, ov_state, settings_buttons,
                                        bg_alpha_slider_, default_zoom_slider_,
                                        ss_interval_slider_, theme_toggle_,
                                        color_mgmt_toggle_);
        for (auto& btn : settings_buttons) {
            if (btn.label == "CloseSettings") {
                btn.action = [this]() { toggle_settings(); };
            }
        }
        toolbar_buttons_.insert(toolbar_buttons_.end(),
                                settings_buttons.begin(), settings_buttons.end());
    }

    // --- Sidebar (drawn in its dedicated area — no dim overlay) ---
    if (show_sidebar_) {
        std::vector<OverlayButton> sidebar_buttons;
        overlay_.render_sidebar(cr, win_w, win_h, ov_state, sidebar_buttons);
        for (auto& btn : sidebar_buttons) {
            if (btn.label == "CloseSidebar") {
                btn.action = [this]() { toggle_sidebar(); };
            }
        }
        toolbar_buttons_.insert(toolbar_buttons_.end(),
                                sidebar_buttons.begin(), sidebar_buttons.end());
    }

    cairo_destroy(cr);
    cairo_surface_destroy(cs);

    // Mark buffer as busy, attach, damage, commit
    bufs_[bi].mark_busy();
    wl_surface_attach(surface_, bufs_[bi].buffer(), 0, 0);
    wl_surface_damage_buffer(surface_, 0, 0, win_w, win_h);
    wl_surface_commit(surface_);
    wl_display_flush(conn_.display());

    // Toggle buffer for next frame
    paint_buf_ = 1 - bi;
}

void App::on_shm_release() {
    if (pending_redraw_) {
        pending_redraw_ = false;
        render();
    }
}


// --- Event handlers ---

void App::set_window_size(int width, int height) {
    if (width > 0 && height > 0 &&
        (width != window_width_ || height != window_height_)) {
        window_width_ = width;
        window_height_ = height;
        render();
    }
}

void App::on_close() {
    running_ = false;
}

void App::on_key(const KeyEvent& ev) {
    if (ev.state != WL_KEYBOARD_KEY_STATE_PRESSED) return;

    switch (ev.sym) {
        case XKB_KEY_q:
            if (ev.ctrl) quit();
            break;
        case XKB_KEY_z:
            if (ev.ctrl && markup_active_) { undo_markup(); break; }
            break;
        case XKB_KEY_o:
            if (ev.ctrl) open_file_dialog();
            break;
        case XKB_KEY_f:
        case XKB_KEY_F11:
            toggle_fullscreen();
            break;
        case XKB_KEY_Escape:
            if (markup_active_) { cancel_markup(); break; }
            if (crop_active_) { cancel_crop(); break; }
            if (show_menu_) { show_menu_ = false; render(); break; }
            if (show_sidebar_) { toggle_sidebar(); break; }
            if (show_settings_) { toggle_settings(); break; }
            if (fullscreen_) toggle_fullscreen();
            else quit();
            break;
        case XKB_KEY_Right:
        case XKB_KEY_j:
            next_image();
            break;
        case XKB_KEY_Left:
        case XKB_KEY_k:
            prev_image();
            break;
        case XKB_KEY_Home:
            first_image();
            break;
        case XKB_KEY_End:
            last_image();
            break;
        case XKB_KEY_plus:
        case XKB_KEY_equal:
            if (ev.ctrl) zoom_in();
            break;
        case XKB_KEY_minus:
            if (ev.ctrl) zoom_out();
            break;
        case XKB_KEY_0:
            if (ev.ctrl) zoom_fit();
            break;
        case XKB_KEY_1:
            if (ev.ctrl) zoom_1to1();
            break;
        case XKB_KEY_space:
            toggle_slideshow();
            break;
        case XKB_KEY_i:
            toggle_overlay();
            break;
        case XKB_KEY_Return:
        case XKB_KEY_KP_Enter:
            if (markup_active_) { commit_markup(); break; }
            if (crop_active_) { apply_crop(); break; }
            break;
        case XKB_KEY_r:
            if (ev.shift) rotate_90_ccw();
            else rotate_90_cw();
            break;
        case XKB_KEY_h:
            flip_horizontal();
            break;
        case XKB_KEY_v:
            flip_vertical();
            break;
        case XKB_KEY_Delete:
        case XKB_KEY_KP_Delete:
            delete_image();
            break;
        default:
            break;
    }
}

void App::on_pointer(const PointerEvent& ev) {
    if (ev.button != 0x110) return; // BTN_LEFT

    if (ev.state == WL_POINTER_BUTTON_STATE_PRESSED) {
        // Sidebar hit-test — consume clicks below toolbar area
        if (show_sidebar_ && ev.y >= Overlay::kToolbarHeight) {
            int sw = 320;
            int sx = window_width_ - sw;
            if (ev.x >= sx) {
                for (auto& btn : toolbar_buttons_) {
                    if (ev.x >= btn.x && ev.x < btn.x + btn.w &&
                        ev.y >= btn.y && ev.y < btn.y + btn.h && btn.action) {
                        if (btn.label == "CloseSidebar") {
                            toggle_sidebar();
                            return;
                        }
                    }
                }
                return; // consume click inside sidebar
            }
            // Click outside sidebar — let it pass through to other handlers
        }

        // Settings popup hit-test takes priority
        if (show_settings_) {
            int pw = 480, ph = 540;
            int px = (window_width_ - pw) / 2;
            int py = (window_height_ - ph) / 2;

            // Click outside popup closes it
            if (!(ev.x >= px && ev.x < px + pw && ev.y >= py && ev.y < py + ph)) {
                toggle_settings();
                return;
            }

            // 1. M3 slider dispatch
            if (bg_alpha_slider_.handlePointerDown(ev.x, ev.y)) {
                active_slider_ = &bg_alpha_slider_;
                set_bg_alpha(bg_alpha_slider_.value());
                return;
            }
            if (default_zoom_slider_.handlePointerDown(ev.x, ev.y)) {
                active_slider_ = &default_zoom_slider_;
                set_default_zoom(default_zoom_slider_.value());
                return;
            }
            if (ss_interval_slider_.handlePointerDown(ev.x, ev.y)) {
                active_slider_ = &ss_interval_slider_;
                set_slideshow_interval((int)ss_interval_slider_.value());
                return;
            }

            // 2. M3 toggle dispatch
            if (theme_toggle_.containsPoint(ev.x, ev.y)) {
                toggle_theme();
                return;
            }
            if (color_mgmt_toggle_.containsPoint(ev.x, ev.y)) {
                toggle_color_management();
                return;
            }

            // 3. Regular buttons (close, etc.)
            for (auto& btn : toolbar_buttons_) {
                if (ev.x >= btn.x && ev.x < btn.x + btn.w &&
                    ev.y >= btn.y && ev.y < btn.y + btn.h && btn.action) {
                    btn.action();
                    return;
                }
            }

            return; // consume click inside popup even if not on a button
        }

        // Menu popup: close on outside click
        if (show_menu_ && ev.y > Overlay::kToolbarHeight) {
            int pw = 200, ph = 160;
            int px = window_width_ - pw - 8;
            int py = Overlay::kToolbarHeight + 4;
            if (!(ev.x >= px && ev.x < px + pw && ev.y >= py && ev.y < py + ph)) {
                show_menu_ = false;
                render();
                return;
            }
        }

        // Crop mode: drag rectangle or handles
        if (crop_active_) {
            if (ev.y > Overlay::kToolbarHeight && decoded_image_.width > 0) {
                int img_x, img_y;
                win_to_img(ev.x, ev.y, img_x, img_y);

                // Check corner handles (in image coords)
                int hl = 10;
                auto near_handle = [&](int hx, int hy) -> bool {
                    int wx, wy;
                    img_to_win(hx, hy, wx, wy);
                    return abs(ev.x - wx) < hl * 2 && abs(ev.y - wy) < hl * 2;
                };

                crop_drag_handle_ = CropNone;
                if (near_handle(crop_x_, crop_y_)) crop_drag_handle_ = CropTL;
                else if (near_handle(crop_x_ + crop_w_, crop_y_)) crop_drag_handle_ = CropTR;
                else if (near_handle(crop_x_, crop_y_ + crop_h_)) crop_drag_handle_ = CropBL;
                else if (near_handle(crop_x_ + crop_w_, crop_y_ + crop_h_)) crop_drag_handle_ = CropBR;
                else if (img_x >= crop_x_ && img_x < crop_x_ + crop_w_ &&
                         img_y >= crop_y_ && img_y < crop_y_ + crop_h_)
                    crop_drag_handle_ = CropMove;

                if (crop_drag_handle_ != CropNone) {
                    crop_dragging_ = true;
                    crop_drag_start_x_ = img_x;
                    crop_drag_start_y_ = img_y;
                    crop_drag_orig_x_ = crop_x_;
                    crop_drag_orig_y_ = crop_y_;
                    crop_drag_orig_w_ = crop_w_;
                    crop_drag_orig_h_ = crop_h_;
                    return;
                }
            }
            // If click was on toolbar buttons (Apply/Cancel), let them handle it
        }

        // Hit-test toolbar buttons first (covers markup submenus, apply/cancel)
        if (show_toolbar_) {
            for (int i = 0; i < (int)toolbar_buttons_.size(); i++) {
                auto& btn = toolbar_buttons_[i];
                if (ev.x >= btn.x && ev.x < btn.x + btn.w &&
                    ev.y >= btn.y && ev.y < btn.y + btn.h && btn.action) {
                    toolbar_press_idx_ = i;
                    btn.action();  // triggers render(), showing press state
                    return;
                }
            }
        }

        // Markup mode: hue bar drag (before drawing starts)
        if (markup_active_ && !crop_active_ && decoded_image_.width > 0 &&
            ev.x >= hue_bar_x_ && ev.x < hue_bar_x_ + hue_bar_w_ &&
            ev.y >= hue_bar_y_ && ev.y < hue_bar_y_ + hue_bar_h_) {
            hue_bar_dragging_ = true;
            float t = (ev.x - hue_bar_x_) / hue_bar_w_;
            if (t < 0) t = 0;
            if (t > 1) t = 1;
            markup_color_ = hsv_to_rgb(t * 360.0f, 1.0f, 1.0f);
            render();
            return;
        }

        // Markup mode: start drawing (after button hit-test and hue bar so submenus take priority)
        if (markup_active_ && !crop_active_) {
            if (ev.y > Overlay::kToolbarHeight && decoded_image_.width > 0) {
                int img_x, img_y;
                win_to_img(ev.x, ev.y, img_x, img_y);
                markup_drawing_ = true;
                markup_drag_start_x_ = (float)img_x;
                markup_drag_start_y_ = (float)img_y;
                auto el = std::make_unique<MarkupElement>();
                el->type = markup_tool_;
                el->color = markup_color_;
                el->thickness = markup_thickness_;
                el->points_x.push_back((float)img_x);
                el->points_y.push_back((float)img_y);
                markup_current_ = std::move(el);
                return;
            }
        }

        // Hit-test thumbnail strip
        bool strip_visible = show_thumbnails_ && (!(fullscreen_ || slideshow_) || show_thumbnails_hover_);
        int strip_h = (strip_visible && decoded_image_.width > 0 && dir_images_.size() > 1)
                          ? ThumbnailStrip::kHeight : 0;
        int strip_y = window_height_ - strip_h;
        if (strip_visible && ev.y >= strip_y && !thumb_entries_.empty()) {
            for (auto& e : thumb_entries_) {
                if (ev.x >= e.x && ev.x < e.x + e.w &&
                    ev.y >= e.y && ev.y < e.y + e.h && e.action) {
                    // Double-click detection: same thumb within 500ms
                    uint32_t now = ev.time;
                    if (e.index == last_thumb_click_index_ &&
                        now - last_thumb_click_time_ < 500) {
                        // Double-click — already navigated on first click,
                        // just open sidebar for the now-current image
                        last_thumb_click_index_ = -1;
                        if (!show_sidebar_) {
                            show_sidebar_ = true;
                            render();
                        }
                        return;
                    }
                    last_thumb_click_index_ = e.index;
                    last_thumb_click_time_ = now;
                    e.action(); // single-click navigate
                    return;
                }
            }
        }

        // Start drag-to-pan immediately
        if (!show_toolbar_ || ev.y > Overlay::kToolbarHeight) {
            dragging_ = true;
            drag_start_x_ = ev.x;
            drag_start_y_ = ev.y;
            pan_start_x_ = pan_x_;
            pan_start_y_ = pan_y_;
        }
    } else if (ev.state == WL_POINTER_BUTTON_STATE_RELEASED) {
        if (active_slider_) {
            active_slider_->handlePointerUp(ev.x, ev.y);
            active_slider_ = nullptr;
            return;
        }
        if (toolbar_press_idx_ >= 0) {
            toolbar_press_idx_ = -1;
            pending_redraw_ = true;
        }
        if (dragging_) {
            dragging_ = false;
        }
        if (hue_bar_dragging_) {
            hue_bar_dragging_ = false;
        }
        if (crop_dragging_) {
            crop_dragging_ = false;
        }
        if (markup_drawing_) {
            markup_drawing_ = false;
            if (markup_current_) {
                auto tool = markup_current_->type;

                //--- Pen: smooth with Catmull-Rom ---
                if (tool == MarkupTool::kPen &&
                    markup_current_->points_x.size() > 3) {
                    auto& px = markup_current_->points_x;
                    auto& py = markup_current_->points_y;
                    size_t n = px.size();

                    std::vector<double> sim_flat;
                    sim_flat.reserve(n * 2);
                    std::vector<double> raw_flat;
                    raw_flat.reserve(n * 2);
                    for (size_t i = 0; i < n; i++) {
                        raw_flat.push_back((double)px[i]);
                        raw_flat.push_back((double)py[i]);
                    }
                    psimpl::simplify_radial_distance<2>(raw_flat.begin(), raw_flat.end(),
                                                        1.0, std::back_inserter(sim_flat));
                    size_t sim_n = sim_flat.size() / 2;

                    if (sim_n >= 4) {
                        try {
                            auto spline = tinyspline::BSpline::interpolateCatmullRom(
                                sim_flat, 2, 0.5f);
                            int num_samples = std::max(64, (int)sim_n * 4);
                            auto sampled = spline.sample(num_samples);
                            px.clear();
                            py.clear();
                            px.reserve(sampled.size() / 2);
                            py.reserve(sampled.size() / 2);
                            for (size_t i = 0; i < sampled.size(); i += 2) {
                                px.push_back((float)sampled[i]);
                                py.push_back((float)sampled[i + 1]);
                            }
                        } catch (const std::exception& e) {
                            std::cerr << "Catmull-Rom smoothing failed: "
                                      << e.what() << "\n";
                        }
                    }
                }

                //--- Rect/Ellipse: no point list needed, use rect_x/y/w/h ---
                if (tool == MarkupTool::kRect || tool == MarkupTool::kEllipse) {
                    markup_current_->points_x.clear();
                    markup_current_->points_y.clear();
                }

                //--- Numbered marker: store number text ---
                if (tool == MarkupTool::kNumbered) {
                    numbered_count_++;
                    markup_current_->text = std::to_string(numbered_count_);
                }

                markup_elements_.push_back(std::move(*markup_current_));
                markup_current_.reset();
            }
            render();
        }
    }
}

void App::on_motion(int x, int y) {
    pointer_x_ = x;
    pointer_y_ = y;

    // 1. Active settings slider drag (highest priority)
    if (active_slider_) {
        if (!show_settings_) {
            active_slider_->handlePointerUp(0, 0);
            active_slider_ = nullptr;
            return;
        }
        active_slider_->handlePointerMove(x, y);
        if (active_slider_ == &bg_alpha_slider_) {
            set_bg_alpha(bg_alpha_slider_.value());
        } else if (active_slider_ == &default_zoom_slider_) {
            set_default_zoom(default_zoom_slider_.value());
        } else if (active_slider_ == &ss_interval_slider_) {
            set_slideshow_interval((int)ss_interval_slider_.value());
        }
        return;
    }

    // 1.5 Hue bar drag
    if (hue_bar_dragging_) {
        float t = (x - hue_bar_x_) / hue_bar_w_;
        if (t < 0) t = 0;
        if (t > 1) t = 1;
        markup_color_ = hsv_to_rgb(t * 360.0f, 1.0f, 1.0f);
        render();
        return;
    }

    // 2. Crop drag
    if (crop_active_ && crop_dragging_) {
        if (decoded_image_.width <= 0) { crop_dragging_ = false; return; }
        int img_x, img_y;
        win_to_img(x, y, img_x, img_y);
        int dx = img_x - crop_drag_start_x_;
        int dy = img_y - crop_drag_start_y_;

        int min_dim = 16;
        switch (crop_drag_handle_) {
        case CropMove:
            crop_x_ = std::clamp(crop_drag_orig_x_ + dx, 0, decoded_image_.width - crop_w_);
            crop_y_ = std::clamp(crop_drag_orig_y_ + dy, 0, decoded_image_.height - crop_h_);
            break;
        case CropTL:
            crop_x_ = std::clamp(crop_drag_orig_x_ + dx, 0, crop_drag_orig_x_ + crop_drag_orig_w_ - min_dim);
            crop_y_ = std::clamp(crop_drag_orig_y_ + dy, 0, crop_drag_orig_y_ + crop_drag_orig_h_ - min_dim);
            crop_w_ = crop_drag_orig_w_ + (crop_drag_orig_x_ - crop_x_);
            crop_h_ = crop_drag_orig_h_ + (crop_drag_orig_y_ - crop_y_);
            break;
        case CropTR:
            crop_y_ = std::clamp(crop_drag_orig_y_ + dy, 0, crop_drag_orig_y_ + crop_drag_orig_h_ - min_dim);
            crop_w_ = std::max(min_dim, crop_drag_orig_w_ + dx);
            crop_h_ = crop_drag_orig_h_ + (crop_drag_orig_y_ - crop_y_);
            break;
        case CropBL:
            crop_x_ = std::clamp(crop_drag_orig_x_ + dx, 0, crop_drag_orig_x_ + crop_drag_orig_w_ - min_dim);
            crop_w_ = crop_drag_orig_w_ + (crop_drag_orig_x_ - crop_x_);
            crop_h_ = std::max(min_dim, crop_drag_orig_h_ + dy);
            break;
        case CropBR:
            crop_w_ = std::max(min_dim, crop_drag_orig_w_ + dx);
            crop_h_ = std::max(min_dim, crop_drag_orig_h_ + dy);
            break;
        default: break;
        }
        // Clamp to image bounds
        int max_x = decoded_image_.width - crop_w_;
        if (crop_x_ < 0) crop_x_ = 0;
        else if (crop_x_ > max_x) crop_x_ = max_x;
        int max_y = decoded_image_.height - crop_h_;
        if (crop_y_ < 0) crop_y_ = 0;
        else if (crop_y_ > max_y) crop_y_ = max_y;
        if (crop_w_ > decoded_image_.width) crop_w_ = decoded_image_.width;
        if (crop_h_ > decoded_image_.height) crop_h_ = decoded_image_.height;

        render();
        return;
    }

    // 2.5 Markup draw
    if (markup_active_ && markup_drawing_ && markup_current_) {
        int img_x, img_y;
        win_to_img(x, y, img_x, img_y);
        auto tool = markup_current_->type;
        if (tool == MarkupTool::kPen) {
            markup_current_->points_x.push_back((float)img_x);
            markup_current_->points_y.push_back((float)img_y);
        } else if (tool == MarkupTool::kLine || tool == MarkupTool::kArrow) {
            markup_current_->points_x.resize(2);
            markup_current_->points_y.resize(2);
            markup_current_->points_x[1] = (float)img_x;
            markup_current_->points_y[1] = (float)img_y;
        } else if (tool == MarkupTool::kRect || tool == MarkupTool::kEllipse) {
            float dx = (float)img_x - markup_drag_start_x_;
            float dy = (float)img_y - markup_drag_start_y_;
            if (dx >= 0) { markup_current_->rect_x = markup_drag_start_x_; markup_current_->rect_w = dx; }
            else { markup_current_->rect_x = (float)img_x; markup_current_->rect_w = -dx; }
            if (dy >= 0) { markup_current_->rect_y = markup_drag_start_y_; markup_current_->rect_h = dy; }
            else { markup_current_->rect_y = (float)img_y; markup_current_->rect_h = -dy; }
        }
        render();
        return;
    }

    // 3. Toolbar button hover tracking
    {
        int new_hover = -1;
        if (show_toolbar_) {
            for (int i = 0; i < (int)toolbar_buttons_.size(); i++) {
                auto& btn = toolbar_buttons_[i];
                if (x >= btn.x && x < btn.x + btn.w &&
                    y >= btn.y && y < btn.y + btn.h) {
                    new_hover = i;
                    break;
                }
            }
        }
        // Clear press if pointer leaves the pressed button
        if (toolbar_press_idx_ >= 0 && new_hover != toolbar_press_idx_) {
            toolbar_press_idx_ = -1;
            pending_redraw_ = true;
        }
        if (new_hover != toolbar_hover_idx_) {
            toolbar_hover_idx_ = new_hover;
            pending_redraw_ = true;
        }
    }

    // 3. Fullscreen/slideshow toolbar hover zone
    if (fullscreen_ || slideshow_) {
        bool in_zone = y < Overlay::kToolbarHoverZone;
        if (in_zone) {
            if (!show_toolbar_) {
                show_toolbar_ = true;
                pending_redraw_ = true;
            }
            toolbar_hide_time_ = 0;
            return;
        }
        if (show_toolbar_ && toolbar_hide_time_ == 0) {
            struct timespec ts;
            clock_gettime(CLOCK_MONOTONIC, &ts);
            toolbar_hide_time_ = (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
        }

        bool in_strip_zone = y >= window_height_ - ThumbnailStrip::kHeight;
        if (in_strip_zone != show_thumbnails_hover_) {
            show_thumbnails_hover_ = in_strip_zone;
            pending_redraw_ = true;
            return;
        }
        if (show_thumbnails_hover_) return;
    }

    // 3. Image drag-to-pan
    if (decoded_image_.width <= 0 || !dragging_) return;

    pan_x_ = pan_start_x_ + (float)(x - drag_start_x_);
    pan_y_ = pan_start_y_ + (float)(y - drag_start_y_);
    pending_redraw_ = true;
}

void App::on_scroll(const ScrollEvent& ev) {
    // Thumbnail strip horizontal scroll
    bool strip_active = show_thumbnails_ && (!(fullscreen_ || slideshow_) || show_thumbnails_hover_);
    int strip_h_scroll = (strip_active && decoded_image_.width > 0 && dir_images_.size() > 1)
                             ? ThumbnailStrip::kHeight : 0;
    if (strip_active && !dir_images_.empty() &&
        pointer_y_ >= window_height_ - strip_h_scroll) {
        int entry_w = ThumbnailStrip::kThumbW + ThumbnailStrip::kGap;
        int max_scroll = std::max(0, (int)dir_images_.size() * entry_w + ThumbnailStrip::kMargin - window_width_);
        thumb_scroll_ += ev.discrete_x * entry_w;
        if (ev.discrete_y != 0) thumb_scroll_ += ev.discrete_y * entry_w;
        thumb_scroll_ = std::max(0, std::min(thumb_scroll_, max_scroll));
        invalidate_thumb_strip();
        render();
        return;
    }

    if (decoded_image_.width <= 0) return;
    if (ev.discrete_y > 0) zoom_out();
    else if (ev.discrete_y < 0) zoom_in();
}

// --- Menu ---

void App::toggle_menu() {
    show_menu_ = !show_menu_;
    if (show_menu_) {
        show_settings_ = false;
    }
    render();
}

// --- View state ---

void App::toggle_fullscreen() {
    if (!xdg_toplevel_) return;
    fullscreen_ = !fullscreen_;
    show_toolbar_ = !fullscreen_;
    toolbar_hide_time_ = 0;
    if (fullscreen_) {
        xdg_toplevel_set_fullscreen(xdg_toplevel_, nullptr);
    } else {
        xdg_toplevel_unset_fullscreen(xdg_toplevel_);
    }
    render();
}

void App::toggle_slideshow() {
    slideshow_ = !slideshow_;
    if (slideshow_) {
        std::cout << "Slideshow started\n";
    } else {
        std::cout << "Slideshow stopped\n";
    }
}

void App::toggle_overlay() {
    show_overlay_ = !show_overlay_;
    render();
}

void App::toggle_settings() {
    show_settings_ = !show_settings_;
    if (!show_settings_ && active_slider_) {
        active_slider_->handlePointerUp(0, 0);
        active_slider_ = nullptr;
    }
    render();
}

void App::toggle_sidebar() {
    show_sidebar_ = !show_sidebar_;
    render();
}

void App::set_bg_alpha(float a) {
    bg_alpha_ = std::max(0.0f, std::min(1.0f, a));
    config_.bg_alpha = bg_alpha_;
    Config::save(config_);
    render();
}

void App::set_slideshow_interval(int ms) {
    ms = std::max(1000, std::min(30000, ms));
    config_.slideshow_interval_ms = ms;
    Config::save(config_);
    render();
}

void App::toggle_color_management() {
    config_.enable_color_management = !config_.enable_color_management;
    Config::save(config_);
    render();
}

void App::set_default_zoom(float z) {
    z = std::max(0.5f, std::min(5.0f, z));
    config_.default_zoom = z;
    Config::save(config_);
    render();
}

void App::toggle_theme() {
    config_.theme = (config_.theme == "dark") ? "light" : "dark";
    Config::save(config_);
    render();
}

// --- Rotation / Flip ---

void App::rotate_90_cw() {
    rotation_ = (rotation_ + 90) % 360;
    image_modified_ = true;
    render();
}

void App::rotate_90_ccw() {
    rotation_ = (rotation_ - 90 + 360) % 360;
    image_modified_ = true;
    render();
}

void App::flip_horizontal() {
    flip_h_ = !flip_h_;
    image_modified_ = true;
    render();
}

void App::flip_vertical() {
    flip_v_ = !flip_v_;
    image_modified_ = true;
    render();
}

int App::run() {
    struct pollfd fds[2];
    fds[0].fd = wl_display_get_fd(conn_.display());
    fds[0].events = POLLIN;

    int dbus_fd = portal_dialog_.get_fd();
    fds[1].fd = dbus_fd;
    fds[1].events = POLLIN;

    int nfds = dbus_fd >= 0 ? 2 : 1;

    std::cout << "Horizon Photo Viewer started\n";

    while (running_) {
        wl_display_flush(conn_.display());

        int timeout = slideshow_ ? config_.slideshow_interval_ms : -1;
        int ret = poll(fds, nfds, timeout);
        if (ret < 0) break;

        if (ret > 0) {
            if (nfds > 1 && (fds[1].revents & POLLIN)) {
                portal_dialog_.dispatch();
            }
            if (fds[0].revents & POLLIN) {
                wl_display_dispatch(conn_.display());
            }
        } else if (ret == 0 && slideshow_) {
            next_image();
        }

        // Toolbar auto-hide: check if 2s have elapsed since cursor left zone
        if (toolbar_hide_time_ > 0 && (fullscreen_ || slideshow_)) {
            struct timespec ts;
            clock_gettime(CLOCK_MONOTONIC, &ts);
            int64_t now = (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
            static constexpr int64_t kToolbarHideDelayMs = 2000;
            if (now - toolbar_hide_time_ >= kToolbarHideDelayMs) {
                toolbar_hide_time_ = 0;
                show_toolbar_ = false;
                pending_redraw_ = true;
            }
        }

        process_thumb_batch();

        if (pending_redraw_) {
            render();
        }
    }

    return 0;
}

}
