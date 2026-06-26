#include "ui/overlay.hpp"

#include <cmath>
#include <cstdio>
#include <iostream>
#include <sstream>
#include <vector>
#include <cstring>

#include <stb/stb_truetype.h>
#define NANOSVG_IMPLEMENTATION
#include <nanosvg/nanosvg.h>
#define NANOSVGRAST_IMPLEMENTATION
#include <nanosvg/nanosvgrast.h>

namespace hpv {

// ── Material 3 dark theme color tokens ──────────────────────────────────────
namespace m3 {
// Tokens — updated by apply_theme()
double surface_r = 0.102;
double surface_g = 0.110;
double surface_b = 0.118;
double surface_container_r = 0.145;
double surface_container_g = 0.153;
double surface_container_b = 0.161;
double surface_container_high_r = 0.188;
double surface_container_high_g = 0.200;
double surface_container_high_b = 0.216;

double on_surface_r = 0.890;
double on_surface_g = 0.886;
double on_surface_b = 0.902;
double on_surface_variant_r = 0.769;
double on_surface_variant_g = 0.776;
double on_surface_variant_b = 0.816;

double primary_r = 0.565;
double primary_g = 0.792;
double primary_b = 0.976;
double primary_container_r = 0.102;
double primary_container_g = 0.333;
double primary_container_b = 0.549;
double on_primary_container_r = 0.820;
double on_primary_container_g = 0.894;
double on_primary_container_b = 1.000;

double outline_r = 0.557;
double outline_g = 0.565;
double outline_b = 0.600;
double outline_variant_r = 0.267;
double outline_variant_g = 0.278;
double outline_variant_b = 0.310;

double tonal_container_r = 0.173;
double tonal_container_g = 0.196;
double tonal_container_b = 0.224;

void apply_theme(bool light) {
    if (!light) {
        surface_r = 0.102; surface_g = 0.110; surface_b = 0.118;
        surface_container_r = 0.145; surface_container_g = 0.153; surface_container_b = 0.161;
        surface_container_high_r = 0.188; surface_container_high_g = 0.200; surface_container_high_b = 0.216;
        on_surface_r = 0.890; on_surface_g = 0.886; on_surface_b = 0.902;
        on_surface_variant_r = 0.769; on_surface_variant_g = 0.776; on_surface_variant_b = 0.816;
        primary_r = 0.565; primary_g = 0.792; primary_b = 0.976;
        primary_container_r = 0.102; primary_container_g = 0.333; primary_container_b = 0.549;
        on_primary_container_r = 0.820; on_primary_container_g = 0.894; on_primary_container_b = 1.000;
        outline_r = 0.557; outline_g = 0.565; outline_b = 0.600;
        outline_variant_r = 0.267; outline_variant_g = 0.278; outline_variant_b = 0.310;
        tonal_container_r = 0.173; tonal_container_g = 0.196; tonal_container_b = 0.224;
    } else {
        surface_r = 0.953; surface_g = 0.953; surface_b = 0.957;
        surface_container_r = 0.933; surface_container_g = 0.933; surface_container_b = 0.941;
        surface_container_high_r = 0.918; surface_container_high_g = 0.918; surface_container_high_b = 0.925;
        on_surface_r = 0.106; on_surface_g = 0.106; on_surface_b = 0.114;
        on_surface_variant_r = 0.290; on_surface_variant_g = 0.294; on_surface_variant_b = 0.325;
        primary_r = 0.282; primary_g = 0.553; primary_b = 0.800;
        primary_container_r = 0.835; primary_container_g = 0.922; primary_container_b = 1.000;
        on_primary_container_r = 0.004; on_primary_container_g = 0.122; on_primary_container_b = 0.251;
        outline_r = 0.459; outline_g = 0.459; outline_b = 0.506;
        outline_variant_r = 0.729; outline_variant_g = 0.729; outline_variant_b = 0.761;
        tonal_container_r = 0.886; tonal_container_g = 0.918; tonal_container_b = 0.957;
    }
}
} // namespace m3

// Material Symbols Rounded codepoints (Private Use Area)
static constexpr int kIconCodepoints[] = {
    0xE2C7, // folder_open      → Open
    0xE5CB, // chevron_left     → <
    0xE5CC, // chevron_right    → >
    0xE145, // add              → +
    0xE15B, // remove           → -
    0xEA16, // fit_screen       → Fit
    0xE5D0, // fullscreen       → Full
    0xE037, // play_arrow       → Play
    0xE8B8, // settings         → Gear
    0xE88E, // info             → Info
    0xE945, // crop             → Crop
    0xE5D3, // more_vert        → Menu
};
// kNumIcons defined in overlay.hpp

static constexpr const char* kIconLabels[] = {
    "Open", "<", ">", "+", "-", "Fit", "Full", "Play", "Gear", "Info", "Crop", "Menu",
};

// Toolbar layout: index into icon/label arrays, or -1 for spacer
static constexpr int8_t kToolbarLayout[] = {
    0,      // folder (Open)
    -1,     // spacer
    1,      // chevron_left
    2,      // chevron_right
    -1,     // spacer
    3,      // add
    4,      // remove
    5,      // fit_screen
    -1,     // spacer
    6,      // fullscreen
    7,      // play_arrow
    -1,     // spacer
    10,     // crop
};
static constexpr int kNumToolbarItems = sizeof(kToolbarLayout) / sizeof(kToolbarLayout[0]);

// Right-justified icons
static constexpr int kSettingsIconIdx = 8;
static constexpr int kMetadataIconIdx = 9;
static constexpr int kMenuIconIdx = 11;

static constexpr int kIconSize = 96;

Overlay::~Overlay() {
    destroy_icons();
}

void Overlay::draw_rounded_rect(cairo_t* cr, double x, double y, double w, double h, double r) {
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;
    cairo_move_to(cr, x + r, y);
    cairo_arc(cr, x + w - r, y + r, r, -M_PI_2, 0);
    cairo_arc(cr, x + w - r, y + h - r, r, 0, M_PI_2);
    cairo_arc(cr, x + r, y + h - r, r, M_PI_2, M_PI);
    cairo_arc(cr, x + r, y + r, r, M_PI, -M_PI_2);
    cairo_close_path(cr);
}

bool Overlay::ready_icon_from_codepoint(unsigned char* fdata, size_t /*fsize*/,
                                         int codepoint, int icon_size) {
    stbtt_fontinfo info;
    if (!stbtt_InitFont(&info, fdata, 0)) return false;

    float scale = stbtt_ScaleForPixelHeight(&info, (float)icon_size);
    int gw, gh, gx, gy;
    unsigned char* bitmap = stbtt_GetCodepointBitmap(&info, scale, scale, codepoint, &gw, &gh, &gx, &gy);
    if (!bitmap || gw <= 0 || gh <= 0) {
        if (bitmap) stbtt_FreeBitmap(bitmap, nullptr);
        icon_surfaces_.push_back(nullptr);
        return false;
    }

    cairo_surface_t* surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, gw, gh);
    unsigned char* data = cairo_image_surface_get_data(surf);
    int stride = cairo_image_surface_get_stride(surf);

