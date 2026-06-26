#include "ui/overlay.hpp"

#include <cmath>
#include <cstdio>
#include <sstream>
#include <vector>

#include <stb/stb_truetype.h>

namespace hpv {

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
};
static constexpr int kNumIcons = 10;

static constexpr const char* kIconLabels[] = {
    "Open", "<", ">", "+", "-", "Fit", "Full", "Play", "Gear", "Info",
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
};
static constexpr int kNumToolbarItems = sizeof(kToolbarLayout) / sizeof(kToolbarLayout[0]);

// Right-justified icons
static constexpr int kSettingsIconIdx = 8;
static constexpr int kMetadataIconIdx = 9;

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

bool Overlay::init_icons(const char* font_path) {
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
        ready_icon_from_codepoint(font_data_, font_size_, kIconCodepoints[i], kIconSize);
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
                             std::vector<OverlayButton>& buttons, float bg_alpha) {
    (void)win_h;
    int h = kToolbarHeight;

    // Toolbar background
    cairo_set_source_rgba(cr, 0.08, 0.08, 0.10, 0.92 * bg_alpha);
    cairo_rectangle(cr, 0, 0, win_w, h);
    cairo_fill(cr);

    // Bottom border line
    cairo_set_source_rgba(cr, 0.18, 0.18, 0.20, 0.5 * bg_alpha);
    cairo_move_to(cr, 0, h);
    cairo_line_to(cr, win_w, h);
    cairo_set_line_width(cr, 1);
    cairo_stroke(cr);

    buttons.clear();
    int x = 8;
    int btn_h = h - 10;
    int btn_y = 5;
    int bw = 34;
    double radius = 6;

    for (int ti = 0; ti < kNumToolbarItems; ti++) {
        int8_t idx = kToolbarLayout[ti];
        if (idx < 0) {
            x += 12;
            continue;
        }

        cairo_save(cr);

        // Button background
        cairo_set_source_rgba(cr, 0.18, 0.18, 0.20, bg_alpha);
        draw_rounded_rect(cr, x, btn_y, bw, btn_h, radius);
        cairo_fill(cr);

        cairo_set_source_rgba(cr, 0.28, 0.28, 0.30, 0.8 * bg_alpha);
        draw_rounded_rect(cr, x, btn_y, bw, btn_h, radius);
        cairo_set_line_width(cr, 1);
        cairo_stroke(cr);

        // Draw icon
        if (idx < (int)icon_surfaces_.size() && icon_surfaces_[idx]) {
            cairo_surface_t* icon = icon_surfaces_[idx];
            int iw = cairo_image_surface_get_width(icon);
            int ih = cairo_image_surface_get_height(icon);
            double iscale = std::min((double)(bw - 8) / iw, (double)(btn_h - 8) / ih);
            int dx = x + (bw - (int)(iw * iscale)) / 2;
            int dy = btn_y + (btn_h - (int)(ih * iscale)) / 2;
            cairo_translate(cr, dx, dy);
            cairo_scale(cr, iscale, iscale);
            cairo_set_source_rgb(cr, 0.92, 0.92, 0.94);
            cairo_mask_surface(cr, icon, 0, 0);
        }

        cairo_restore(cr);

        buttons.push_back({x, btn_y, bw, btn_h, kIconLabels[idx], {}});
        x += bw + 4;
    }

    // Right-justified buttons: metadata info, then settings gear
    auto draw_rj_button = [&](int rx, int icon_idx, const char* label) {
        cairo_save(cr);

        cairo_set_source_rgba(cr, 0.18, 0.18, 0.20, bg_alpha);
        draw_rounded_rect(cr, rx, btn_y, bw, btn_h, radius);
        cairo_fill(cr);

        cairo_set_source_rgba(cr, 0.28, 0.28, 0.30, 0.8 * bg_alpha);
        draw_rounded_rect(cr, rx, btn_y, bw, btn_h, radius);
        cairo_set_line_width(cr, 1);
        cairo_stroke(cr);

        if (icon_idx < (int)icon_surfaces_.size() && icon_surfaces_[icon_idx]) {
            cairo_surface_t* icon = icon_surfaces_[icon_idx];
            int iw = cairo_image_surface_get_width(icon);
            int ih = cairo_image_surface_get_height(icon);
            double iscale = std::min((double)(bw - 8) / iw, (double)(btn_h - 8) / ih);
            int dx = rx + (bw - (int)(iw * iscale)) / 2;
            int dy = btn_y + (btn_h - (int)(ih * iscale)) / 2;
            cairo_translate(cr, dx, dy);
            cairo_scale(cr, iscale, iscale);
            cairo_set_source_rgb(cr, 0.92, 0.92, 0.94);
            cairo_mask_surface(cr, icon, 0, 0);
        }

        cairo_restore(cr);

        buttons.push_back({rx, btn_y, bw, btn_h, label, {}});
    };

    draw_rj_button(win_w - (bw + 4) * 2 - 8, kMetadataIconIdx, "Info");
    draw_rj_button(win_w - bw - 8, kSettingsIconIdx, "Gear");
}

