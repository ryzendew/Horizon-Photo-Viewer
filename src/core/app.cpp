#include "core/app.hpp"
#include "decode/svg_render.hpp"

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

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb/stb_image_write.h"

#include "psimpl/psimpl.h"
#include "tinyspline/tinysplinecxx.h"
#include <nanosvg/nanosvg.h>
#include <nanosvg/nanosvgrast.h>

namespace hpv {
namespace fs = std::filesystem;

static float hue_from_rgb(uint32_t rgba) {
    float r = ((rgba >> 24) & 0xFF) / 255.0f;
    float g = ((rgba >> 16) & 0xFF) / 255.0f;
    float b = ((rgba >> 8) & 0xFF) / 255.0f;
    float mx = std::max({r, g, b}), mn = std::min({r, g, b});
    if (mx - mn < 0.001f) return 0;
    float d = mx - mn, h;
    if (mx == r) h = (g - b) / d + (g < b ? 6 : 0);
    else if (mx == g) h = (b - r) / d + 2;
    else h = (r - g) / d + 4;
    return h / 6.0f;
}

static uint32_t hsv_to_rgb(float h, float s, float v) {
    float c = v * s;
    float hp = h / 60.0f;
    float x = c * (1.0f - std::abs(fmod(hp, 2.0f) - 1.0f));
    float r, g, b;
    int i = (int)hp % 6;
    if (i == 0)      { r = c; g = x; b = 0; }
    else if (i == 1) { r = x; g = c; b = 0; }
    else if (i == 2) { r = 0; g = c; b = x; }
    else if (i == 3) { r = 0; g = x; b = c; }
    else if (i == 4) { r = x; g = 0; b = c; }
    else             { r = c; g = 0; b = x; }
    float m = v - c;
    return ((uint32_t)((r + m) * 255) << 24) |
           ((uint32_t)((g + m) * 255) << 16) |
           ((uint32_t)((b + m) * 255) << 8) |
           0xFF;
}
}

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
            const char* crop_paths[] = {
#ifdef CROP_SVG_PATH_SYSTEM
                CROP_SVG_PATH_SYSTEM,
#endif
                "assets/crop.svg",
                "../assets/crop.svg",
                nullptr,
            };
            for (const char** cp = crop_paths; *cp; cp++) {
                if (overlay_.init_icons(*ip, *cp) && overlay_.crop_icon_loaded()) {
                    icons_loaded = true;
                    break;
                }
            }
            if (icons_loaded) break;
        }
        if (!icons_loaded)
            std::cerr << "overlay: icons (font + crop svg) not found\n";
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
    if (decoded_image_.rgba.empty() && !current_path_.empty()) {
        load_image(current_path_);
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
        int avail_h = win_h - (show_toolbar_ ? Overlay::kToolbarHeight : 0) - strip_h;
        float fit_scale = std::min((float)content_w / img_w, (float)avail_h / img_h) * zoom_;
        int draw_w = std::max(1, (int)(img_w * fit_scale));
        int draw_h = std::max(1, (int)(img_h * fit_scale));

        int offset_x = (content_w - draw_w) / 2 + (int)pan_x_;
        int offset_y = (show_toolbar_ ? Overlay::kToolbarHeight : 0) + (avail_h - draw_h) / 2 + (int)pan_y_;

        cairo_save(cr);
        cairo_translate(cr, offset_x, offset_y);

        if (svg_parsed_) {
            // --- Vector cache (Cairo path rendering, crisp at any zoom) ---
            // If cache exists at the right size, display it immediately.
            // If size changed, display old cache scaled and schedule rebuild.
            // If no cache yet, build it now.
            if (svg_vector_cache_ && svg_vector_w_ == draw_w && svg_vector_h_ == draw_h) {
                // Cache matches display size — instant display
                cairo_set_source_surface(cr, svg_vector_cache_, 0, 0);
                cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_NEAREST);
                cairo_paint(cr);
            } else if (svg_vector_cache_) {
                // Size mismatch — display old cache scaled, schedule rebuild
                cairo_save(cr);
                double sx = (double)draw_w / svg_vector_w_;
                double sy = (double)draw_h / svg_vector_h_;
                cairo_scale(cr, sx, sy);
                cairo_set_source_surface(cr, svg_vector_cache_, 0, 0);
                cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
                cairo_paint(cr);
                cairo_restore(cr);
                if (!svg_vector_pending_) {
                    svg_vector_pending_ = true;
                    render();
                }
            } else {
                // First frame — build cache at display size
                svg_vector_cache_ = cairo_image_surface_create(
                    CAIRO_FORMAT_ARGB32, draw_w, draw_h);
                svg_vector_w_ = draw_w;
                svg_vector_h_ = draw_h;
                cairo_t* vcr = cairo_create(svg_vector_cache_);
                cairo_scale(vcr, (double)draw_w / orig_img_w_,
                                  (double)draw_h / orig_img_h_);
                {
                    struct timespec tv0, tv1;
                    clock_gettime(CLOCK_MONOTONIC, &tv0);
                    render_svg_cairo(vcr, svg_parsed_, orig_img_w_, orig_img_h_);
                    if (svg_embedded_.valid()) {
                        cairo_surface_t* es = cairo_image_surface_create_for_data(
                            svg_embedded_.bgra.data(), CAIRO_FORMAT_ARGB32,
                            svg_embedded_.img_w, svg_embedded_.img_h,
                            svg_embedded_.img_w * 4);
                        cairo_save(vcr);
                        cairo_translate(vcr, svg_embedded_.x, svg_embedded_.y);
                        cairo_scale(vcr,
                            (double)svg_embedded_.w / svg_embedded_.img_w,
                            (double)svg_embedded_.h / svg_embedded_.img_h);
                        cairo_set_source_surface(vcr, es, 0, 0);
                        cairo_paint(vcr);
                        cairo_surface_destroy(es);
                        cairo_restore(vcr);
                    }
                    clock_gettime(CLOCK_MONOTONIC, &tv1);
                    int64_t v_us = (tv1.tv_sec - tv0.tv_sec) * 1000000 +
                                   (tv1.tv_nsec - tv0.tv_nsec) / 1000;
                    std::cerr << "[svg] vector: " << v_us << " us ("
                              << draw_w << "x" << draw_h << ")\n";
                }
                cairo_destroy(vcr);
                cairo_set_source_surface(cr, svg_vector_cache_, 0, 0);
                cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_NEAREST);
                cairo_paint(cr);
            }

            // If a rebuild was pending, build new cache and blit to display
            if (svg_vector_pending_) {
                cairo_surface_t* new_cv = cairo_image_surface_create(
                    CAIRO_FORMAT_ARGB32, draw_w, draw_h);
                cairo_t* vcr = cairo_create(new_cv);
                cairo_scale(vcr, (double)draw_w / orig_img_w_,
                                  (double)draw_h / orig_img_h_);
                {
                    struct timespec tv0, tv1;
                    clock_gettime(CLOCK_MONOTONIC, &tv0);
                    render_svg_cairo(vcr, svg_parsed_, orig_img_w_, orig_img_h_);
                    if (svg_embedded_.valid()) {
                        cairo_surface_t* es = cairo_image_surface_create_for_data(
                            svg_embedded_.bgra.data(), CAIRO_FORMAT_ARGB32,
                            svg_embedded_.img_w, svg_embedded_.img_h,
                            svg_embedded_.img_w * 4);
                        cairo_save(vcr);
                        cairo_translate(vcr, svg_embedded_.x, svg_embedded_.y);
                        cairo_scale(vcr,
                            (double)svg_embedded_.w / svg_embedded_.img_w,
                            (double)svg_embedded_.h / svg_embedded_.img_h);
                        cairo_set_source_surface(vcr, es, 0, 0);
                        cairo_paint(vcr);
                        cairo_surface_destroy(es);
                        cairo_restore(vcr);
                    }
                    clock_gettime(CLOCK_MONOTONIC, &tv1);
                    int64_t v_us = (tv1.tv_sec - tv0.tv_sec) * 1000000 +
                                   (tv1.tv_nsec - tv0.tv_nsec) / 1000;
                    std::cerr << "[svg] vector: " << v_us << " us ("
                              << draw_w << "x" << draw_h << ")\n";
                }
                cairo_destroy(vcr);
                if (svg_vector_cache_)
                    cairo_surface_destroy(svg_vector_cache_);
                svg_vector_cache_ = new_cv;
                svg_vector_w_ = draw_w;
                svg_vector_h_ = draw_h;
                svg_vector_pending_ = false;

                // Blit the new cache to the display
                cairo_set_source_surface(cr, svg_vector_cache_, 0, 0);
                cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_NEAREST);
                cairo_paint(cr);
            }

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