    for (int y = 0; y < gh; y++) {
        for (int x = 0; x < gw; x++) {
            uint8_t a = bitmap[(size_t)y * (size_t)gw + (size_t)x];
            size_t off = (size_t)y * (size_t)stride + (size_t)x * 4;
            data[off + 0] = 0;
            data[off + 1] = 0;
            data[off + 2] = 0;
            data[off + 3] = a;
        }
    }
    cairo_surface_mark_dirty(surf);
    icon_surfaces_.push_back(surf);

    stbtt_FreeBitmap(bitmap, nullptr);
    return true;
}

bool Overlay::ready_icon_from_svg(const char* svg_path, int icon_size) {
    NSVGimage* img = nsvgParseFromFile(svg_path, "px", 96);
    if (!img) {
        icon_surfaces_.push_back(nullptr);
        return false;
    }

    float scale = (float)icon_size / fmaxf(img->width, img->height);

    unsigned char* rgba = (unsigned char*)malloc((size_t)icon_size * icon_size * 4);
    if (!rgba) {
        std::cerr << "ready_icon_from_svg: malloc(" << (icon_size * icon_size * 4) << ") failed\n";
        nsvgDelete(img);
        icon_surfaces_.push_back(nullptr);
        return false;
    }
    memset(rgba, 0, (size_t)icon_size * icon_size * 4);

    NSVGrasterizer* rast = nsvgCreateRasterizer();
    if (!rast) {
        std::cerr << "ready_icon_from_svg: nsvgCreateRasterizer failed\n";
        free(rgba);
        nsvgDelete(img);
        icon_surfaces_.push_back(nullptr);
        return false;
    }

    nsvgRasterize(rast, img, 0, 0, scale, rgba, icon_size, icon_size, icon_size * 4);
    nsvgDeleteRasterizer(rast);
    nsvgDelete(img);

    // Create a cairo surface from the RGBA buffer — use ARGB32 (BGRA on little-endian)
    cairo_surface_t* surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, icon_size, icon_size);
    if (cairo_surface_status(surf) != CAIRO_STATUS_SUCCESS) {
        std::cerr << "ready_icon_from_svg: cairo_image_surface_create failed\n";
        free(rgba);
        icon_surfaces_.push_back(nullptr);
        return false;
    }

    // nanosvg outputs premultiplied RGBA, cairo expects premultiplied ARGB32 (BGRA on LE)
    unsigned char* dst = cairo_image_surface_get_data(surf);
    int stride = cairo_image_surface_get_stride(surf);
    for (int y = 0; y < icon_size; y++) {
        for (int x = 0; x < icon_size; x++) {
            int si = (y * icon_size + x) * 4;
            int di = y * stride + x * 4;
            dst[di + 0] = rgba[si + 2]; // B
            dst[di + 1] = rgba[si + 1]; // G
            dst[di + 2] = rgba[si + 0]; // R
            dst[di + 3] = rgba[si + 3]; // A
        }
    }
    cairo_surface_mark_dirty(surf);
    free(rgba);

    icon_surfaces_.push_back(surf);
    return true;
}

bool Overlay::init_icons(const char* font_path, const char* crop_svg_path) {
    destroy_icons();

    FILE* f = fopen(font_path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); return false; }

    font_data_ = new unsigned char[(size_t)sz];
    font_size_ = (size_t)sz;
    if (fread(font_data_, 1, (size_t)sz, f) != (size_t)sz) {
        fclose(f);
        destroy_icons();
        return false;
    }
    fclose(f);

    icon_surfaces_.reserve(kNumIcons);
    for (int i = 0; i < kNumIcons; i++) {
        if (i == 10 && crop_svg_path) {
            ready_icon_from_svg(crop_svg_path, kIconSize);
        } else {
            ready_icon_from_codepoint(font_data_, font_size_, kIconCodepoints[i], kIconSize);
        }
    }

    return !icon_surfaces_.empty();
}

void Overlay::destroy_icons() {
    for (auto* surf : icon_surfaces_) {
        if (surf) cairo_surface_destroy(surf);
    }
    icon_surfaces_.clear();
    delete[] font_data_;
    font_data_ = nullptr;
    font_size_ = 0;
}

void Overlay::render_overlay(cairo_t* cr, int win_w, int win_h, const OverlayState& state) {
    if (state.image_width <= 0) {
        render_placeholder(cr, win_w, win_h);
    }

    if (state.show_info && !state.filename.empty()) {
        render_info(cr, win_w, win_h, state);
    }
}

