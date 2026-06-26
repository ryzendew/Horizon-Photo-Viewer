#pragma once

#include "decode/exif.hpp"

#include <cairo.h>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace hpv {

struct OverlayState {
    bool show_info = false;
    bool toolbar_visible = true;
    bool fullscreen = false;
    bool slideshow = false;
    bool show_settings = false;
    bool show_sidebar = false;
    float bg_alpha = 1.0f;

    std::string filename;
    int image_width = 0;
    int image_height = 0;
    float zoom = 1.0f;
    int image_index = 0;
    int image_count = 0;

    int slideshow_interval_ms = 5000;
    bool enable_color_management = true;
    float default_zoom = 1.0f;
    std::string theme = "dark";

    ExifInfo exif;
};

struct OverlayButton {
    int x = 0, y = 0, w = 0, h = 0;
    std::string label;
    std::function<void()> action;
};

class Overlay {
public:
    ~Overlay();

    void render_overlay(cairo_t* cr, int win_w, int win_h, const OverlayState& state);
    void render_toolbar(cairo_t* cr, int win_w, int win_h,
                        std::vector<OverlayButton>& buttons, float bg_alpha = 1.0f);
    void render_settings_popup(cairo_t* cr, int win_w, int win_h,
                               OverlayState& state, std::vector<OverlayButton>& buttons);
    void render_sidebar(cairo_t* cr, int win_w, int win_h,
                        const OverlayState& state, std::vector<OverlayButton>& buttons);
    void draw_rounded_rect(cairo_t* cr, double x, double y, double w, double h, double r);

    bool init_icons(const char* font_path);
    void destroy_icons();

    static constexpr int kToolbarHeight = 44;
    static constexpr int kToolbarHoverZone = 60;

private:
    void render_placeholder(cairo_t* cr, int win_w, int win_h);
    void render_info(cairo_t* cr, int win_w, int win_h, const OverlayState& state);

    bool ready_icon_from_codepoint(unsigned char* font_data, size_t font_size,
                                   int codepoint, int icon_size);

    std::vector<cairo_surface_t*> icon_surfaces_;
    unsigned char* font_data_ = nullptr;
    size_t font_size_ = 0;
};

}