// --- Coordinate conversion (image ↔ window) ---

void App::img_to_win(int img_x, int img_y, int& win_x, int& win_y) const {
    if (decoded_image_.width <= 0 || decoded_image_.height <= 0) {
        win_x = img_x; win_y = img_y;
        return;
    }
    float img_w = (float)decoded_image_.width;
    float img_h = (float)decoded_image_.height;
    bool hide_strip = fullscreen_ || slideshow_;
    bool strip_visible = show_thumbnails_ && (!hide_strip || show_thumbnails_hover_);
    int strip_h = (strip_visible && decoded_image_.width > 0 && dir_images_.size() > 1)
                      ? ThumbnailStrip::kHeight : 0;
    int avail_h = window_height_ - (show_toolbar_ ? Overlay::kToolbarHeight : 0) - strip_h;
    int content_w = window_width_ - (show_sidebar_ ? 320 : 0);
    float fit_scale = std::min((float)content_w / img_w, (float)avail_h / img_h) * zoom_;
    int draw_w = std::max(1, (int)(img_w * fit_scale));
    int draw_h = std::max(1, (int)(img_h * fit_scale));
    int offset_x = (content_w - draw_w) / 2 + (int)pan_x_;
    int offset_y = (show_toolbar_ ? Overlay::kToolbarHeight : 0) + (avail_h - draw_h) / 2 + (int)pan_y_;
    float sx = (float)draw_w / img_w;
    float sy = (float)draw_h / img_h;
    win_x = (int)(img_x * sx) + offset_x;
    win_y = (int)(img_y * sy) + offset_y;
}

void App::win_to_img(int win_x, int win_y, int& img_x, int& img_y) const {
    if (decoded_image_.width <= 0 || decoded_image_.height <= 0) {
        img_x = win_x; img_y = win_y;
        return;
    }
    float img_w = (float)decoded_image_.width;
    float img_h = (float)decoded_image_.height;
    bool hide_strip = fullscreen_ || slideshow_;
    bool strip_visible = show_thumbnails_ && (!hide_strip || show_thumbnails_hover_);
    int strip_h = (strip_visible && decoded_image_.width > 0 && dir_images_.size() > 1)
                      ? ThumbnailStrip::kHeight : 0;
    int avail_h = window_height_ - (show_toolbar_ ? Overlay::kToolbarHeight : 0) - strip_h;
    int content_w = window_width_ - (show_sidebar_ ? 320 : 0);
    float fit_scale = std::min((float)content_w / img_w, (float)avail_h / img_h) * zoom_;
    int draw_w = std::max(1, (int)(img_w * fit_scale));
    int draw_h = std::max(1, (int)(img_h * fit_scale));
    int offset_x = (content_w - draw_w) / 2 + (int)pan_x_;
    int offset_y = (show_toolbar_ ? Overlay::kToolbarHeight : 0) + (avail_h - draw_h) / 2 + (int)pan_y_;
    float sx = (float)draw_w / img_w;
    float sy = (float)draw_h / img_h;
    img_x = (sx > 0) ? (int)((win_x - offset_x) / sx) : 0;
    img_y = (sy > 0) ? (int)((win_y - offset_y) / sy) : 0;
}