void Overlay::render_toolbar(cairo_t* cr, int win_w, int win_h,
                             std::vector<OverlayButton>& buttons,
                             int hovered_idx, int pressed_idx, float bg_alpha) {
    (void)win_h;
    (void)bg_alpha;
    int h = kToolbarHeight;

    // Toolbar background — surface (fully opaque)
    cairo_set_source_rgba(cr, m3::surface_r, m3::surface_g, m3::surface_b, 0.92);
    cairo_rectangle(cr, 0, 0, win_w, h);
    cairo_fill(cr);

    // Bottom border — outline-variant
    cairo_set_source_rgba(cr, m3::outline_variant_r, m3::outline_variant_g,
                          m3::outline_variant_b, 0.6);
    cairo_move_to(cr, 0, h);
    cairo_line_to(cr, win_w, h);
    cairo_set_line_width(cr, 1);
    cairo_stroke(cr);

    buttons.clear();

    // M3 Icon Button — 48dp touch target, 24dp icon.
    const int btn_size = 48;
    const int btn_y = (h - btn_size) / 2;
    const int icon_pad = 12;             // (48 – 24) / 2
    const int group_gap = 12;
    const int item_gap  = 4;

    auto draw_button = [&](int cx, int cy, int csize, int icon_idx, int btn_idx) {
        if (icon_idx >= (int)icon_surfaces_.size() || !icon_surfaces_[icon_idx])
            return;

        float cr2 = csize * 0.22f; // corner radius ≈ 10 px at 48dp

        // Pressed state layer (on_surface at 12 %)
        if (btn_idx == pressed_idx) {
            cairo_set_source_rgba(cr, m3::on_surface_variant_r, m3::on_surface_variant_g,
                                  m3::on_surface_variant_b, 0.12);
            draw_rounded_rect(cr, cx + 1, cy + 1, csize - 2, csize - 2, cr2);
            cairo_fill(cr);
        }
        // Hover state layer (on_surface at 8 %, only when not pressed)
        if (btn_idx == hovered_idx && btn_idx != pressed_idx) {
            cairo_set_source_rgba(cr, m3::on_surface_variant_r, m3::on_surface_variant_g,
                                  m3::on_surface_variant_b, 0.08);
            draw_rounded_rect(cr, cx + 1, cy + 1, csize - 2, csize - 2, cr2);
            cairo_fill(cr);
        }
        // Icon (Material Symbols, 24x24 effective)
        cairo_surface_t* icon = icon_surfaces_[icon_idx];
        int iw = cairo_image_surface_get_width(icon);
        int ih = cairo_image_surface_get_height(icon);
        double iscale = std::min((double)(csize - icon_pad * 2) / iw,
                                 (double)(csize - icon_pad * 2) / ih);
        if (iscale <= 0) return;
        cairo_save(cr);
        cairo_translate(cr,
            cx + (csize - (int)(iw * iscale)) / 2,
            cy + (csize - (int)(ih * iscale)) / 2);
        cairo_scale(cr, iscale, iscale);
        cairo_set_source_rgba(cr, m3::on_surface_variant_r, m3::on_surface_variant_g,
                              m3::on_surface_variant_b, 1.0);
        cairo_mask_surface(cr, icon, 0, 0);
        cairo_restore(cr);
    };

    int x = 4;  // leading edge
    int btn_idx = 0;

    for (int ti = 0; ti < kNumToolbarItems; ti++) {
        int8_t idx = kToolbarLayout[ti];
        if (idx < 0) {
            x += group_gap;
            continue;
        }
        draw_button(x, btn_y, btn_size, idx, btn_idx);
        buttons.push_back({x, btn_y, btn_size, btn_size, kIconLabels[idx], {}});
        x += btn_size + item_gap;
        btn_idx++;
    }

    // Right-justified: Menu, Settings, Info (right to left)
    int rj_x = win_w - 4;
    rj_x -= btn_size; // Settings is rightmost
    draw_button(rj_x, btn_y, btn_size, kSettingsIconIdx, btn_idx);
    buttons.push_back({rj_x, btn_y, btn_size, btn_size, kIconLabels[kSettingsIconIdx], {}});
    btn_idx++;
    rj_x -= btn_size + 4;
    draw_button(rj_x, btn_y, btn_size, kMetadataIconIdx, btn_idx);
    buttons.push_back({rj_x, btn_y, btn_size, btn_size, kIconLabels[kMetadataIconIdx], {}});
    btn_idx++;
    rj_x -= btn_size + 4;
    draw_button(rj_x, btn_y, btn_size, kMenuIconIdx, btn_idx);
    buttons.push_back({rj_x, btn_y, btn_size, btn_size, kIconLabels[kMenuIconIdx], {}});
}

void Overlay::render_menu_popup(cairo_t* cr, int win_w, int win_h,
                                const OverlayState& state,
                                std::vector<OverlayButton>& buttons) {
    (void)win_h;
    (void)state;
    int pw = 200;
    int ph = 160;
    int px = win_w - pw - 8;
    int py = Overlay::kToolbarHeight + 4;

    // Surface container background
    cairo_set_source_rgba(cr, m3::surface_container_high_r, m3::surface_container_high_g,
                          m3::surface_container_high_b, 0.95);
    draw_rounded_rect(cr, px, py, pw, ph, 10);
    cairo_fill(cr);

    // Outline
    cairo_set_source_rgba(cr, m3::outline_variant_r, m3::outline_variant_g,
                          m3::outline_variant_b, 0.6);
    cairo_set_line_width(cr, 1);
    draw_rounded_rect(cr, px, py, pw, ph, 10);
    cairo_stroke(cr);

    // Menu items
    int item_h = 44;
    const char* items[] = { "Save", "Save As", "Save As Copy" };
    for (int i = 0; i < 3; i++) {
        int iy = py + 4 + i * item_h;
        // Hover highlight
        // (hover state passed through the button vector is checked at App level)
        cairo_set_source_rgba(cr, m3::on_surface_r, m3::on_surface_g,
                              m3::on_surface_b, 0.87);
        cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL,
                               CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(cr, 14);
        cairo_move_to(cr, px + 16, iy + 28);
        cairo_show_text(cr, items[i]);

        buttons.push_back({
            px + 8, iy, pw - 16, item_h, std::string(items[i]), {}
        });
    }
}

