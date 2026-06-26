#pragma once

#include "core/config.hpp"
#include "core/thumb_cache.hpp"
#include "core/decode_pool.hpp"
#include "core/trash.hpp"
#include "wayland/connection.hpp"
#include "wayland/seat.hpp"
#include "wayland/surface_extensions.hpp"
#include "wayland/shm_buffer.hpp"
#include "render/text_renderer.hpp"
#include "dbus/portal_file_dialog.hpp"
#include "decode/decoder.hpp"
#include "ui/overlay.hpp"
#include "ui/thumbnail_strip.hpp"

#include <cairo.h>
#include <functional>
#include <memory>
#include <string>
#include <cstdint>
#include <vector>
#include <map>
#include <poll.h>

struct wl_surface;
struct xdg_surface;
struct xdg_toplevel;
typedef struct _cairo cairo_t;

namespace hpv {

struct DecodedImage {
    std::vector<uint8_t> rgba;
    int width = 0;
    int height = 0;
    int stride = 0;
};

class App {
public:
    App(int argc, char** argv);
    ~App();
    App(const App&) = delete;
    App& operator=(const App&) = delete;

    bool init();
    int run();
    void quit() { running_ = false; }

    // Wayland events
    void set_window_size(int width, int height);
    void on_close();
    void on_key(const KeyEvent& ev);
    void on_pointer(const PointerEvent& ev);
    void on_motion(int x, int y);
    void on_scroll(const ScrollEvent& ev);

    // Buffer release callback
    void on_shm_release();

    // Image loading
    void open_file(const std::string& path);
    void open_directory(const std::string& path);
    void next_image();
    void prev_image();
    void first_image();
    void last_image();
    void delete_image();

    // View state
    void zoom_in();
    void zoom_out();
    void zoom_fit();
    void zoom_1to1();
    void toggle_fullscreen();
    void toggle_slideshow();
    void toggle_overlay();
    void toggle_settings();
    void toggle_sidebar();
    void set_bg_alpha(float a);

    // File dialog
    void open_file_dialog();
    void on_file_dialog_result(const std::string& path);

    // Called by listener trampolines (namespace scope, needs public access)
    void render();
    void present();

private:
    bool init_wayland();
    bool create_window();
    void init_font();
    void update_title();
    void load_image(const std::string& path);
    void load_directory(const std::string& path);
    std::vector<std::string> image_files_in_dir(const std::string& dir);

    // State
    bool running_ = true;
    bool pending_redraw_ = false;
    Config config_;

    // Wayland
    WaylandConnection conn_;
    wl_surface* surface_ = nullptr;
    xdg_surface* xdg_surface_ = nullptr;
    xdg_toplevel* xdg_toplevel_ = nullptr;
    SurfaceExtensions surface_extensions_;

    // Input
    Seat seat_;
    wl_data_device* data_device_ = nullptr;
    wl_data_offer* drag_offer_ = nullptr;
    std::vector<std::string> drag_mime_types_;

    // Rendering — double-buffered
    ShmBuffer bufs_[2];
    int paint_buf_ = 0;
    TextRenderer text_renderer_;

    // File dialog
    PortalFileDialog portal_dialog_;

    // Window state
    int window_width_ = 1280;
    int window_height_ = 720;
    bool fullscreen_ = false;
    bool slideshow_ = false;
    bool show_overlay_ = false;
    bool show_thumbnails_ = true;

    // Settings
    bool show_settings_ = false;
    float bg_alpha_ = 1.0f;

    // Sidebar
    bool show_sidebar_ = false;

    // Double-click tracking
    int last_thumb_click_index_ = -1;
    uint32_t last_thumb_click_time_ = 0;

    // Thumbnail strip hover (fullscreen/slideshow)
    bool show_thumbnails_hover_ = false;

    // Image data
    std::string current_path_;
    std::string current_dir_;
    std::vector<std::string> dir_images_;
    int dir_image_index_ = -1;
    DecodedImage current_image_;
    DecodedImage decoded_image_;
    float zoom_ = 1.0f;
    float pan_x_ = 0.0f;
    float pan_y_ = 0.0f;
    ExifInfo exif_info_;

    // Decoder
    DecoderRegistry decoders_;

    // Background prefetch
    DecodePool decode_pool_;

    // Cached BGRA pixels (rebuilt only when decoded_image_ changes)
    std::vector<uint8_t> bgra_cache_;

    // Drag-to-pan state
    bool dragging_ = false;
    int drag_start_x_ = 0;
    int drag_start_y_ = 0;
    float pan_start_x_ = 0.0f;
    float pan_start_y_ = 0.0f;

    // Pointer tracking (for scroll-to-position decisions)
    int pointer_x_ = 0;
    int pointer_y_ = 0;

    // Toolbar (populated during render for hit-testing)
    bool show_toolbar_ = true;
    int64_t toolbar_hide_time_ = 0;  // timestamp (ms) when cursor left hover zone; 0 = no pending hide
    std::vector<OverlayButton> toolbar_buttons_;
    Overlay overlay_;

    // Thumbnail strip
    ThumbnailStrip thumbnail_strip_;
    std::vector<ThumbnailEntry> thumb_entries_;
    int thumb_scroll_ = 0;
    cairo_surface_t* cached_strip_ = nullptr;
    int cached_strip_w_ = 0;
    bool thumb_dirty_ = true;

    // Thumbnail cache: index -> small BGRA pixels
    std::map<int, std::vector<uint8_t>> thumb_cache_;
    std::map<int, int> thumb_cache_w_, thumb_cache_h_;
    std::vector<int> thumb_pending_;  // indices queued for background decoding

    void load_thumbnail(int index);
    void process_thumb_batch(int count = 3);
    void queue_thumb_preload(int from, int to);
    void gen_thumb_bgra(const std::vector<uint8_t>& rgba, int w, int h,
                        std::vector<uint8_t>& out, int& out_w, int& out_h);
    void invalidate_thumb_strip();
    void destroy_cached_strip();

    // Data device (drag-and-drop) handlers
    static void handle_data_offer(void* data, wl_data_device* device, wl_data_offer* offer);
    static void handle_data_enter(void* data, wl_data_device* device, uint32_t serial,
                                  wl_surface* surface, wl_fixed_t x, wl_fixed_t y,
                                  wl_data_offer* offer);
    static void handle_data_leave(void* data, wl_data_device* device);
    static void handle_data_motion(void* data, wl_data_device* device, uint32_t time,
                                   wl_fixed_t x, wl_fixed_t y);
    static void handle_data_drop(void* data, wl_data_device* device);
    static void handle_data_selection(void* data, wl_data_device* device,
                                      wl_data_offer* offer);

    void on_data_drop(uint32_t serial, wl_data_offer* offer);
};

}