void Overlay::render_settings_popup(cairo_t* cr, int win_w, int win_h,
                                     OverlayState& state, std::vector<OverlayButton>& buttons) {
    int pw = 360, ph = 280;
    int px = (win_w - pw) / 2;
    int py = (win_h - ph) / 2;

    // Dim background (fully opaque — settings is a modal)
    cairo_set_source_rgba(cr, 0, 0, 0, 0.5);
    cairo_paint(cr);

    // Panel background
    cairo_set_source_rgba(cr, 0.12, 0.12, 0.14, 1.0);
    draw_rounded_rect(cr, px, py, pw, ph, 12);
    cairo_fill(cr);

    // Border
    cairo_set_source_rgba(cr, 0.25, 0.25, 0.28, 0.6);
    draw_rounded_rect(cr, px, py, pw, ph, 12);
    cairo_set_line_width(cr, 1);
    cairo_stroke(cr);

    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, 16);
    cairo_set_source_rgb(cr, 0.9, 0.9, 0.9);
    cairo_move_to(cr, px + 20, py + 30);
    cairo_show_text(cr, "Settings");

    // Separator
    cairo_set_source_rgba(cr, 0.3, 0.3, 0.3, 0.5);
    cairo_move_to(cr, px + 16, py + 42);
    cairo_line_to(cr, px + pw - 16, py + 42);
    cairo_set_line_width(cr, 1);
    cairo_stroke(cr);

    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 13);
    int ly = py + 65;

    auto label = [&](const char* text, int y) {
        cairo_set_source_rgb(cr, 0.7, 0.7, 0.7);
        cairo_move_to(cr, px + 20, y);
        cairo_show_text(cr, text);
    };

    // Background Opacity slider
    label("Background Opacity", ly);
    int slx = px + 180, sly = ly - 10, slw = 160, slh = 16;
    cairo_set_source_rgba(cr, 0.25, 0.25, 0.28, 0.8);
    draw_rounded_rect(cr, slx, sly, slw, slh, 8);
    cairo_fill(cr);
    int fill_w = (int)(slw * state.bg_alpha);
    if (fill_w > 0) {
        cairo_set_source_rgba(cr, 0.4, 0.6, 1.0, 0.8);
        draw_rounded_rect(cr, slx, sly, fill_w, slh, 8);
        cairo_fill(cr);
    }
    // Percentage text
    char alpha_pct[16];
    snprintf(alpha_pct, sizeof(alpha_pct), "%.0f%%", state.bg_alpha * 100);
    cairo_set_font_size(cr, 11);
    cairo_set_source_rgb(cr, 0.9, 0.9, 0.9);
    cairo_move_to(cr, slx + slw / 2 - 15, sly + 12);
    cairo_show_text(cr, alpha_pct);
    buttons.push_back({slx, sly, slw, slh, "bg_alpha", {}});

    ly += 40;

    // Slideshow interval
    char ss_buf[64];
    snprintf(ss_buf, sizeof(ss_buf), "Slideshow Interval: %d ms", state.slideshow_interval_ms);
    label(ss_buf, ly);
    ly += 24;

    // Color management
    label(state.enable_color_management ? "Color Management: on" : "Color Management: off", ly);
    ly += 24;

    // Default zoom
    char dz_buf[32];
    snprintf(dz_buf, sizeof(dz_buf), "Default Zoom: %.1fx", state.default_zoom);
    label(dz_buf, ly);
    ly += 24;

    // Theme
    std::string theme_label = "Theme: " + state.theme;
    label(theme_label.c_str(), ly);
    ly += 40;

    // Close button
    int cw = 80, ch = 32;
    int cx = px + (pw - cw) / 2;
    int cy = py + ph - ch - 16;
    cairo_set_source_rgba(cr, 0.3, 0.3, 0.35, 1.0);
    draw_rounded_rect(cr, cx, cy, cw, ch, 6);
    cairo_fill(cr);
    cairo_set_source_rgb(cr, 0.9, 0.9, 0.9);
    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, 13);
    cairo_move_to(cr, cx + 25, cy + 21);
    cairo_show_text(cr, "Close");
    buttons.push_back({cx, cy, cw, ch, "CloseSettings", {}});
}