void Overlay::render_crop_overlay(cairo_t* cr, int win_w, int win_h,
                                  const OverlayState& state,
                                  std::vector<OverlayButton>& buttons) {
    (void)state;
    // Semi-transparent dim overlay over the image area
    int top = Overlay::kToolbarHeight;
    cairo_set_source_rgba(cr, 0, 0, 0, 0.45);
    cairo_rectangle(cr, 0, top, win_w, win_h - top);
    cairo_fill(cr);

    // Draw crop rectangle (coordinates are in image space, so we need to
    // map them through zoom/pan). Since we don't have zoom/pan here,
    // we use the passed state fields.
    int img_w = state.image_width;
    int img_h = state.image_height;
    if (img_w <= 0 || img_h <= 0) return;

    // We don't have direct access to crop rect coordinates from OverlayState,
    // so we draw a simple initial rectangle centered on the image.
    // The actual interactive rectangle is passed via buttons or drawn by App.
    // For now just draw a helpful hint:
    (void)buttons;
    cairo_set_source_rgba(cr, 1, 1, 1, 0.6);
    cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL,
                           CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 18);
    cairo_move_to(cr, win_w / 2 - 80, win_h / 2);
    cairo_show_text(cr, "Crop Mode — drag to area");
}

void Overlay::render_settings_popup(cairo_t* cr, int win_w, int win_h,
                                     OverlayState& state, std::vector<OverlayButton>& buttons,
                                     M3Slider& bg_alpha_slider,
                                     M3Slider& default_zoom_slider,
                                     M3Slider& ss_interval_slider,
                                     M3Toggle& theme_toggle,
                                     M3Toggle& color_mgmt_toggle) {
    const int pw = 480, ph = 540;
    const int radius = 16;
    const int px = (win_w - pw) / 2;
    const int py = (win_h - ph) / 2;
    const int outer_pad = 24;

    // Scrim
    cairo_set_source_rgba(cr, 0, 0, 0, 0.5);
    cairo_paint(cr);

    // Dialog body — surface-container
    cairo_set_source_rgba(cr, m3::surface_container_r, m3::surface_container_g,
                          m3::surface_container_b, 1.0);
    draw_rounded_rect(cr, px, py, pw, ph, radius);
    cairo_fill(cr);

    // Border — outline-variant
    cairo_set_source_rgba(cr, m3::outline_variant_r, m3::outline_variant_g,
                          m3::outline_variant_b, 0.6);
    draw_rounded_rect(cr, px, py, pw, ph, radius);
    cairo_set_line_width(cr, 1);
    cairo_stroke(cr);

    // Title
    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, 16);
    cairo_set_source_rgba(cr, m3::on_surface_r, m3::on_surface_g, m3::on_surface_b, 1.0);
    cairo_move_to(cr, px + outer_pad, py + 28);
    cairo_show_text(cr, "Settings");

    // Close X button in title bar
    int xb_sz = 36;
    int xb_x = px + pw - outer_pad - xb_sz;
    int xb_y = py + (36 - xb_sz) / 2;
    cairo_set_source_rgba(cr, m3::on_surface_variant_r, m3::on_surface_variant_g,
                          m3::on_surface_variant_b, 1.0);
    cairo_set_line_width(cr, 2);
    int cm = 10;
    int xb_cx = xb_x + xb_sz / 2;
    int xb_cy = xb_y + xb_sz / 2;
    cairo_move_to(cr, xb_cx - cm, xb_cy - cm);
    cairo_line_to(cr, xb_cx + cm, xb_cy + cm);
    cairo_move_to(cr, xb_cx + cm, xb_cy - cm);
    cairo_line_to(cr, xb_cx - cm, xb_cy + cm);
    cairo_stroke(cr);
    buttons.push_back({xb_x, xb_y, xb_sz, xb_sz, "CloseSettings", {}});

    // Divider under title
    int div_y = py + 40;
    cairo_set_source_rgba(cr, m3::outline_variant_r, m3::outline_variant_g,
                          m3::outline_variant_b, 0.5);
    cairo_move_to(cr, px + outer_pad, div_y);
    cairo_line_to(cr, px + pw - outer_pad, div_y);
    cairo_set_line_width(cr, 1);
    cairo_stroke(cr);

    // ── Card layout constants ──────────────────────────────────────────
    const int content_x = px + 12;
    const int content_w = pw - 24;
    const int card_pad = 16;
    const double card_rad = 14.0;

    const int cat_y_off = 21;
    const int desc_y_off = 38;
    const int first_row_off = 48;
    const int row_pitch = 56;
    const int card_bot_pad = 16;

    // Within each row (56px):
    //   bandTop + 0: divider (if not first)
    //   bandTop + 6: title (14px) + value text right-aligned (11px)
    //   bandTop + 22: description (11px)
    //   bandTop + 36: slider track centered
    //   or
    //   bandTop + 16: toggle centered

    // ── Drawing helpers ────────────────────────────────────────────────
    auto draw_card_bg = [&](int cy, int ch) {
        cairo_set_source_rgba(cr, m3::surface_container_high_r,
                              m3::surface_container_high_g,
                              m3::surface_container_high_b, 0.5);
        draw_rounded_rect(cr, content_x, cy, content_w, ch, card_rad);
        cairo_fill(cr);
    };

    auto draw_cat_label = [&](int y, const char* text) {
        cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
                               CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(cr, 10);
        cairo_set_source_rgba(cr, m3::on_surface_variant_r, m3::on_surface_variant_g,
                              m3::on_surface_variant_b, 0.42);
        cairo_move_to(cr, content_x + card_pad, y);
        cairo_show_text(cr, text);
    };

    auto draw_card_desc = [&](int y, const char* text) {
        cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
                               CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(cr, 11);
        cairo_set_source_rgba(cr, m3::on_surface_variant_r, m3::on_surface_variant_g,
                              m3::on_surface_variant_b, 0.46);
        cairo_move_to(cr, content_x + card_pad, y);
        cairo_show_text(cr, text);
    };

    // right_x = right edge for right-aligned content
    auto draw_row_title_and_val = [&](int y, const char* title,
                                      const char* val_str, int right_x) {
        cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
                               CAIRO_FONT_WEIGHT_BOLD);
        cairo_set_font_size(cr, 14);
        cairo_set_source_rgba(cr, m3::on_surface_r, m3::on_surface_g,
                              m3::on_surface_b, 0.93);
        cairo_move_to(cr, content_x + card_pad, y);
        cairo_show_text(cr, title);

        if (val_str && val_str[0]) {
            cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
                                   CAIRO_FONT_WEIGHT_NORMAL);
            cairo_set_font_size(cr, 11);
            cairo_text_extents_t ve;
            cairo_text_extents(cr, val_str, &ve);
            cairo_move_to(cr, right_x - ve.width - ve.x_bearing, y);
            cairo_set_source_rgba(cr, m3::on_surface_r, m3::on_surface_g,
                                  m3::on_surface_b, 1.0);
            cairo_show_text(cr, val_str);
        }
    };

    auto draw_row_title = [&](int y, const char* text) {
        draw_row_title_and_val(y, text, nullptr, 0);
    };

    auto draw_row_desc = [&](int y, const char* text) {
        cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
                               CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(cr, 11);
        cairo_set_source_rgba(cr, m3::on_surface_variant_r, m3::on_surface_variant_g,
                              m3::on_surface_variant_b, 0.46);
        cairo_move_to(cr, content_x + card_pad, y);
        cairo_show_text(cr, text);
    };

    auto draw_divider = [&](int y) {
        cairo_set_source_rgba(cr, m3::outline_variant_r, m3::outline_variant_g,
                              m3::outline_variant_b, 0.15);
        cairo_set_line_width(cr, 1);
        cairo_move_to(cr, content_x + card_pad + 8, y);
        cairo_line_to(cr, content_x + content_w - card_pad - 8, y);
        cairo_stroke(cr);
    };



    // ═══════════════════════════════════════════════════════════════════
    // CARD 1: DISPLAY
    // ═══════════════════════════════════════════════════════════════════
    int c1_y = div_y + 12;
    int c1_h = first_row_off + 3 * row_pitch + card_bot_pad;

    draw_card_bg(c1_y, c1_h);
    draw_cat_label(c1_y + cat_y_off, "DISPLAY");
    draw_card_desc(c1_y + desc_y_off, "Adjust viewer appearance");

    // Row 0: Background Opacity (slider)
    {
        int r0 = c1_y + first_row_off;
        int sl_l = content_x + card_pad;
        int sl_w = content_w - card_pad * 2;
        int sl_y = r0 + 30;
        char val[16];
        snprintf(val, sizeof(val), "%.0f%%", state.bg_alpha * 100);
        int right_x = content_x + content_w - card_pad;
        draw_row_title_and_val(r0 + 6, "Background Opacity", val, right_x);
        draw_row_desc(r0 + 22, "Contrast of the UI background");
        bg_alpha_slider.setGeometry(sl_l, sl_y, sl_w, 14);
        bg_alpha_slider.setRange(0.0f, 1.0f);
        bg_alpha_slider.setStep(0);
        bg_alpha_slider.setShowValueLabel(true);
        bg_alpha_slider.setValueLabel(val);
        bg_alpha_slider.paint(cr);
    }

    // Divider + Row 1: Default Zoom (slider)
    {
        int r1 = c1_y + first_row_off + 1 * row_pitch;
        draw_divider(r1);
        int sl_l = content_x + card_pad;
        int sl_w = content_w - card_pad * 2;
        int sl_y = r1 + 30;
        char val[16];
        snprintf(val, sizeof(val), "%.1fx", state.default_zoom);
        int right_x = content_x + content_w - card_pad;
        draw_row_title_and_val(r1 + 6, "Default Zoom", val, right_x);
        draw_row_desc(r1 + 22, "Zoom level for new images");
        default_zoom_slider.setGeometry(sl_l, sl_y, sl_w, 14);
        default_zoom_slider.setRange(0.5f, 5.0f);
        default_zoom_slider.setStep(0.1f);
        default_zoom_slider.setShowValueLabel(true);
        default_zoom_slider.setValueLabel(val);
        default_zoom_slider.paint(cr);
    }

    // Divider + Row 2: Theme (toggle)
    {
        int r2 = c1_y + first_row_off + 2 * row_pitch;
        draw_divider(r2);
        draw_row_title(r2 + 6, "Theme");
        draw_row_desc(r2 + 22, "Dark or light appearance");
        int tg_w = 40, tg_h = 24;
        int tg_x = content_x + content_w - card_pad - tg_w;
        int tg_y = r2 + 16;
        theme_toggle.setGeometry(tg_x, tg_y, tg_w, tg_h);
        theme_toggle.paint(cr);
    }

    // ═══════════════════════════════════════════════════════════════════
    // CARD 2: SLIDESHOW
    // ═══════════════════════════════════════════════════════════════════
    int c2_y = c1_y + c1_h + 10;
    int c2_h = first_row_off + 2 * row_pitch + card_bot_pad;

    draw_card_bg(c2_y, c2_h);
    draw_cat_label(c2_y + cat_y_off, "SLIDESHOW");
    draw_card_desc(c2_y + desc_y_off, "Configure the auto-advance experience");

    // Row 0: Interval (slider)
    {
        int r0 = c2_y + first_row_off;
        int sl_l = content_x + card_pad;
        int sl_w = content_w - card_pad * 2;
        int sl_y = r0 + 30;
        int val_ms = ((state.slideshow_interval_ms + 500) / 1000) * 1000;
        char val[16];
        snprintf(val, sizeof(val), "%ds", val_ms / 1000);
        int right_x = content_x + content_w - card_pad;
        draw_row_title_and_val(r0 + 6, "Interval", val, right_x);
        draw_row_desc(r0 + 22, "Time between auto-advance (seconds)");
        ss_interval_slider.setGeometry(sl_l, sl_y, sl_w, 14);
        ss_interval_slider.setRange(1000.0f, 30000.0f);
        ss_interval_slider.setStep(1000.0f);
        ss_interval_slider.setShowValueLabel(true);
        ss_interval_slider.setValueLabel(val);
        ss_interval_slider.paint(cr);
    }

    // Divider + Row 1: Color Management (toggle)
    {
        int r1 = c2_y + first_row_off + 1 * row_pitch;
        draw_divider(r1);
        draw_row_title(r1 + 6, "Color Management");
        draw_row_desc(r1 + 22, "ICC profile color accuracy");
        int tg_w = 40, tg_h = 24;
        int tg_x = content_x + content_w - card_pad - tg_w;
        int tg_y = r1 + 16;
        color_mgmt_toggle.setGeometry(tg_x, tg_y, tg_w, tg_h);
        color_mgmt_toggle.paint(cr);
    }

    // ── Close button (M3 Tonal) ────────────────────────────────────────
    int cw = 96, ch = 36;
    int cx = px + (pw - cw) / 2;
    int cy = c2_y + c2_h + 16;

    cairo_set_source_rgba(cr, m3::primary_container_r, m3::primary_container_g,
                          m3::primary_container_b, 1.0);
    draw_rounded_rect(cr, cx, cy, cw, ch, 18);
    cairo_fill(cr);

    cairo_set_source_rgba(cr, m3::on_primary_container_r, m3::on_primary_container_g,
                          m3::on_primary_container_b, 1.0);
    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, 13);
    cairo_move_to(cr, cx + 32, cy + 24);
    cairo_show_text(cr, "Close");
    buttons.push_back({cx, cy, cw, ch, "CloseSettings", {}});
}

