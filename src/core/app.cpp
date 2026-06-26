#include "core/app.hpp"

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
#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>

#include <cairo.h>

namespace hpv {
namespace fs = std::filesystem;
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
            if (overlay_.init_icons(*ip)) {
                icons_loaded = true;
                break;
            }
        }
        if (!icons_loaded)
            std::cerr << "overlay: MaterialSymbolsRounded.ttf not found\n";
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

    // --- Background (use SOURCE to overwrite stale buffer content) ---
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgba(cr, 0.12, 0.12, 0.14, bg_alpha_);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

    // --- Content area width (shrunk when sidebar is open) ---
    int content_w = win_w;
    int sidebar_w = 0;
    if (show_sidebar_) {
        sidebar_w = 320;
        content_w = win_w - sidebar_w;
    }

    // --- Sidebar area background (painted before the image) ---
    if (show_sidebar_) {
        cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
        cairo_set_source_rgba(cr, 0.14, 0.14, 0.16, bg_alpha_);
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
        float img_w = (float)decoded_image_.width;
        float img_h = (float)decoded_image_.height;
        int avail_h = win_h - (show_toolbar_ ? Overlay::kToolbarHeight : 0);
        float fit_scale = std::min((float)content_w / img_w, (float)avail_h / img_h) * zoom_;
        int draw_w = std::max(1, (int)(img_w * fit_scale));
        int draw_h = std::max(1, (int)(img_h * fit_scale));

        cairo_surface_t* img_surf = cairo_image_surface_create_for_data(
            bgra_cache_.data(),
            CAIRO_FORMAT_ARGB32,
            decoded_image_.width, decoded_image_.height,
            decoded_image_.width * 4
        );

        int offset_x = (content_w - draw_w) / 2 + (int)pan_x_;
        int offset_y = (show_toolbar_ ? Overlay::kToolbarHeight : 0) + (avail_h - draw_h) / 2 + (int)pan_y_;

        cairo_save(cr);
        cairo_translate(cr, offset_x, offset_y);
        cairo_scale(cr, (double)draw_w / decoded_image_.width,
                        (double)draw_h / decoded_image_.height);
        cairo_set_source_surface(cr, img_surf, 0, 0);
        cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_NEAREST);
        cairo_paint(cr);
        cairo_restore(cr);

        cairo_surface_destroy(img_surf);
    }

    // --- Overlay (toolbar only — info/placeholder drawn after thumbnail strip) ---
    OverlayState ov_state;
    ov_state.show_info = show_overlay_;
    ov_state.toolbar_visible = show_toolbar_;
    ov_state.fullscreen = fullscreen_;
    ov_state.slideshow = slideshow_;
    ov_state.show_settings = show_settings_;
    ov_state.show_sidebar = show_sidebar_;
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
        overlay_.render_toolbar(cr, win_w, win_h, toolbar_buttons_, bg_alpha_);
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
        }
    }

    // --- Thumbnail strip (constrained to content area) ---
    bool hide_strip = fullscreen_ || slideshow_;
    bool strip_visible = show_thumbnails_ && (!hide_strip || show_thumbnails_hover_);
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
                int strip_y_win = win_h - ThumbnailStrip::kHeight;
                for (auto& e : thumb_entries_) e.y += strip_y_win;
            }
            thumb_dirty_ = false;
        }
        // Blit cached strip
        cairo_set_source_surface(cr, cached_strip_, 0, win_h - ThumbnailStrip::kHeight);
        cairo_paint(cr);
    }

    // --- Overlay info / placeholder (drawn on top of thumbnail strip) ---
    overlay_.render_overlay(cr, win_w, win_h, ov_state);

    // --- Settings popup (drawn on top of everything) ---
    if (show_settings_) {
        std::vector<OverlayButton> settings_buttons;
        overlay_.render_settings_popup(cr, win_w, win_h, ov_state, settings_buttons);
        for (auto& btn : settings_buttons) {
            if (btn.label == "CloseSettings") {
                btn.action = [this]() { toggle_settings(); };
            } else if (btn.label == "bg_alpha") {
                btn.action = [this]() { /* handled in on_pointer */ };
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
        case XKB_KEY_o:
            if (ev.ctrl) open_file_dialog();
            break;
        case XKB_KEY_f:
        case XKB_KEY_F11:
            toggle_fullscreen();
            break;
        case XKB_KEY_Escape:
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
            bool hit_popup = false;
            // Settings popup bounds (matching render_settings_popup layout)
            int pw = 360, ph = 280;
            int px = (window_width_ - pw) / 2;
            int py = (window_height_ - ph) / 2;
            // Check if click is inside popup
            if (ev.x >= px && ev.x < px + pw && ev.y >= py && ev.y < py + ph) {
                hit_popup = true;
                // Hit-test settings buttons
                for (auto& btn : toolbar_buttons_) {
                    if (ev.x >= btn.x && ev.x < btn.x + btn.w &&
                        ev.y >= btn.y && ev.y < btn.y + btn.h) {
                        if (btn.label == "CloseSettings") {
                            toggle_settings();
                            return;
                        }
                        if (btn.label == "bg_alpha") {
                            // Calculate alpha from slider position
                            float a = (float)(ev.x - btn.x) / (float)btn.w;
                            set_bg_alpha(a);
                            return;
                        }
                    }
                }
            }
            // Click outside popup closes it; click inside but not on a button = consume
            if (!hit_popup) {
                toggle_settings();
                return;
            }
            return; // consume click inside popup even if not on a button
        }

        // Hit-test toolbar buttons (skip when hidden in fullscreen)
        if (show_toolbar_) {
            for (auto& btn : toolbar_buttons_) {
                if (ev.x >= btn.x && ev.x < btn.x + btn.w &&
                    ev.y >= btn.y && ev.y < btn.y + btn.h && btn.action) {
                    btn.action();
                    return;
                }
            }
        }

        // Hit-test thumbnail strip
        int strip_y = window_height_ - ThumbnailStrip::kHeight;
        bool strip_visible = show_thumbnails_ && (!(fullscreen_ || slideshow_) || show_thumbnails_hover_);
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
        if (dragging_) {
            dragging_ = false;
        }
    }
}