void Overlay::render_sidebar(cairo_t* cr, int win_w, int win_h,
                              const OverlayState& state, std::vector<OverlayButton>& buttons) {
    int sw = 320;
    int sx = win_w - sw;
    int sh = win_h;
    int top_off = kToolbarHeight;

    // Left border (separates sidebar from content area, below toolbar)
    cairo_set_source_rgba(cr, 0.25, 0.25, 0.28, 0.6);
    cairo_move_to(cr, sx, top_off);
    cairo_line_to(cr, sx, sh);
    cairo_set_line_width(cr, 1);
    cairo_stroke(cr);

    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, 16);
    cairo_set_source_rgb(cr, 0.9, 0.9, 0.9);
    cairo_move_to(cr, sx + 16, top_off + 26);
    cairo_show_text(cr, "Metadata");

    // Close button (X) at top-right of sidebar area
    {
        int cbx = sx + sw - 32;
        int cby = top_off + 4;
        int cbw = 24;
        int cbh = 24;
        cairo_set_source_rgba(cr, 0.3, 0.3, 0.35, 1.0);
        draw_rounded_rect(cr, cbx, cby, cbw, cbh, 4);
        cairo_fill(cr);
        cairo_set_source_rgb(cr, 0.9, 0.9, 0.9);
        cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
        cairo_set_font_size(cr, 14);
        cairo_move_to(cr, cbx + 7, cby + 17);
        cairo_show_text(cr, "X");
        buttons.push_back({cbx, cby, cbw, cbh, "CloseSidebar", {}});
    }

    // Separator
    cairo_set_source_rgba(cr, 0.3, 0.3, 0.3, 0.5);
    cairo_move_to(cr, sx + 12, top_off + 40);
    cairo_line_to(cr, sx + sw - 12, top_off + 40);
    cairo_set_line_width(cr, 1);
    cairo_stroke(cr);

    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 12);

    int ly = top_off + 58;
    int left_col = sx + 16;
    int val_col = sx + 120;
    int val_max_w = sw - 136;
    int line_h = 20;

    // Draw a label-value pair with optional value wrapping
    auto field = [&](const char* label, const std::string& value,
                     bool wrap = false, int wrap_lines = 2) {
        if (value.empty()) return;
        cairo_set_source_rgba(cr, 0.6, 0.6, 0.65, 0.9);
        cairo_move_to(cr, left_col, ly);
        cairo_show_text(cr, label);

        cairo_set_source_rgb(cr, 0.9, 0.9, 0.9);
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
        cairo_set_source_rgba(cr, 0.5, 0.7, 1.0, 0.9);
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
                // Not enough room — show indicator
                cairo_set_source_rgba(cr, 0.5, 0.5, 0.5, 0.8);
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
    cairo_set_source_rgb(cr, 1, 1, 1);
    cairo_move_to(cr, (win_w - ext.width) / 2 - ext.x_bearing,
                      win_h / 2 - 20 - ext.y_bearing);
    cairo_show_text(cr, msg);

    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 14);
    const char* sub = "Ctrl+O  Open File";
    cairo_text_extents(cr, sub, &ext);
    cairo_set_source_rgb(cr, 0.7, 0.7, 0.7);
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

    // Info text at bottom center
    {
        std::string info_str = info.str();
        cairo_text_extents_t ext;
        cairo_text_extents(cr, info_str.c_str(), &ext);
        int px = (win_w - (int)ext.width - 20) / 2;
        int py = win_h - 30;
        cairo_set_source_rgba(cr, 0, 0, 0, 0.6 * state.bg_alpha);
        draw_rounded_rect(cr, px - 6, py - 4, ext.width + 20, ext.height + 8, 10);
        cairo_fill(cr);
        cairo_set_source_rgb(cr, 0.8, 0.8, 0.8);
        cairo_move_to(cr, px + 4 - ext.x_bearing, py - ext.y_bearing);
        cairo_show_text(cr, info_str.c_str());
    }

    // EXIF line below info
    if (!exif_str.empty()) {
        cairo_set_font_size(cr, 11);
        cairo_text_extents_t ext;
        cairo_text_extents(cr, exif_str.c_str(), &ext);
        int px = (win_w - (int)ext.width - 20) / 2;
        int py = win_h - 10;
        cairo_set_source_rgba(cr, 0, 0, 0, 0.6 * state.bg_alpha);
        draw_rounded_rect(cr, px - 6, py - 4, ext.width + 20, ext.height + 8, 10);
        cairo_fill(cr);
        cairo_set_source_rgb(cr, 0.7, 0.7, 0.75);
        cairo_move_to(cr, px + 4 - ext.x_bearing, py - ext.y_bearing);
        cairo_show_text(cr, exif_str.c_str());
    }
}

}