void Overlay::render_sidebar(cairo_t* cr, int win_w, int win_h,
                              const OverlayState& state, std::vector<OverlayButton>& buttons) {
    int sw = 320;
    int sx = win_w - sw;
    int sh = win_h;
    int top_off = kToolbarHeight;

    // Sidebar background — surface
    cairo_set_source_rgba(cr, m3::surface_r, m3::surface_g, m3::surface_b, 1.0);
    cairo_rectangle(cr, sx, 0, sw, sh);
    cairo_fill(cr);

    // Left outline — outline
    cairo_set_source_rgba(cr, m3::outline_r, m3::outline_g, m3::outline_b, 0.5);
    cairo_move_to(cr, sx, top_off);
    cairo_line_to(cr, sx, sh);
    cairo_set_line_width(cr, 1);
    cairo_stroke(cr);

    // Header row: title + close icon button
    int header_h = 44;
    int pad = 16;

    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, 16);
    cairo_set_source_rgba(cr, m3::on_surface_r, m3::on_surface_g, m3::on_surface_b, 1.0);
    cairo_move_to(cr, sx + pad, top_off + header_h - 12);
    cairo_show_text(cr, "Metadata");

    // Close button — M3 standard icon button
    int cb_size = 36;
    int cb_x = sx + sw - pad - cb_size;
    int cb_y = top_off + (header_h - cb_size) / 2;
    // Draw close mark
    cairo_set_source_rgba(cr, m3::on_surface_variant_r, m3::on_surface_variant_g,
                          m3::on_surface_variant_b, 1.0);
    cairo_set_line_width(cr, 2);
    int cm = 10;
    int c_cx = cb_x + cb_size / 2;
    int c_cy = cb_y + cb_size / 2;
    cairo_move_to(cr, c_cx - cm, c_cy - cm);
    cairo_line_to(cr, c_cx + cm, c_cy + cm);
    cairo_move_to(cr, c_cx + cm, c_cy - cm);
    cairo_line_to(cr, c_cx - cm, c_cy + cm);
    cairo_stroke(cr);
    buttons.push_back({cb_x, cb_y, cb_size, cb_size, "CloseSidebar", {}});

    // Divider
    int div_y = top_off + header_h;
    cairo_set_source_rgba(cr, m3::outline_variant_r, m3::outline_variant_g,
                          m3::outline_variant_b, 0.5);
    cairo_move_to(cr, sx + pad, div_y);
    cairo_line_to(cr, sx + sw - pad, div_y);
    cairo_set_line_width(cr, 1);
    cairo_stroke(cr);

    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 12);

    int ly = div_y + 16;
    int left_col = sx + pad;
    int val_col = sx + 128;
    int val_max_w = sw - 144;
    int line_h = 20;

    auto field = [&](const char* label, const std::string& value,
                     bool wrap = false, int wrap_lines = 2) {
        if (value.empty()) return;
        cairo_set_source_rgba(cr, m3::on_surface_variant_r, m3::on_surface_variant_g,
                              m3::on_surface_variant_b, 0.9);
        cairo_move_to(cr, left_col, ly);
        cairo_show_text(cr, label);

        cairo_set_source_rgba(cr, m3::on_surface_r, m3::on_surface_g,
                              m3::on_surface_b, 1.0);
        if (wrap) {
            std::string remain = value;
            int lines_used = 0;
            for (int line = 0; line < wrap_lines && !remain.empty(); line++) {
                int fit = 0;
                for (size_t i = 1; i <= remain.size(); i++) {
                    cairo_text_extents_t ext;
                    cairo_text_extents(cr, remain.substr(0, i).c_str(), &ext);
                    if (ext.width > val_max_w) break;
                    fit = (int)i;
                }
                if (fit == 0) fit = 1;

                std::string display;
                if (line == wrap_lines - 1 && (size_t)fit < remain.size()) {
                    while (fit > 0) {
                        std::string s = remain.substr(0, fit) + "...";
                        cairo_text_extents_t ext;
                        cairo_text_extents(cr, s.c_str(), &ext);
                        if (ext.width <= val_max_w) break;
                        fit--;
                    }
                    display = remain.substr(0, std::max(0, fit)) + "...";
                    remain.clear();
                } else {
                    display = remain.substr(0, fit);
                    remain = remain.substr(fit);
                }
                cairo_move_to(cr, val_col, ly + line * line_h);
                cairo_show_text(cr, display.c_str());
                lines_used = line + 1;
            }
            ly += line_h * lines_used;
        } else {
            cairo_move_to(cr, val_col, ly);
            cairo_show_text(cr, value.c_str());
            ly += line_h;
        }
    };

    auto section_header = [&](const char* text) {
        cairo_set_source_rgba(cr, m3::primary_r, m3::primary_g, m3::primary_b, 0.9);
        cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
        cairo_set_font_size(cr, 11);
        cairo_move_to(cr, left_col, ly);
        cairo_show_text(cr, text);
        cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(cr, 12);
        ly += line_h;
    };

    // File info
    if (!state.filename.empty()) {
        field("File", state.filename, true, 2);
    }
    if (state.image_width > 0) {
        char dim[32];
        snprintf(dim, sizeof(dim), "%d x %d", state.image_width, state.image_height);
        field("Dimensions", dim);
    }

    if (!state.exif.image_description.empty())
        field("Description", state.exif.image_description, true, 2);
    field("Camera Make", state.exif.make);
    field("Camera Model", state.exif.model);
    field("Lens", state.exif.lens);
    if (!state.exif.focal_length.empty())
        field("Focal Length", state.exif.focal_length + " mm");
    if (!state.exif.aperture.empty())
        field("Aperture", "f/" + state.exif.aperture);
    if (!state.exif.exposure.empty())
        field("Exposure", state.exif.exposure + " s");
    if (!state.exif.exposure_bias.empty())
        field("Exposure Bias", state.exif.exposure_bias + " EV");
    if (!state.exif.iso.empty())
        field("ISO", state.exif.iso);
    if (!state.exif.flash.empty())
        field("Flash", state.exif.flash);
    if (!state.exif.metering_mode.empty())
        field("Metering", state.exif.metering_mode);
    if (!state.exif.exposure_mode.empty())
        field("Exposure Mode", state.exif.exposure_mode);
    if (!state.exif.exposure_program.empty())
        field("Program", state.exif.exposure_program);
    if (!state.exif.white_balance.empty())
        field("White Balance", state.exif.white_balance);
    if (!state.exif.color_space.empty())
        field("Color Space", state.exif.color_space);
    if (!state.exif.subject_distance.empty())
        field("Subject Dist.", state.exif.subject_distance);
    if (!state.exif.digital_zoom.empty())
        field("Digital Zoom", state.exif.digital_zoom);
    if (!state.exif.scene_capture_type.empty())
        field("Scene Type", state.exif.scene_capture_type);
    if (!state.exif.contrast.empty())
        field("Contrast", state.exif.contrast);
    if (!state.exif.saturation.empty())
        field("Saturation", state.exif.saturation);
    if (!state.exif.sharpness.empty())
        field("Sharpness", state.exif.sharpness);
    if (!state.exif.date_time_original.empty() && state.exif.date_time_original != state.exif.date_time)
        field("Date Taken", state.exif.date_time_original);
    if (state.exif.date_time != state.exif.date_time_original)
        field("Date Modified", state.exif.date_time);
    else if (!state.exif.date_time.empty())
        field("Date/Time", state.exif.date_time);
    field("Orientation", std::to_string(state.exif.orientation));
    // File info
    {
        if (state.exif.file_size > 0) {
            char sz[32];
            double bytes = (double)state.exif.file_size;
            if (bytes > 1024 * 1024)
                snprintf(sz, sizeof(sz), "%.1f MiB", bytes / (1024 * 1024));
            else if (bytes > 1024)
                snprintf(sz, sizeof(sz), "%.1f KiB", bytes / 1024);
            else
                snprintf(sz, sizeof(sz), "%zu B", (size_t)bytes);
            field("File Size", sz);
        }
        if (!state.exif.file_modified.empty())
            field("Modified", state.exif.file_modified);
    }

    if (!state.exif.software.empty())
        field("Software", state.exif.software);
    if (!state.exif.artist.empty())
        field("Artist", state.exif.artist, true, 2);
    if (!state.exif.copyright.empty())
        field("Copyright", state.exif.copyright, true, 2);
    if (!state.exif.gps_latitude.empty())
        field("GPS Latitude", state.exif.gps_latitude);
    if (!state.exif.gps_longitude.empty())
        field("GPS Longitude", state.exif.gps_longitude);
    if (!state.exif.gps_altitude.empty())
        field("GPS Altitude", state.exif.gps_altitude);
    if (!state.exif.x_resolution.empty())
        field("X Resolution", state.exif.x_resolution);
    if (!state.exif.y_resolution.empty())
        field("Y Resolution", state.exif.y_resolution);

    // ICC Profile
    if (!state.exif.icc_description.empty())
        field("ICC Profile", state.exif.icc_description, true, 1);
    if (!state.exif.icc_copyright.empty())
        field("ICC Copyright", state.exif.icc_copyright, true, 1);
    if (!state.exif.icc_model.empty())
        field("ICC Model", state.exif.icc_model, true, 1);

    // Additional metadata from all_metadata
    if (!state.exif.all_metadata.empty()) {
        ly += 4;
        section_header("All Tags");
        for (auto& md : state.exif.all_metadata) {
            if (ly + line_h > sh - 16) {
                cairo_set_source_rgba(cr, m3::on_surface_variant_r, m3::on_surface_variant_g,
                                      m3::on_surface_variant_b, 0.8);
                cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
                cairo_set_font_size(cr, 11);
                cairo_move_to(cr, left_col, ly);
                cairo_show_text(cr, "... more");
                break;
            }
            field(md.first.c_str(), md.second, true, 1);
        }
    }
}