// --- Crop rectangle rendering ---

void App::draw_crop_rect(cairo_t* cr, int win_w, int win_h) {
    (void)win_w;
    (void)win_h;
    if (decoded_image_.width <= 0 || decoded_image_.height <= 0) return;

    int wx1, wy1, wx2, wy2;
    img_to_win(crop_x_, crop_y_, wx1, wy1);
    img_to_win(crop_x_ + crop_w_, crop_y_ + crop_h_, wx2, wy2);

    int rw = wx2 - wx1;
    int rh = wy2 - wy1;
    if (rw < 1 || rh < 1) return;

    // Clear the dim area by drawing a "window" through the dim overlay
    // using the path difference technique: draw the dim rect, then
    // subtract the crop rectangle.
    // Since we draw the dim overlay in render_crop_overlay first and
    // then draw the crop rect on top, we just draw the crop rect outline.

    // Crop rectangle outline — accent color
    cairo_set_source_rgba(cr, m3::primary_r, m3::primary_g, m3::primary_b, 0.9);
    cairo_set_line_width(cr, 2);
    overlay_.draw_rounded_rect(cr, wx1, wy1, rw, rh, 4);
    cairo_stroke(cr);

    // Lighter fill for the crop area
    cairo_set_source_rgba(cr, m3::primary_r, m3::primary_g, m3::primary_b, 0.08);
    overlay_.draw_rounded_rect(cr, wx1, wy1, rw, rh, 4);
    cairo_fill(cr);

    // Corner handles
    int hl = 10;
    auto draw_handle = [&](int hx, int hy) {
        cairo_rectangle(cr, hx - hl / 2, hy - hl / 2, hl, hl);
        cairo_fill(cr);
    };
    cairo_set_source_rgba(cr, 1, 1, 1, 0.9);
    draw_handle(wx1, wy1);
    draw_handle(wx2, wy1);
    draw_handle(wx1, wy2);
    draw_handle(wx2, wy2);

    // Center cross-hair
    cairo_set_source_rgba(cr, m3::primary_r, m3::primary_g, m3::primary_b, 0.5);
    cairo_set_line_width(cr, 1);
    cairo_move_to(cr, wx1 + rw / 2, wy1);
    cairo_line_to(cr, wx1 + rw / 2, wy2);
    cairo_stroke(cr);
    cairo_move_to(cr, wx1, wy1 + rh / 2);
    cairo_line_to(cr, wx2, wy1 + rh / 2);
    cairo_stroke(cr);

    // Size label
    char buf[64];
    snprintf(buf, sizeof(buf), "%d × %d", crop_w_, crop_h_);
    cairo_set_source_rgba(cr, 1, 1, 1, 0.9);
    cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL,
                           CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 13);
    cairo_move_to(cr, wx1 + 6, wy1 - 6);
    cairo_show_text(cr, buf);
}

void App::draw_markup_elements(cairo_t* cr) {
    if (markup_elements_.empty() && !markup_current_) return;

    auto set_color = [&](const MarkupElement& el) {
        cairo_set_source_rgba(cr,
            ((el.color >> 24) & 0xFF) / 255.0,
            ((el.color >> 16) & 0xFF) / 255.0,
            ((el.color >> 8) & 0xFF) / 255.0,
            ((el.color >> 0) & 0xFF) / 255.0);
        cairo_set_line_width(cr, el.thickness);
        cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
        cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    };

    auto draw_pen = [&](const MarkupElement& el) {
        if (el.points_x.size() < 2) return;
        cairo_move_to(cr, el.points_x[0], el.points_y[0]);
        for (size_t i = 1; i < el.points_x.size(); i++)
            cairo_line_to(cr, el.points_x[i], el.points_y[i]);
        cairo_stroke(cr);
    };

    auto draw_line = [&](const MarkupElement& el) {
        if (el.points_x.size() < 2) return;
        cairo_move_to(cr, el.points_x[0], el.points_y[0]);
        cairo_line_to(cr, el.points_x[1], el.points_y[1]);
        cairo_stroke(cr);
    };

    auto draw_arrow = [&](const MarkupElement& el) {
        if (el.points_x.size() < 2) return;
        float x1 = el.points_x[0], y1 = el.points_y[0];
        float x2 = el.points_x[1], y2 = el.points_y[1];
        float dx = x2 - x1, dy = y2 - y1;
        float len = std::sqrt(dx * dx + dy * dy);
        if (len < 1.0f) return;
        float ux = dx / len, uy = dy / len;
        // Shaft
        cairo_move_to(cr, x1, y1);
        cairo_line_to(cr, x2, y2);
        cairo_stroke(cr);
        // Arrowhead
        float head_len = std::max(10.0f, el.thickness * 3.0f);
        float head_angle = 0.45f;
        float ax1 = x2 - ux * head_len * std::cos(head_angle) + uy * head_len * std::sin(head_angle);
        float ay1 = y2 - uy * head_len * std::cos(head_angle) - ux * head_len * std::sin(head_angle);
        float ax2 = x2 - ux * head_len * std::cos(head_angle) - uy * head_len * std::sin(head_angle);
        float ay2 = y2 - uy * head_len * std::cos(head_angle) + ux * head_len * std::sin(head_angle);
        cairo_set_line_width(cr, std::max(1.5f, el.thickness * 0.5f));
        cairo_move_to(cr, x2, y2);
        cairo_line_to(cr, ax1, ay1);
        cairo_move_to(cr, x2, y2);
        cairo_line_to(cr, ax2, ay2);
        cairo_stroke(cr);
    };

    auto draw_rect = [&](const MarkupElement& el) {
        if (el.rect_w <= 0 || el.rect_h <= 0) return;
        cairo_rectangle(cr, el.rect_x, el.rect_y, el.rect_w, el.rect_h);
        cairo_stroke(cr);
    };

    auto draw_ellipse = [&](const MarkupElement& el) {
        if (el.rect_w <= 0 || el.rect_h <= 0) return;
        float cx = el.rect_x + el.rect_w * 0.5f;
        float cy = el.rect_y + el.rect_h * 0.5f;
        float rx = el.rect_w * 0.5f;
        float ry = el.rect_h * 0.5f;
        cairo_save(cr);
        cairo_translate(cr, cx, cy);
        cairo_scale(cr, rx, ry);
        cairo_arc(cr, 0, 0, 1, 0, 2 * M_PI);
        cairo_restore(cr);
        cairo_stroke(cr);
    };

    auto draw_numbered = [&](const MarkupElement& el) {
        if (el.points_x.size() < 1) return;
        float cx = el.points_x[0], cy = el.points_y[0];
        float r = std::max(12.0f, el.thickness * 3.0f);
        cairo_arc(cr, cx, cy, r, 0, 2 * M_PI);
        cairo_fill(cr);
        // White border
        cairo_set_source_rgba(cr, 1, 1, 1, 0.9);
        cairo_set_line_width(cr, 1.5);
        cairo_arc(cr, cx, cy, r, 0, 2 * M_PI);
        cairo_stroke(cr);
        // Number text
        if (!el.text.empty()) {
            cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL,
                                   CAIRO_FONT_WEIGHT_BOLD);
            cairo_set_font_size(cr, r * 0.9f);
            cairo_text_extents_t te;
            cairo_text_extents(cr, el.text.c_str(), &te);
            cairo_move_to(cr, cx - te.x_advance * 0.5f, cy + te.height * 0.35f);
            cairo_set_source_rgba(cr, 1, 1, 1, 0.9);
            cairo_show_text(cr, el.text.c_str());
        }
    };

    auto draw_one = [&](const MarkupElement& el) {
        set_color(el);
        switch (el.type) {
        case MarkupTool::kPen:   draw_pen(el); break;
        case MarkupTool::kLine:  draw_line(el); break;
        case MarkupTool::kArrow: draw_arrow(el); break;
        case MarkupTool::kRect:  draw_rect(el); break;
        case MarkupTool::kEllipse: draw_ellipse(el); break;
        case MarkupTool::kNumbered: draw_numbered(el); break;
        default: draw_pen(el); break;
        }
    };

    for (const auto& el : markup_elements_) draw_one(el);
    if (markup_current_) draw_one(*markup_current_);
}