void App::on_motion(int x, int y) {
    pointer_x_ = x;
    pointer_y_ = y;

    if (fullscreen_ || slideshow_) {
        // Toolbar hover at top
        bool in_zone = y < Overlay::kToolbarHoverZone;
        if (in_zone != show_toolbar_) {
            show_toolbar_ = in_zone;
            pending_redraw_ = true;
            return;
        }
        if (show_toolbar_) return;

        // Thumbnail strip hover at bottom
        bool in_strip_zone = y >= window_height_ - ThumbnailStrip::kHeight;
        if (in_strip_zone != show_thumbnails_hover_) {
            show_thumbnails_hover_ = in_strip_zone;
            pending_redraw_ = true;
            return;
        }
        if (show_thumbnails_hover_) return;
    }

    if (decoded_image_.width <= 0 || !dragging_) return;

    pan_x_ = pan_start_x_ + (float)(x - drag_start_x_);
    pan_y_ = pan_start_y_ + (float)(y - drag_start_y_);
    pending_redraw_ = true;
}

void App::on_scroll(const ScrollEvent& ev) {
    // Thumbnail strip horizontal scroll
    bool strip_active = show_thumbnails_ && (!(fullscreen_ || slideshow_) || show_thumbnails_hover_);
    if (strip_active && !dir_images_.empty() &&
        pointer_y_ >= window_height_ - ThumbnailStrip::kHeight) {
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
    zoom_ = 1.0f;
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

        result = decoders_.decode(file_buf.data(), file_buf.size());
        if (result.pixels.empty()) {
            std::cerr << "Failed to decode: " << path << "\n";
            return;
        }
    }

    current_path_ = path;

#ifdef HAVE_LCMS2
    if (!result.icc_profile.empty()) {
        apply_color_management(result);
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

    current_image_ = DecodedImage{
        .rgba = std::move(result.pixels),
        .width = result.width,
        .height = result.height,
        .stride = result.width * 4,
    };
    decoded_image_ = current_image_;

    // Rebuild BGRA cache
    size_t npix = (size_t)decoded_image_.width * decoded_image_.height;
    bgra_cache_.resize(npix * 4);
    for (size_t i = 0; i < npix; i++) {
        bgra_cache_[i * 4 + 0] = decoded_image_.rgba[i * 4 + 2];
        bgra_cache_[i * 4 + 1] = decoded_image_.rgba[i * 4 + 1];
        bgra_cache_[i * 4 + 2] = decoded_image_.rgba[i * 4 + 0];
        bgra_cache_[i * 4 + 3] = decoded_image_.rgba[i * 4 + 3];
    }

    // Cache thumbnail for the current image
    if (dir_image_index_ >= 0) {
        gen_thumb_bgra(decoded_image_.rgba, decoded_image_.width, decoded_image_.height,
                       thumb_cache_[dir_image_index_],
                       thumb_cache_w_[dir_image_index_],
                       thumb_cache_h_[dir_image_index_]);
    }
    invalidate_thumb_strip();

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
            ext == ".heif" || ext == ".avif" || ext == ".jxl") {
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

        process_thumb_batch();

        if (pending_redraw_) {
            render();
        }
    }

    return 0;
}

}
