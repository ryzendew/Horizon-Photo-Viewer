#include "ui/overlay.hpp"

#include <cstdio>

namespace hpv {

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
    buttons.push_back({xb_x, xb_y, xb_sz, xb_sz, "CloseSettings", {}, {}});

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
    buttons.push_back({cx, cy, cw, ch, "CloseSettings", {}, {}});
}

} // namespace hpv