// --- Crop methods ---

void App::toggle_crop() {
    if (crop_active_) {
        cancel_crop();
        return;
    }
    if (decoded_image_.width <= 0) return;
    // Cancel markup mode if active
    if (markup_active_) cancel_markup();
    // Initialize crop rectangle to 90% of image, centered
    crop_x_ = decoded_image_.width / 20;
    crop_y_ = decoded_image_.height / 20;
    crop_w_ = decoded_image_.width * 9 / 10;
    crop_h_ = decoded_image_.height * 9 / 10;
    crop_active_ = true;
    render();
}

void App::apply_crop() {
    if (!crop_active_ || decoded_image_.width <= 0 || !svg_source_data_.empty()) return;
    if (crop_w_ <= 0 || crop_h_ <= 0) { cancel_crop(); return; }

    int x = std::max(0, std::min(crop_x_, decoded_image_.width - 1));
    int y = std::max(0, std::min(crop_y_, decoded_image_.height - 1));
    int w = std::min(crop_w_, decoded_image_.width - x);
    int h = std::min(crop_h_, decoded_image_.height - y);
    if (w <= 0 || h <= 0) { cancel_crop(); return; }

    // Extract the cropped region from the RGBA data
    std::vector<uint8_t> cropped((size_t)w * h * 4);
    int src_stride = decoded_image_.width * 4;
    for (int row = 0; row < h; row++) {
        int sy = y + row;
        memcpy(cropped.data() + (size_t)row * w * 4,
               decoded_image_.rgba.data() + (size_t)sy * src_stride + (size_t)x * 4,
               (size_t)w * 4);
    }

    decoded_image_.rgba = std::move(cropped);
    decoded_image_.width = w;
    decoded_image_.height = h;
    decoded_image_.stride = w * 4;

    // Rebuild BGRA cache
    size_t npix = (size_t)w * h;
    bgra_cache_.resize(npix * 4);
    for (size_t i = 0; i < npix; i++) {
        bgra_cache_[i * 4 + 0] = decoded_image_.rgba[i * 4 + 2];
        bgra_cache_[i * 4 + 1] = decoded_image_.rgba[i * 4 + 1];
        bgra_cache_[i * 4 + 2] = decoded_image_.rgba[i * 4 + 0];
        bgra_cache_[i * 4 + 3] = decoded_image_.rgba[i * 4 + 3];
    }

    crop_active_ = false;
    image_modified_ = true;
    update_title();
    render();
}

void App::cancel_crop() {
    crop_active_ = false;
    crop_dragging_ = false;
    render();
}

// --- Markup ---

void App::toggle_markup() {
    if (markup_active_) {
        cancel_markup();
        return;
    }
    if (decoded_image_.width <= 0) return;
    if (crop_active_) cancel_crop();
    markup_active_ = true;
    markup_tool_ = MarkupTool::kPen;
    markup_color_ = 0xFF0000FF;
    markup_thickness_ = 3.0f;
    numbered_count_ = 0;
    markup_current_.reset();
    render();
}

void App::commit_markup() {
    if (!markup_active_) return;
    if (markup_current_) {
        markup_elements_.push_back(std::move(*markup_current_));
        markup_current_.reset();
    }
    markup_active_ = false;
    image_modified_ = true;
    render();
}

void App::cancel_markup() {
    markup_active_ = false;
    markup_current_.reset();
    render();
}

void App::undo_markup() {
    if (!markup_elements_.empty()) {
        markup_redo_stack_.push_back(std::move(markup_elements_.back()));
        markup_elements_.pop_back();
        render();
    }
}

// --- Menu ---