void Overlay::render_placeholder(cairo_t* cr, int win_w, int win_h) {
    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, 20);

    cairo_text_extents_t ext;
    const char* msg = "No Image Open";
    cairo_text_extents(cr, msg, &ext);
    cairo_set_source_rgba(cr, m3::on_surface_r, m3::on_surface_g, m3::on_surface_b, 1.0);
    cairo_move_to(cr, (win_w - ext.width) / 2 - ext.x_bearing,
                      win_h / 2 - 20 - ext.y_bearing);
    cairo_show_text(cr, msg);

    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 14);
    const char* sub = "Ctrl+O  Open File";
    cairo_text_extents(cr, sub, &ext);
    cairo_set_source_rgba(cr, m3::on_surface_variant_r, m3::on_surface_variant_g,
                          m3::on_surface_variant_b, 1.0);
    cairo_move_to(cr, (win_w - ext.width) / 2 - ext.x_bearing,
                      win_h / 2 + 10 - ext.y_bearing);
    cairo_show_text(cr, sub);

    const char* sub2 = "Ctrl+Q  Quit";
    cairo_text_extents(cr, sub2, &ext);
    cairo_move_to(cr, (win_w - ext.width) / 2 - ext.x_bearing,
                      win_h / 2 + 32 - ext.y_bearing);
    cairo_show_text(cr, sub2);
}

