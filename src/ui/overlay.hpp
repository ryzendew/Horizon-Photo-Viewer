#pragma once

#include "ui/m3_widgets.hpp"
#include "decode/exif.hpp"

#include <cairo.h>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace hpv {

// M3 dynamic color tokens — call apply_theme() before rendering to switch
namespace m3 {
extern double surface_r, surface_g, surface_b;
extern double surface_container_r, surface_container_g, surface_container_b;
extern double surface_container_high_r, surface_container_high_g, surface_container_high_b;
extern double on_surface_r, on_surface_g, on_surface_b;
extern double on_surface_variant_r, on_surface_variant_g, on_surface_variant_b;
extern double primary_r, primary_g, primary_b;
extern double primary_container_r, primary_container_g, primary_container_b;
extern double on_primary_container_r, on_primary_container_g, on_primary_container_b;
extern double outline_r, outline_g, outline_b;
extern double outline_variant_r, outline_variant_g, outline_variant_b;
extern double tonal_container_r, tonal_container_g, tonal_container_b;
void apply_theme(bool light);
}

struct OverlayState {
    bool show_info = false;
    bool toolbar_visible = true;
    bool fullscreen = false;
    bool slideshow = false;
    bool show_settings = false;
    bool show_sidebar = false;
    bool show_menu = false;
    bool crop_active = false;
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
                        std::vector<OverlayButton>& buttons,
                        int hovered_idx = -1, int pressed_idx = -1,
                        float bg_alpha = 1.0f);
    void render_settings_popup(cairo_t* cr, int win_w, int win_h,
                               OverlayState& state, std::vector<OverlayButton>& buttons,
                               M3Slider& bg_alpha_slider,
                               M3Slider& default_zoom_slider,
                               M3Slider& ss_interval_slider,
                               M3Toggle& theme_toggle,
                               M3Toggle& color_mgmt_toggle);
    void render_sidebar(cairo_t* cr, int win_w, int win_h,
                        const OverlayState& state, std::vector<OverlayButton>& buttons);
    void render_crop_overlay(cairo_t* cr, int win_w, int win_h,
                             const OverlayState& state, std::vector<OverlayButton>& buttons);
    void render_menu_popup(cairo_t* cr, int win_w, int win_h,
                           const OverlayState& state, std::vector<OverlayButton>& buttons);
    void draw_rounded_rect(cairo_t* cr, double x, double y, double w, double h, double r);

    bool init_icons(const char* font_path, const char* crop_svg_path);
    void destroy_icons();
    bool crop_icon_loaded() const {
        return kNumIcons > 10 && icon_surfaces_.size() > 10 && icon_surfaces_[10] != nullptr;
    }

    static constexpr int kToolbarHeight = 48;
    static constexpr int kToolbarHoverZone = 64;
    static constexpr int kNumIcons = 12;

private:
    void render_placeholder(cairo_t* cr, int win_w, int win_h);
    void render_info(cairo_t* cr, int win_w, int win_h, const OverlayState& state);

    bool ready_icon_from_codepoint(unsigned char* font_data, size_t font_size,
                                   int codepoint, int icon_size);
    bool ready_icon_from_svg(const char* svg_path, int icon_size);

    std::vector<cairo_surface_t*> icon_surfaces_;
    unsigned char* font_data_ = nullptr;
    size_t font_size_ = 0;
};

}