void App::toggle_menu() {
    show_menu_ = !show_menu_;
    if (show_menu_) {
        show_settings_ = false;
    }
    render();
}

// --- Save helpers ---

void App::write_png_file(const std::string& path) {
    if (decoded_image_.rgba.empty()) return;
    // decoded_image_ is RGBA. stbi_write_png expects RGB or RGBA.
    stbi_write_png(path.c_str(),
                   decoded_image_.width,
                   decoded_image_.height,
                   4, // RGBA
                   decoded_image_.rgba.data(),
                   decoded_image_.width * 4);
    std::cout << "Saved: " << path << " ("
              << decoded_image_.width << "x" << decoded_image_.height << ")\n";
}

void App::save_image() {
    if (!image_modified_ || current_path_.empty()) return;
    write_png_file(current_path_);
    image_modified_ = false;
    update_title();
}

void App::save_as() {
    save_dialog_(false);
}

void App::save_as_copy() {
    save_dialog_(true);
}

void App::save_dialog_(bool as_copy) {
    if (decoded_image_.rgba.empty()) return;

    std::string parent_handle = "wayland:wl_surface@";
    uint32_t surface_id = wl_proxy_get_id((struct wl_proxy*)surface_);
    parent_handle += std::to_string(surface_id);

    fs::path current(current_path_);
    std::string suggested = current.empty() ? "image.png" : current.filename().string();
    std::string folder;
    if (!current.empty()) {
        folder = "file://" + current.parent_path().string();
    }

    portal_dialog_.save_file(parent_handle, suggested, folder,
        [this, as_copy](const std::string& path) {
            if (path.empty()) return;
            write_png_file(path);
            image_modified_ = false;
            if (!as_copy) {
                // Save As — make this the new current file
                open_file(path);
            } else {
                // Save As Copy — keep current file, just refresh
                render();
            }
        });
}

// --- File dialog ---

void App::open_file_dialog() {
    std::string parent_handle = "wayland:wl_surface@";
    uint32_t surface_id = wl_proxy_get_id((struct wl_proxy*)surface_);
    parent_handle += std::to_string(surface_id);

    portal_dialog_.open_file(parent_handle, [this](const std::string& path) {
        on_file_dialog_result(path);
    });
}

void App::on_file_dialog_result(const std::string& path) {
    if (path.empty()) return;
    open_file(path);
}

// --- Image navigation ---

void App::open_file(const std::string& path) {
    fs::path fp(path);
    if (!fs::exists(fp)) return;

    // If the path is a directory, open it instead
    if (fs::is_directory(fp)) {
        open_directory(fp.string());
        return;
    }

    current_path_ = path;
    decoded_image_ = DecodedImage{};
    bgra_cache_.clear();
    dragging_ = false;
    zoom_ = config_.default_zoom;
    pan_x_ = 0.0f;
    pan_y_ = 0.0f;
    update_title();

    auto dir = fp.parent_path().string();
    load_directory(dir);
    // load_image runs after load_directory so the thumbnail cache is set up
    // for the correct dir_image_index_ — otherwise load_directory would
    // clear the thumbnail that load_image just generated.
    load_image(path);
}

void App::open_directory(const std::string& path) {
    load_directory(path);
    if (!dir_images_.empty()) {
        open_file(dir_images_[0]);
    }
}

void App::load_directory(const std::string& dir) {
    current_dir_ = dir;
    dir_images_ = image_files_in_dir(dir);
    dir_image_index_ = -1;
    thumb_cache_.clear();
    thumb_cache_w_.clear();
    thumb_cache_h_.clear();
    thumb_scroll_ = 0;

    auto it = std::find(dir_images_.begin(), dir_images_.end(), current_path_);
    if (it != dir_images_.end()) {
        dir_image_index_ = (int)std::distance(dir_images_.begin(), it);
    }

    // Queue all thumbnails for background preloading, ordered by distance
    // from the current image so nearest ones appear first.
    thumb_pending_.clear();
    int n = (int)dir_images_.size();
    for (int d = 1; d < n; d++) {
        for (int sign : {-1, 1}) {
            int idx = dir_image_index_ + sign * d;
            if (idx >= 0 && idx < n && idx != dir_image_index_) {
                thumb_pending_.push_back(idx);
            }
        }
    }
    // Ensure current image is cached first (it will be by load_image)
    if (dir_image_index_ >= 0) {
        thumb_cache_[dir_image_index_] = {};
        thumb_cache_w_[dir_image_index_] = 0;
        thumb_cache_h_[dir_image_index_] = 0;
    }
    invalidate_thumb_strip();
}

void App::next_image() {
    if (dir_images_.empty()) return;
    dir_image_index_ = (dir_image_index_ + 1) % (int)dir_images_.size();
    open_file(dir_images_[dir_image_index_]);
}

void App::prev_image() {
    if (dir_images_.empty()) return;
    dir_image_index_ = (dir_image_index_ - 1 + (int)dir_images_.size()) % (int)dir_images_.size();
    open_file(dir_images_[dir_image_index_]);
}

void App::first_image() {
    if (dir_images_.empty()) return;
    dir_image_index_ = 0;
    open_file(dir_images_[0]);
}

void App::last_image() {
    if (dir_images_.empty()) return;
    dir_image_index_ = (int)dir_images_.size() - 1;
    open_file(dir_images_[dir_image_index_]);
}