void Overlay::render_info(cairo_t* cr, int win_w, int win_h, const OverlayState& state) {
    std::ostringstream info;
    info << state.filename;
    if (state.image_width > 0) {
        info << "  |  " << state.image_width << "x" << state.image_height;
    }
    info << "  |  Zoom " << (int)(state.zoom * 100) << "%";
    if (state.image_count > 1) {
        info << "  |  " << (state.image_index + 1) << "/" << state.image_count;
    }

    // EXIF data
    std::string exif_str;
    if (!state.exif.make.empty() || !state.exif.model.empty()) {
        exif_str += state.exif.make;
        if (!state.exif.make.empty() && !state.exif.model.empty()) exif_str += " ";
        exif_str += state.exif.model;
    }
    if (!state.exif.focal_length.empty() || !state.exif.aperture.empty() ||
        !state.exif.exposure.empty() || !state.exif.iso.empty()) {
        if (!exif_str.empty()) exif_str += "  |  ";
        auto append = [&](const std::string& val) {
            if (val.empty()) return;
            if (!exif_str.empty()) exif_str += "  ";
            exif_str += val;
        };
        if (!state.exif.focal_length.empty())
            append(state.exif.focal_length + "mm");
        if (!state.exif.aperture.empty())
            append("f/" + state.exif.aperture);
        if (!state.exif.exposure.empty())
            append(state.exif.exposure + "s");
        if (!state.exif.iso.empty())
            append("ISO " + state.exif.iso);
    }
    if (!state.exif.date_time.empty()) {
        if (!exif_str.empty()) exif_str += "  |  ";
        exif_str += state.exif.date_time;
    }

    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 12);

    // Info text at bottom center — M3 chip-style
    {
        std::string info_str = info.str();
        cairo_text_extents_t ext;
        cairo_text_extents(cr, info_str.c_str(), &ext);
        int pad_h = 12, pad_v = 6;
        int px = (win_w - (int)ext.width - pad_h * 2) / 2;
        int py = win_h - 30;
        int chip_w = (int)ext.width + pad_h * 2;
        int chip_h = (int)ext.height + pad_v * 2;
        cairo_set_source_rgba(cr, m3::surface_container_r, m3::surface_container_g,
                              m3::surface_container_b, 0.95 * state.bg_alpha);
        draw_rounded_rect(cr, px, py - pad_v, chip_w, chip_h, 8);
        cairo_fill(cr);
        cairo_set_source_rgba(cr, m3::on_surface_r, m3::on_surface_g,
                              m3::on_surface_b, 0.95 * state.bg_alpha);
        cairo_move_to(cr, px + pad_h - ext.x_bearing, py - ext.y_bearing);
        cairo_show_text(cr, info_str.c_str());
    }

    // EXIF line below info — M3 chip-style
    if (!exif_str.empty()) {
        cairo_set_font_size(cr, 11);
        cairo_text_extents_t ext;
        cairo_text_extents(cr, exif_str.c_str(), &ext);
        int pad_h = 10, pad_v = 5;
        int px = (win_w - (int)ext.width - pad_h * 2) / 2;
        int py = win_h - 10;
        int chip_w = (int)ext.width + pad_h * 2;
        int chip_h = (int)ext.height + pad_v * 2;
        cairo_set_source_rgba(cr, m3::surface_container_r, m3::surface_container_g,
                              m3::surface_container_b, 0.95 * state.bg_alpha);
        draw_rounded_rect(cr, px, py - pad_v, chip_w, chip_h, 8);
        cairo_fill(cr);
        cairo_set_source_rgba(cr, m3::on_surface_variant_r, m3::on_surface_variant_g,
                              m3::on_surface_variant_b, 0.95 * state.bg_alpha);
        cairo_move_to(cr, px + pad_h - ext.x_bearing, py - ext.y_bearing);
        cairo_show_text(cr, exif_str.c_str());
    }
}

}