void App::delete_image() {
    if (current_path_.empty()) return;

    // Advance to next/previous before trashing
    int next_idx = -1;
    if (!dir_images_.empty()) {
        // If this image is in the directory listing, find its index
        auto it = std::find(dir_images_.begin(), dir_images_.end(), current_path_);
        if (it != dir_images_.end()) {
            int idx = (int)(it - dir_images_.begin());
            if (idx + 1 < (int)dir_images_.size())
                next_idx = idx + 1;
            else if (idx > 0)
                next_idx = idx - 1;
        }
    }

    if (!trash_file(current_path_)) return;

    // Remove from current directory listing
    if (!dir_images_.empty()) {
        auto it = std::find(dir_images_.begin(), dir_images_.end(), current_path_);
        if (it != dir_images_.end()) {
            dir_images_.erase(it);
        }
        if (!dir_images_.empty()) {
            // Navigate to next available image
            if (next_idx >= 0 && next_idx < (int)dir_images_.size()) {
                dir_image_index_ = next_idx;
                open_file(dir_images_[dir_image_index_]);
                return;
            }
            // Fallback: first image or last
            dir_image_index_ = 0;
            open_file(dir_images_[0]);
            return;
        }
    }

    // No more images — clear state
    current_path_.clear();
    decoded_image_ = {};
    current_image_ = {};
    bgra_cache_.clear();
    exif_info_ = {};
    render();
}

void App::load_image(const std::string& path) {
    DecodeResult result;
    std::vector<uint8_t> file_buf;

    // Check prefetch pool first
    auto prefetched = decode_pool_.try_claim(path);
    if (prefetched.has_value()) {
        result = std::move(*prefetched);
        std::cout << "Prefetch hit: ";
    } else {
        FILE* f = fopen(path.c_str(), "rb");
        if (!f) {
            std::cerr << "Cannot open: " << path << "\n";
            return;
        }
        fseek(f, 0, SEEK_END);
        size_t size = ftell(f);
        fseek(f, 0, SEEK_SET);
        file_buf.resize(size);
        if (fread(file_buf.data(), 1, size, f) != size) {
            std::cerr << "Failed to read: " << path << "\n";
            fclose(f);
            return;
        }
        fclose(f);

        result = decoders_.decode(file_buf.data(), file_buf.size(),
                                   window_width_, window_height_);
        if (result.pixels.empty()) {
            std::cerr << "Failed to decode: " << path << "\n";
            return;
        }
    }

    current_path_ = path;

#ifdef HAVE_LCMS2
    if (!result.icc_profile.empty()) {
        const auto& disp = conn_.display_icc_profile();
        if (!disp.empty() && config_.enable_color_management) {
            apply_color_management(result, disp);
        } else {
            apply_color_management(result);
        }
    }
#endif

#ifdef HAVE_METADATA
    if (!file_buf.empty()) {
        exif_info_ = parse_exif(file_buf.data(), file_buf.size());
    } else {
        exif_info_ = ExifInfo{};
    }
#else
    exif_info_ = ExifInfo{};
#endif

    // File-level metadata
    {
        struct stat st;
        if (stat(path.c_str(), &st) == 0) {
            exif_info_.file_size = (uint64_t)st.st_size;
            char time_buf[32];
            struct tm* tm = localtime(&st.st_mtime);
            if (tm) {
                strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", tm);
                exif_info_.file_modified = time_buf;
            }
        }
    }

    std::cout << "Loaded: " << path << " (" << result.width << "x" << result.height
              << ", " << result.format_name << ")\n";

    // Clean up any previous SVG state
    if (svg_parsed_) { nsvgDelete(svg_parsed_); svg_parsed_ = nullptr; }
    svg_embedded_ = EmbeddedImage{};
    svg_source_data_.clear();
    if (svg_vector_cache_) { cairo_surface_destroy(svg_vector_cache_); svg_vector_cache_ = nullptr; }
    svg_vector_w_ = 0; svg_vector_h_ = 0; svg_vector_pending_ = false;
    orig_img_w_ = (float)result.width;
    orig_img_h_ = (float)result.height;
    if (result.format_name == "SVG") {
        if (!file_buf.empty()) {
            svg_source_data_ = file_buf;
        } else {
            // Prefetch hit — re-read the file for source data
            FILE* f = fopen(path.c_str(), "rb");
            if (f) {
                fseek(f, 0, SEEK_END);
                svg_source_data_.resize(ftell(f));
                fseek(f, 0, SEEK_SET);
                if (fread(svg_source_data_.data(), 1, svg_source_data_.size(), f) != svg_source_data_.size())
                    svg_source_data_.clear();
                fclose(f);
            }
        }
        // Parse and cache SVG for vector rendering
        if (!svg_source_data_.empty()) {
            struct timespec tp0, tp1;
            clock_gettime(CLOCK_MONOTONIC, &tp0);
            svg_parsed_ = svg_parse(svg_source_data_.data(), svg_source_data_.size());
            clock_gettime(CLOCK_MONOTONIC, &tp1);
            int64_t parse_us = (tp1.tv_sec - tp0.tv_sec) * 1000000 +
                               (tp1.tv_nsec - tp0.tv_nsec) / 1000;
            if (svg_parsed_) {
                if (svg_parsed_->width > 0 && svg_parsed_->height > 0) {
                    orig_img_w_ = svg_parsed_->width;
                    orig_img_h_ = svg_parsed_->height;
                    result.width = (int)svg_parsed_->width;
                    result.height = (int)svg_parsed_->height;
                }
                // Extract the first embedded PNG image, if any
                svg_embedded_ = extract_embedded_image(
                    svg_source_data_.data(), svg_source_data_.size());
                std::cerr << "[svg] embedded: "
                          << (svg_embedded_.valid()
                              ? std::to_string(svg_embedded_.img_w) + "x"
                                + std::to_string(svg_embedded_.img_h) + " px"
                              : "none")
                          << "\n";
            }
            result.pixels = std::vector<uint8_t>(4, 0);
            std::cerr << "[svg] parse: " << parse_us << " us, viewbox "
                      << orig_img_w_ << "x" << orig_img_h_ << "\n";
        }
    }

    current_image_ = DecodedImage{
        .rgba = std::move(result.pixels),
        .width = result.width,
        .height = result.height,
        .stride = result.width * 4,
    };
    decoded_image_ = current_image_;

    // Rebuild BGRA cache (skipped for SVGs — rendered as vectors)
    if (svg_source_data_.empty()) {
        size_t npix = (size_t)decoded_image_.width * decoded_image_.height;
        bgra_cache_.resize(npix * 4);
        for (size_t i = 0; i < npix; i++) {
            bgra_cache_[i * 4 + 0] = decoded_image_.rgba[i * 4 + 2];
            bgra_cache_[i * 4 + 1] = decoded_image_.rgba[i * 4 + 1];
            bgra_cache_[i * 4 + 2] = decoded_image_.rgba[i * 4 + 0];
            bgra_cache_[i * 4 + 3] = decoded_image_.rgba[i * 4 + 3];
        }
    }

    // Cache thumbnail for the current image
    if (dir_image_index_ >= 0 && svg_source_data_.empty()) {
        gen_thumb_bgra(decoded_image_.rgba, decoded_image_.width, decoded_image_.height,
                       thumb_cache_[dir_image_index_],
                       thumb_cache_w_[dir_image_index_],
                       thumb_cache_h_[dir_image_index_]);
    }
    invalidate_thumb_strip();

    // Clear markup and crop state for new image
    markup_elements_.clear();
    markup_current_.reset();
    markup_drawing_ = false;
    markup_active_ = false;
    crop_active_ = false;
    crop_dragging_ = false;

    render();

    // Prefetch the next image
    if (dir_image_index_ >= 0 && dir_images_.size() > 1) {
        int next = (dir_image_index_ + 1) % (int)dir_images_.size();
        decode_pool_.prefetch(dir_images_[next], decoders_);
    }
}

void App::update_title() {
    if (!xdg_toplevel_) return;
    fs::path p(current_path_);
    auto title = p.filename().string() + " - Horizon Photo Viewer";
    xdg_toplevel_set_title(xdg_toplevel_, title.c_str());
}

void App::gen_thumb_bgra(const std::vector<uint8_t>& rgba, int w, int h,
                         std::vector<uint8_t>& out, int& out_w, int& out_h) {
    int tw = 80, th = 60;
    float scale = std::min((float)tw / w, (float)th / h);
    out_w = std::max(1, (int)(w * scale));
    out_h = std::max(1, (int)(h * scale));
    out.resize((size_t)out_w * out_h * 4);

    int src_step = w * 4;
    for (int ty = 0; ty < out_h; ty++) {
        int sy = std::min((int)(ty / scale), h - 1);
        for (int tx = 0; tx < out_w; tx++) {
            int sx = std::min((int)(tx / scale), w - 1);
            size_t si = (size_t)sy * src_step + (size_t)sx * 4;
            size_t ti = (size_t)ty * out_w * 4 + (size_t)tx * 4;
            out[ti + 0] = rgba[si + 2];
            out[ti + 1] = rgba[si + 1];
            out[ti + 2] = rgba[si + 0];
            out[ti + 3] = rgba[si + 3];
        }
    }
}

void App::invalidate_thumb_strip() {
    thumb_dirty_ = true;
}

void App::destroy_cached_strip() {
    if (cached_strip_) {
        cairo_surface_destroy(cached_strip_);
        cached_strip_ = nullptr;
    }
    cached_strip_w_ = 0;
}

void App::process_thumb_batch(int count) {
    if (!show_thumbnails_) return;
    bool any = false;
    int n = std::min(count, (int)thumb_pending_.size());
    for (int i = 0; i < n; i++) {
        int idx = thumb_pending_[i];
        if (thumb_cache_.count(idx) == 0 || thumb_cache_[idx].empty()) {
            load_thumbnail(idx);
            any = true;
        }
    }
    thumb_pending_.erase(thumb_pending_.begin(), thumb_pending_.begin() + n);
    if (any) {
        invalidate_thumb_strip();
        render();
    }
}

void App::queue_thumb_preload(int from, int to) {
    if (from < 0) from = 0;
    if (to > (int)dir_images_.size()) to = (int)dir_images_.size();
    for (int i = from; i < to; i++) {
        if (thumb_cache_.count(i) == 0) {
            // Check not already in pending
            if (std::find(thumb_pending_.begin(), thumb_pending_.end(), i) == thumb_pending_.end()) {
                thumb_pending_.push_back(i);
            }
        }
    }
}

// --- Drag-and-drop ---

namespace {

void data_offer_handle_offer(void* data, wl_data_offer* /*offer*/, const char* mime_type) {
    auto* mime_types = static_cast<std::vector<std::string>*>(data);
    mime_types->emplace_back(mime_type);
}

void data_offer_handle_source_actions(void* /*data*/, wl_data_offer* /*offer*/,
                                       uint32_t /*source_actions*/) {}

void data_offer_handle_action(void* /*data*/, wl_data_offer* /*offer*/,
                               uint32_t /*dnd_action*/) {}

constexpr wl_data_offer_listener data_offer_listener_ = {
    .offer = data_offer_handle_offer,
    .source_actions = data_offer_handle_source_actions,
    .action = data_offer_handle_action,
};

}

void App::handle_data_offer(void* data, wl_data_device* /*device*/, wl_data_offer* offer) {
    auto& self = *static_cast<App*>(data);
    self.drag_offer_ = offer;
    self.drag_mime_types_.clear();
    wl_data_offer_add_listener(offer, &data_offer_listener_, &self.drag_mime_types_);
}

void App::handle_data_enter(void* data, wl_data_device* /*device*/, uint32_t serial,
                             wl_surface* /*surface*/, wl_fixed_t /*x*/, wl_fixed_t /*y*/,
                             wl_data_offer* /*offer*/) {
    auto& self = *static_cast<App*>(data);
    bool has_uris = false;
    for (auto& mt : self.drag_mime_types_) {
        if (mt == "text/uri-list") { has_uris = true; break; }
    }
    if (has_uris && self.drag_offer_) {
        wl_data_offer_accept(self.drag_offer_, serial, "text/uri-list");
    }
}

void App::handle_data_leave(void* /*data*/, wl_data_device* /*device*/) {}

void App::handle_data_motion(void* data, wl_data_device* /*device*/,
                              uint32_t /*time*/, wl_fixed_t /*x*/, wl_fixed_t /*y*/) {
    auto& self = *static_cast<App*>(data);
    if (self.drag_offer_) {
        wl_data_offer_set_actions(self.drag_offer_,
                                  WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY,
                                  WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY);
    }
}

void App::handle_data_drop(void* data, wl_data_device* /*device*/) {
    auto& self = *static_cast<App*>(data);
    if (!self.drag_offer_) return;
    // Accept the drop
    int fds[2];
    if (pipe(fds) < 0) return;
    wl_data_offer_receive(self.drag_offer_, "text/uri-list", fds[1]);
    close(fds[1]);
    wl_data_offer_finish(self.drag_offer_);
    wl_data_offer_destroy(self.drag_offer_);
    self.drag_offer_ = nullptr;
    wl_display_flush(self.conn_.display());

    // Read URIs from the pipe
    char buf[4096];
    ssize_t n = read(fds[0], buf, sizeof(buf) - 1);
    close(fds[0]);
    if (n <= 0) return;
    buf[n] = '\0';
    // Parse first file:// URI
    std::string uri;
    char* p = buf;
    while (*p == '\r' || *p == '\n') p++;
    if (std::string_view(p).starts_with("file://")) {
        p += 7;
        while (*p && *p != '\r' && *p != '\n') uri += *p++;
        // URL-decode
        std::string path;
        for (size_t i = 0; i < uri.size(); i++) {
            if (uri[i] == '%' && i + 2 < uri.size()) {
                char hex[3] = {uri[i+1], uri[i+2], '\0'};
                path += (char)std::strtoul(hex, nullptr, 16);
                i += 2;
            } else if (uri[i] == '+') {
                path += ' ';
            } else {
                path += uri[i];
            }
        }
        if (!path.empty()) self.open_file(path);
    }
}

void App::handle_data_selection(void* /*data*/, wl_data_device* /*device*/,
                                 wl_data_offer* /*offer*/) {}

void App::on_data_drop(uint32_t /*serial*/, wl_data_offer* /*offer*/) {}

// --- Thumbnail cache ---

void App::load_thumbnail(int index) {
    if (index < 0 || index >= (int)dir_images_.size()) return;
    if (thumb_cache_.count(index)) return; // already cached in memory

    const auto& filepath = dir_images_[index];

    // Try disk cache first
    {
        std::vector<uint8_t> rgba;
        int w = 0, h = 0;
        if (ThumbCache::load(filepath, rgba, w, h)) {
            // Convert RGBA → BGRA for Cairo
            thumb_cache_w_[index] = w;
            thumb_cache_h_[index] = h;
            thumb_cache_[index].resize(rgba.size());
            auto& bg = thumb_cache_[index];
            for (int i = 0; i < w * h; i++) {
                bg[i * 4 + 0] = rgba[i * 4 + 2];
                bg[i * 4 + 1] = rgba[i * 4 + 1];
                bg[i * 4 + 2] = rgba[i * 4 + 0];
                bg[i * 4 + 3] = rgba[i * 4 + 3];
            }
            return;
        }
    }

    // Mark as attempted so we don't retry failed decodes
    thumb_cache_[index] = {};
    thumb_cache_w_[index] = 0;
    thumb_cache_h_[index] = 0;

    FILE* f = fopen(filepath.c_str(), "rb");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    size_t size = ftell(f);
    fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> buf(size);
    if (fread(buf.data(), 1, size, f) != size) {
        fclose(f);
        return;
    }
    fclose(f);

    auto result = decoders_.decode(buf.data(), size);
    if (result.pixels.empty()) return;

    gen_thumb_bgra(result.pixels, result.width, result.height,
                   thumb_cache_[index], thumb_cache_w_[index], thumb_cache_h_[index]);

    // Save thumbnail to disk cache (convert BGRA → RGBA for canonical storage)
    {
        auto& tb = thumb_cache_[index];
        int tw = thumb_cache_w_[index];
        int th = thumb_cache_h_[index];
        std::vector<uint8_t> rgba(tb.size());
        for (int i = 0; i < tw * th; i++) {
            rgba[i * 4 + 0] = tb[i * 4 + 2];
            rgba[i * 4 + 1] = tb[i * 4 + 1];
            rgba[i * 4 + 2] = tb[i * 4 + 0];
            rgba[i * 4 + 3] = tb[i * 4 + 3];
        }
        ThumbCache::save(filepath, rgba.data(), tw, th);
    }
}

// --- View state ---

void App::zoom_in() {
    zoom_ *= 1.25f;
    render();
}

void App::zoom_out() {
    zoom_ /= 1.25f;
    if (zoom_ < 0.1f) zoom_ = 0.1f;
    render();
}

void App::zoom_fit() {
    zoom_ = 1.0f;
    pan_x_ = 0.0f;
    pan_y_ = 0.0f;
    render();
}

void App::zoom_1to1() {
    if (decoded_image_.width == 0) return;
    float fit_scale = std::min(
        (float)window_width_ / (float)decoded_image_.width,
        (float)window_height_ / (float)decoded_image_.height
    );
    if (fit_scale > 0.0f) zoom_ = 1.0f / fit_scale;
    render();
}

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

std::vector<std::string> App::image_files_in_dir(const std::string& dir) {
    std::vector<std::string> result;
    if (!fs::is_directory(dir)) return result;

    for (const auto& entry : fs::directory_iterator(dir)) {
        if (!entry.is_regular_file()) continue;
        auto ext = entry.path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(),
            [](unsigned char c) { return std::tolower(c); });
        if (ext == ".jpg" || ext == ".jpeg" || ext == ".png" ||
            ext == ".gif" || ext == ".bmp" || ext == ".webp" ||
            ext == ".tiff" || ext == ".tif" || ext == ".heic" ||
            ext == ".heif" || ext == ".avif" || ext == ".jxl" ||
            ext == ".svg") {
            result.push_back(entry.path().string());
        }
    }
    std::sort(result.begin(), result.end());
    return result;
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
