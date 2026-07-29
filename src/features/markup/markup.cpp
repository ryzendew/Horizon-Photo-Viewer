#include "core/viewer/app.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <sstream>

namespace hpv {

// --- Helper: hit-test an element at image-space coordinates ---
bool App::hit_test_markup_element(const MarkupElement& el, float img_x, float img_y) const {
    float threshold = std::max(6.0f, el.thickness * 0.6f);
    switch (el.type) {
    case MarkupTool::kPen: {
        if (el.points_x.empty()) return false;
        float best = 1e9f;
        for (size_t i = 0; i + 1 < el.points_x.size(); i++) {
            float x1 = el.points_x[i], y1 = el.points_y[i];
            float x2 = el.points_x[i + 1], y2 = el.points_y[i + 1];
            float dx = x2 - x1, dy = y2 - y1;
            float len2 = dx * dx + dy * dy;
            if (len2 < 0.01f) {
                float d2 = (img_x - x1) * (img_x - x1) + (img_y - y1) * (img_y - y1);
                best = std::min(best, d2);
                continue;
            }
            float t = std::clamp(((img_x - x1) * dx + (img_y - y1) * dy) / len2, 0.0f, 1.0f);
            float px = x1 + t * dx, py = y1 + t * dy;
            float d2 = (img_x - px) * (img_x - px) + (img_y - py) * (img_y - py);
            best = std::min(best, d2);
        }
        return best < threshold * threshold;
    }
    case MarkupTool::kLine:
    case MarkupTool::kArrow: {
        if (el.points_x.size() < 2) return false;
        float x1 = el.points_x[0], y1 = el.points_y[0];
        float x2 = el.points_x[1], y2 = el.points_y[1];
        float dx = x2 - x1, dy = y2 - y1;
        float len2 = dx * dx + dy * dy;
        if (len2 < 0.01f) {
            float d2 = (img_x - x1) * (img_x - x1) + (img_y - y1) * (img_y - y1);
            return d2 < threshold * threshold;
        }
        float t = std::clamp(((img_x - x1) * dx + (img_y - y1) * dy) / len2, 0.0f, 1.0f);
        float px = x1 + t * dx, py = y1 + t * dy;
        float d2 = (img_x - px) * (img_x - px) + (img_y - py) * (img_y - py);
        return d2 < threshold * threshold;
    }
    case MarkupTool::kRect:
    case MarkupTool::kEllipse:
        if (el.rect_w <= 0 || el.rect_h <= 0) return false;
        return img_x >= el.rect_x - threshold && img_x <= el.rect_x + el.rect_w + threshold &&
               img_y >= el.rect_y - threshold && img_y <= el.rect_y + el.rect_h + threshold;
    case MarkupTool::kText: {
        float tw = el.text_box_w > 0 ? el.text_box_w : 100;
        float th = el.text_box_h > 0 ? el.text_box_h : el.font_size * 1.5f;
        return img_x >= el.text_x && img_x <= el.text_x + tw &&
               img_y >= el.text_y - el.font_size * 0.2f && img_y <= el.text_y + th;
    }
    case MarkupTool::kNumbered: {
        if (el.points_x.empty()) return false;
        float cx = el.points_x[0], cy = el.points_y[0];
        float r = std::max(12.0f, el.thickness * 3.0f);
        float dx = img_x - cx, dy = img_y - cy;
        return dx * dx + dy * dy < r * r;
    }
    default:
        return false;
    }
}

// --- Helper: bounding box of an element in image coordinates ---
bool App::get_markup_element_bbox(const MarkupElement& el,
                                  float& x, float& y, float& w, float& h) const {
    switch (el.type) {
    case MarkupTool::kPen:
    case MarkupTool::kLine:
    case MarkupTool::kArrow: {
        if (el.points_x.empty()) return false;
        float minx = el.points_x[0], maxx = el.points_x[0];
        float miny = el.points_y[0], maxy = el.points_y[0];
        for (size_t i = 1; i < el.points_x.size(); i++) {
            minx = std::min(minx, el.points_x[i]);
            maxx = std::max(maxx, el.points_x[i]);
            miny = std::min(miny, el.points_y[i]);
            maxy = std::max(maxy, el.points_y[i]);
        }
        float pad = std::max(4.0f, el.thickness + 2);
        x = minx - pad; y = miny - pad;
        w = maxx - minx + pad * 2; h = maxy - miny + pad * 2;
        return true;
    }
    case MarkupTool::kRect:
    case MarkupTool::kEllipse: {
        x = el.rect_x; y = el.rect_y;
        w = el.rect_w; h = el.rect_h;
        return w > 0 && h > 0;
    }
    case MarkupTool::kText: {
        x = el.text_x;
        y = el.text_y;
        w = el.text_box_w > 0 ? el.text_box_w : 100;
        if (el.text_box_h > 0) {
            h = el.text_box_h;
        } else {
            int line_count = 1;
            for (char c : el.text) if (c == '\n') line_count++;
            float est_h = line_count * el.font_size * el.line_spacing + 4;
            h = std::max(el.font_size * 1.5f, est_h);
        }
        return true;
    }
    case MarkupTool::kNumbered: {
        if (el.points_x.empty()) return false;
        float r = std::max(12.0f, el.thickness * 3.0f);
        x = el.points_x[0] - r; y = el.points_y[0] - r;
        w = r * 2; h = r * 2;
        return true;
    }
    case MarkupTool::kImage: {
        x = el.rect_x; y = el.rect_y;
        w = el.rect_w; h = el.rect_h;
        return w > 0 && h > 0;
    }
    default:
        return false;
    }
}

// --- Helper: build rendered line list from text (used in both rendering and cursor hittest) ---
void App::compute_text_layout(cairo_t* cr, const MarkupElement& el,
                              std::vector<std::string>& out_lines,
                              std::vector<size_t>& out_start_pos,
                              float& out_line_height) {
    out_lines.clear();
    out_start_pos.clear();
    out_line_height = el.font_size * el.line_spacing;

    // Split into paragraphs on newlines, tracking raw character positions
    struct Para { std::string text; size_t start; };
    std::vector<Para> paras;
    size_t start = 0;
    for (size_t pos = 0; pos <= el.text.size(); pos++) {
        if (pos == el.text.size() || el.text[pos] == '\n') {
            paras.push_back({el.text.substr(start, pos - start), start});
            start = pos + 1;
        }
    }

    if (el.text_box_w > 0) {
        for (const auto& para : paras) {
            if (para.text.empty()) {
                out_lines.push_back(std::string());
                out_start_pos.push_back(para.start);
                continue;
            }
            // Extract words and their offsets within the paragraph
            std::vector<std::string> words;
            std::vector<size_t> word_offsets;
            size_t search_off = 0;
            {
                std::istringstream stream(para.text);
                std::string w;
                while (stream >> w) {
                    words.push_back(w);
                    size_t f = para.text.find(w, search_off);
                    word_offsets.push_back(f);
                    search_off = f + w.size();
                }
            }
            size_t wi = 0;
            while (wi < words.size()) {
                std::string cur_line = words[wi];
                size_t cur_start = para.start + word_offsets[wi];
                wi++;
                while (wi < words.size()) {
                    std::string test = cur_line + " " + words[wi];
                    cairo_text_extents_t te;
                    cairo_text_extents(cr, test.c_str(), &te);
                    if (te.x_advance > el.text_box_w) break;
                    cur_line = test;
                    wi++;
                }
                out_lines.push_back(cur_line);
                out_start_pos.push_back(cur_start);
            }
        }
    } else {
        for (const auto& para : paras) {
            if (!para.text.empty()) {
                out_lines.push_back(para.text);
                out_start_pos.push_back(para.start);
            }
        }
    }
}

// --- Helper: measure character position from image-space click in editing text ---
int App::get_text_cursor_at(const MarkupElement& el, float img_x, float img_y) const {
    auto surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
    auto tmp = cairo_create(surface);
    cairo_select_font_face(tmp, el.font_family.c_str(),
                           CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(tmp, el.font_size);

    std::vector<std::string> lines;
    std::vector<size_t> start_pos;
    float lh;
    compute_text_layout(tmp, el, lines, start_pos, lh);

    float base_y = el.text_y + el.font_size;

    // Find which line
    int line_idx = 0;
    for (size_t i = 0; i < lines.size(); i++) {
        float ly = base_y + i * lh;
        if (img_y < ly + lh * 0.5f) break;
        line_idx = (int)i;
    }
    if (line_idx >= (int)lines.size()) line_idx = (int)lines.size() - 1;

    // Find character within the line
    const std::string& line_text = lines[line_idx];
    float rel_x = img_x - el.text_x;
    int char_in_line = 0;
    if (rel_x > 0) {
        char_in_line = (int)line_text.size();
        for (size_t i = 1; i <= line_text.size(); i++) {
            cairo_text_extents_t te;
            cairo_text_extents(tmp, line_text.substr(0, i).c_str(), &te);
            if (te.x_advance >= rel_x) {
                cairo_text_extents_t prev;
                if (i > 1)
                    cairo_text_extents(tmp, line_text.substr(0, i - 1).c_str(), &prev);
                else
                    prev.x_advance = 0;
                float mid = (prev.x_advance + te.x_advance) * 0.5f;
                char_in_line = (rel_x < mid) ? (int)(i - 1) : (int)i;
                break;
            }
        }
    }

    cairo_destroy(tmp);
    cairo_surface_destroy(surface);

    return (int)start_pos[line_idx] + char_in_line;
}

void App::apply_text_format_(uint32_t color, float font_size) {
    if (!markup_text_editing_ || !markup_current_ ||
        markup_current_->type != MarkupTool::kText ||
        markup_text_sel_start_ < 0 ||
        markup_text_sel_start_ == markup_text_cursor_pos_)
        return;

    int sel_a = std::min(markup_text_sel_start_, markup_text_cursor_pos_);
    int sel_b = std::max(markup_text_sel_start_, markup_text_cursor_pos_);
    if (sel_a >= sel_b) return;

    // If no actual override, just clear selection
    if (color == 0 && font_size == 0.f) {
        markup_text_cursor_pos_ = sel_b;
        markup_text_sel_start_ = -1;
        render();
        return;
    }

    auto& spans = markup_current_->format_spans;
    // Remove spans fully inside selection
    spans.erase(std::remove_if(spans.begin(), spans.end(),
        [&](const auto& s) { return (int)s.start >= sel_a && (int)s.end <= sel_b; }),
        spans.end());
    // Trim spans that partially overlap
    for (auto& s : spans) {
        if ((int)s.start < sel_a && (int)s.end > sel_a && (int)s.end <= sel_b)
            s.end = sel_a;
        else if ((int)s.start >= sel_a && (int)s.start < sel_b && (int)s.end > sel_b)
            s.start = sel_b;
        else if ((int)s.start < sel_a && (int)s.end > sel_b) {
            // Split: keep prefix, add suffix as a new span
            MarkupElement::FormatSpan suffix = s;
            suffix.start = sel_b;
            s.end = sel_a;
            spans.push_back(suffix);
        }
    }
    // Add new span (last = highest priority)
    spans.push_back({(size_t)sel_a, (size_t)sel_b, color, font_size});

    // Clear selection, keep cursor at end
    markup_text_cursor_pos_ = sel_b;
    markup_text_sel_start_ = -1;
    render();
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

    auto draw_text = [&](const MarkupElement& el) {
        bool is_editing = markup_text_editing_ && &el == markup_current_.get();
        bool is_drawing = markup_drawing_ && &el == markup_current_.get() && !is_editing;
        cairo_select_font_face(cr, el.font_family.c_str(),
                               CAIRO_FONT_SLANT_NORMAL,
                               CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(cr, el.font_size);

        // Build line layout via shared helper
        std::vector<std::string> lines;
        std::vector<size_t> line_start;
        float line_height;
        compute_text_layout(cr, el, lines, line_start, line_height);

        // Compute content width (widest line)
        float content_w = 0;
        for (size_t li = 0; li < lines.size(); li++) {
            cairo_text_extents_t te;
            cairo_text_extents(cr, lines[li].c_str(), &te);
            if (te.x_advance > content_w) content_w = te.x_advance;
        }
        float box_w = el.text_box_w > 0 ? el.text_box_w : content_w;
        float total_h = el.text_box_h > 0 ? el.text_box_h
                        : std::max(line_height, (float)lines.size() * line_height + 4);

        // Text box background and dashed border during editing or drawing
        if (is_editing || is_drawing) {
            cairo_save(cr);
            if (is_editing) {
                cairo_set_source_rgba(cr, 0.3, 0.5, 0.9, 0.12);
                cairo_rectangle(cr, el.text_x, el.text_y, box_w, total_h);
                cairo_fill(cr);
            }
            cairo_set_source_rgba(cr, 0.3, 0.5, 0.9, 0.5);
            cairo_set_line_width(cr, 1.0);
            const double dashes[] = {4.0, 3.0};
            cairo_set_dash(cr, dashes, 2, 0);
            cairo_rectangle(cr, el.text_x, el.text_y, box_w, total_h);
            cairo_stroke(cr);
            cairo_set_dash(cr, nullptr, 0, 0);
            cairo_restore(cr);
        }

        // Selection highlight overlay
        if (is_editing && markup_text_sel_start_ >= 0 &&
            markup_text_sel_start_ != markup_text_cursor_pos_) {
            int sel_a = std::min(markup_text_sel_start_, markup_text_cursor_pos_);
            int sel_b = std::max(markup_text_sel_start_, markup_text_cursor_pos_);
            for (size_t li = 0; li < lines.size(); li++) {
                size_t ls = line_start[li];
                size_t le = (li + 1 < lines.size()) ? line_start[li + 1] : el.text.size();
                // le should account for newline that was consumed
                // Adjust: if lines are from word-wrap, they're contiguous in the paragraph
                if (li + 1 < lines.size()) {
                    // The next line starts at line_start[li+1]; the current line ends at
                    // line_start[li] + lines[li].size() in raw text (plus any newline if present)
                    le = ls + lines[li].size();
                    if (le < el.text.size() && el.text[le] == '\n') le++;
                }
                if ((int)le <= sel_a || (int)ls >= sel_b) continue;

                // Compute highlight rect for this line
                float lx = el.text_x;
                float ly = el.text_y + el.font_size + li * line_height;
                float highlight_x1 = lx, highlight_x2 = lx;

                // Before selection part
                int pre_len = std::max(0, sel_a - (int)ls);
                if (pre_len > 0 && (size_t)pre_len <= lines[li].size()) {
                    cairo_text_extents_t te;
                    cairo_text_extents(cr, lines[li].substr(0, pre_len).c_str(), &te);
                    highlight_x1 = lx + te.x_advance;
                }
                int sel_len = std::min((int)lines[li].size(), sel_b - (int)ls) - pre_len;
                if (pre_len + sel_len > 0 && (size_t)(pre_len + sel_len) <= lines[li].size()) {
                    cairo_text_extents_t te;
                    cairo_text_extents(cr, lines[li].substr(0, pre_len + sel_len).c_str(), &te);
                    highlight_x2 = lx + te.x_advance;
                }

                cairo_set_source_rgba(cr, 0.3, 0.5, 0.9, 0.25);
                cairo_rectangle(cr, highlight_x1, ly - el.font_size + 2,
                                highlight_x2 - highlight_x1, line_height);
                cairo_fill(cr);
            }
        }

        // Helper: draw line segment with color
        auto draw_seg = [&](const std::string& text, float x, float y, uint32_t col) {
            cairo_set_source_rgba(cr,
                ((col >> 24) & 0xFF) / 255.0,
                ((col >> 16) & 0xFF) / 255.0,
                ((col >> 8) & 0xFF) / 255.0,
                ((col >> 0) & 0xFF) / 255.0);
            cairo_move_to(cr, x, y);
            cairo_show_text(cr, text.c_str());
        };

        float base_y = el.text_y + el.font_size;

        for (size_t li = 0; li < lines.size(); li++) {
            float lx = el.text_x;
            float ly = base_y + li * line_height;
            size_t ls = line_start[li];
            size_t le = ls + lines[li].size();

            // Determine segments per character: split on format-span boundaries
            struct Seg { size_t start, end; uint32_t col; float fs; };
            std::vector<Seg> segs;
            segs.push_back({ls, le, el.color, el.font_size});
            for (const auto& span : el.format_spans) {
                size_t sa = std::max(span.start, ls);
                size_t sb = std::min(span.end, le);
                if (sa >= sb) continue;
                // Split existing segments
                std::vector<Seg> new_segs;
                for (const auto& sg : segs) {
                    if (sg.end <= sa || sg.start >= sb) {
                        new_segs.push_back(sg);
                    } else {
                        if (sg.start < sa) new_segs.push_back({sg.start, sa, sg.col, sg.fs});
                        uint32_t span_col = span.color ? span.color : sg.col;
                        float span_fs = span.font_size > 0 ? span.font_size : sg.fs;
                        new_segs.push_back({sa, sb, span_col, span_fs});
                        if (sb < sg.end) new_segs.push_back({sb, sg.end, sg.col, sg.fs});
                    }
                }
                segs = new_segs;
            }

            // Render each segment
            for (const auto& sg : segs) {
                if (sg.start > le || sg.end <= ls) continue;
                // Extract the substring for this segment
                int line_off = (int)(sg.start - ls);
                int seg_len = (int)(sg.end - sg.start);
                if (line_off < 0) { seg_len += line_off; line_off = 0; }
                if (seg_len <= 0) continue;
                if ((size_t)(line_off + seg_len) > lines[li].size()) {
                    seg_len = (int)lines[li].size() - line_off;
                }
                std::string seg_text = lines[li].substr(line_off, seg_len);
                if (seg_text.empty()) continue;

                // Measure x offset from start of line
                cairo_text_extents_t te;
                if (line_off > 0) {
                    cairo_text_extents(cr, lines[li].substr(0, line_off).c_str(), &te);
                } else {
                    te.x_advance = 0;
                }
                float sx = lx + te.x_advance;

                // Render shadow and outline
                if (el.text_shadow) {
                    float off = std::max(1.0f, el.font_size * 0.05f);
                    draw_seg(seg_text, sx + off, ly + off, el.shadow_color);
                }
                if (el.text_outline) {
                    float ow = std::max(0.5f, el.outline_width);
                    for (int dy = -1; dy <= 1; dy++)
                        for (int dx = -1; dx <= 1; dx++)
                            if (dx != 0 || dy != 0)
                                draw_seg(seg_text, sx + dx * ow, ly + dy * ow, el.outline_color);
                }
                // Render with segment color
                draw_seg(seg_text, sx, ly, sg.col);
            }
        }

        // Cursor during editing
        if (is_editing) {
            int cursor = markup_text_cursor_pos_;
            size_t cli = 0;
            for (size_t i = 0; i < lines.size(); i++) {
                size_t le = line_start[i] + lines[i].size();
                if (i + 1 < lines.size()) {
                    if (le < el.text.size() && el.text[le] == '\n') le++;
                }
                if ((size_t)cursor < le || i + 1 == lines.size()) {
                    cli = i;
                    break;
                }
            }
            if (cli >= lines.size()) cli = lines.size() - 1;

            int off_in_line = (int)cursor - (int)line_start[cli];
            if (off_in_line < 0) off_in_line = 0;
            if ((size_t)off_in_line > lines[cli].size()) off_in_line = (int)lines[cli].size();

            cairo_text_extents_t te;
            if (off_in_line > 0) {
                cairo_text_extents(cr, lines[cli].substr(0, off_in_line).c_str(), &te);
            } else {
                te.x_advance = 0;
            }
            float cx = el.text_x + te.x_advance + 1;
            float cy = base_y + cli * line_height;
            cairo_set_source_rgba(cr, 0.3, 0.5, 0.9, 0.8);
            cairo_set_line_width(cr, 1.5);
            cairo_move_to(cr, cx, cy - el.font_size + 2);
            cairo_line_to(cr, cx, cy + 2);
            cairo_stroke(cr);
        }
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

    auto draw_image = [&](const MarkupElement& el) {
        if (!el.image_data) return;
        auto& img = *el.image_data;
        if (img.rgba.empty() || img.width <= 0 || img.height <= 0) return;
        cairo_save(cr);
        cairo_translate(cr, el.rect_x, el.rect_y);
        cairo_scale(cr, el.rect_w / img.width, el.rect_h / img.height);
        cairo_surface_t* surf = cairo_image_surface_create_for_data(
            img.rgba.data(), CAIRO_FORMAT_ARGB32,
            img.width, img.height, img.stride);
        if (surf) {
            cairo_set_source_surface(cr, surf, 0, 0);
            cairo_paint(cr);
            cairo_surface_destroy(surf);
        }
        cairo_restore(cr);
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
        case MarkupTool::kText: draw_text(el); break;
        case MarkupTool::kImage: draw_image(el); break;
        default: draw_pen(el); break;
        }
    };

    for (const auto& el : markup_elements_) if (el.visible) draw_one(el);
    if (markup_current_) draw_one(*markup_current_);

    // Draw selection box in image coordinates (transform applies)
    if (markup_selected_idx_ >= 0 &&
        markup_selected_idx_ < (int)markup_elements_.size()) {
        const auto& sel = markup_elements_[markup_selected_idx_];
        float bx, by, bw, bh;
        if (get_markup_element_bbox(sel, bx, by, bw, bh)) {
            cairo_matrix_t mat;
            cairo_get_matrix(cr, &mat);
            float sc = (float)std::max(fabs(mat.xx), fabs(mat.yy));
            if (sc < 0.001f) sc = 1.0f;
            float hs = 5.0f / sc; // handle size in image coords (~5px screen)

            // Dashed bounding box
            cairo_save(cr);
            cairo_set_source_rgba(cr, 0.3, 0.5, 0.9, 0.8);
            cairo_set_line_width(cr, 1.5f / sc);
            const double dashes[] = {5.0 / sc, 4.0 / sc};
            cairo_set_dash(cr, dashes, 2, 0);
            cairo_rectangle(cr, bx, by, bw, bh);
            cairo_stroke(cr);
            cairo_set_dash(cr, nullptr, 0, 0);

            // 8 handles: TL, TR, BL, BR, T, R, B, L
            float cx = bx + bw * 0.5f;
            float cy = by + bh * 0.5f;
            float hx[8] = {bx, bx+bw, bx, bx+bw, cx, bx+bw, cx, bx};
            float hy[8] = {by, by,   by+bh, by+bh, by, cy,   by+bh, cy};

            for (int i = 0; i < 8; i++) {
                cairo_set_source_rgba(cr, 1, 1, 1, 0.95);
                cairo_rectangle(cr, hx[i] - hs, hy[i] - hs, hs * 2, hs * 2);
                cairo_fill(cr);
                cairo_set_source_rgba(cr, 0.3, 0.5, 0.9, 0.9);
                cairo_set_line_width(cr, 1.0f / sc);
                cairo_rectangle(cr, hx[i] - hs, hy[i] - hs, hs * 2, hs * 2);
                cairo_stroke(cr);
            }
            cairo_restore(cr);
        }
    }
}

void App::toggle_markup() {
    if (markup_active_) {
        cancel_markup();
        return;
    }
    if (decoded_image_.width <= 0) return;
    if (crop_active_) cancel_crop();
    markup_active_ = true;
    markup_layer_panel_open_ = true;
    markup_selected_idx_ = -1;
    markup_tool_ = MarkupTool::kPen;
    markup_color_ = 0xFF0000FF;
    markup_thickness_ = 3.0f;
    numbered_count_ = 0;
    markup_text_editing_ = false;
    markup_text_input_.clear();
    markup_font_size_ = 32.0f;
    markup_font_family_ = "sans-serif";
    markup_font_dropdown_open_ = false;
    markup_text_shadow_ = false;
    markup_text_outline_ = false;
    markup_line_spacing_ = 1.3f;
    markup_current_.reset();
    render();
}

void App::commit_markup() {
    if (!markup_active_) return;
    finalize_text_();
    if (markup_current_) {
        markup_elements_.push_back(std::move(*markup_current_));
        markup_current_.reset();
    }
    markup_active_ = false;
    markup_layer_panel_open_ = false;
    markup_selected_idx_ = -1;
    image_modified_ = true;
    render();
}

void App::cancel_markup() {
    markup_active_ = false;
    markup_layer_panel_open_ = false;
    markup_selected_idx_ = -1;
    markup_current_.reset();
    markup_elements_.clear();
    markup_redo_stack_.clear();
    markup_text_editing_ = false;
    markup_text_input_.clear();
    image_modified_ = false;
    render();
}

void App::undo_markup() {
    if (!markup_elements_.empty()) {
        int sel = markup_selected_idx_;
        markup_redo_stack_.push_back(std::move(markup_elements_.back()));
        markup_elements_.pop_back();
        if (sel >= (int)markup_elements_.size())
            markup_selected_idx_ = (int)markup_elements_.size() - 1;
        render();
    }
}

void App::add_image_layer(const std::string& path, double win_x, double win_y) {
    FILE* f = fopen(path.c_str(), "rb");
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

    auto result = decoders_.decode(buf.data(), buf.size(), 0, 0);
    if (result.pixels.empty()) return;

    auto el = std::make_unique<MarkupElement>();
    el->type = MarkupTool::kImage;
    el->image_data = std::make_shared<DecodedImage>();
    el->image_data->rgba = std::move(result.pixels);
    el->image_data->width = result.width;
    el->image_data->height = result.height;
    el->image_data->stride = result.width * 4;

    // Position at drop point in image coords
    int img_x, img_y;
    win_to_img((int)win_x, (int)win_y, img_x, img_y);
    float scale = 0.3f; // Scale to 30% of original image width
    float aspect = (float)result.height / (float)result.width;
    float w = std::min((float)decoded_image_.width * scale, (float)result.width);
    float h = w * aspect;
    if (h > (float)decoded_image_.height * scale) {
        h = (float)decoded_image_.height * scale;
        w = h / aspect;
    }
    el->rect_x = (float)img_x - w / 2;
    el->rect_y = (float)img_y - h / 2;
    el->rect_w = w;
    el->rect_h = h;

    markup_elements_.push_back(*el);
    markup_selected_idx_ = (int)markup_elements_.size() - 1;
    markup_current_ = std::move(el);
    render();
}

void App::finalize_text_() {
    if (markup_text_editing_ && markup_current_ &&
        markup_current_->type == MarkupTool::kText) {
        markup_current_->text = markup_text_input_;
        if (markup_selected_idx_ >= 0 &&
            markup_selected_idx_ < (int)markup_elements_.size()) {
            // Drag-created: sync to existing element
            markup_elements_[markup_selected_idx_].text = markup_current_->text;
        } else if (!markup_current_->text.empty()) {
            // Double-click edit: push back
            if (markup_editing_original_idx_ >= 0 &&
                markup_editing_original_idx_ <= (int)markup_elements_.size()) {
                markup_elements_.insert(
                    markup_elements_.begin() + markup_editing_original_idx_,
                    std::move(*markup_current_));
            } else {
                markup_elements_.push_back(std::move(*markup_current_));
            }
        }
        markup_current_.reset();
        markup_text_editing_ = false;
        markup_text_input_.clear();
        markup_editing_original_idx_ = -1;
        markup_selected_idx_ = -1;
    }
}

// --- Layer panel rendering ---
void App::draw_markup_layer_panel(cairo_t* cr, int win_w, int win_h,
                                  std::vector<OverlayButton>& buttons) {
    int pw = 220;
    int px = win_w - pw;
    int py = Overlay::kToolbarHeight;
    int ph = win_h - py;

    // Panel background
    cairo_set_source_rgba(cr, m3::surface_r, m3::surface_g, m3::surface_b, 0.95);
    cairo_rectangle(cr, px, py, pw, ph);
    cairo_fill(cr);

    // Left outline
    cairo_set_source_rgba(cr, m3::outline_r, m3::outline_g, m3::outline_b, 0.4);
    cairo_set_line_width(cr, 1);
    cairo_move_to(cr, px, py);
    cairo_line_to(cr, px, win_h);
    cairo_stroke(cr);

    // Header
    int header_h = 40;
    cairo_set_source_rgba(cr, m3::on_surface_r, m3::on_surface_g, m3::on_surface_b, 0.87);
    cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, 14);
    cairo_move_to(cr, px + 12, py + 26);
    cairo_show_text(cr, "Layers");

    // Close button
    int close_x = px + pw - 32;
    int close_y = py + 4;
    cairo_set_source_rgba(cr, m3::on_surface_r, m3::on_surface_g, m3::on_surface_b, 0.6);
    cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 16);
    cairo_move_to(cr, close_x + 8, close_y + 22);
    cairo_show_text(cr, "\u2715");
    buttons.push_back({close_x, close_y, 32, 32, "MLayerPanelClose", {}, {}});

    // Separator
    cairo_set_source_rgba(cr, m3::outline_variant_r, m3::outline_variant_g,
                          m3::outline_variant_b, 0.5);
    cairo_move_to(cr, px + 8, py + header_h);
    cairo_line_to(cr, px + pw - 8, py + header_h);
    cairo_set_line_width(cr, 1);
    cairo_stroke(cr);

    if (markup_elements_.empty()) {
        cairo_set_source_rgba(cr, m3::on_surface_r, m3::on_surface_g,
                              m3::on_surface_b, 0.4);
        cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL,
                               CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(cr, 12);
        const char* msg = "No elements";
        cairo_text_extents_t te;
        cairo_text_extents(cr, msg, &te);
        cairo_move_to(cr, px + (pw - (int)te.width) / 2, py + header_h + 30);
        cairo_show_text(cr, msg);
        return;
    }

    int item_h = 36;
    int list_y = py + header_h + 4;
    int max_visible = (ph - header_h - 4) / item_h;
    int count = std::min((int)markup_elements_.size(), max_visible);

    for (int i = 0; i < count; i++) {
        int iy = list_y + i * item_h;
        const auto& el = markup_elements_[i];

        // Background highlight for selected element
        if (i == markup_selected_idx_) {
            cairo_set_source_rgba(cr, m3::primary_r, m3::primary_g, m3::primary_b, 0.12);
            cairo_rectangle(cr, px + 4, iy, pw - 8, item_h);
            cairo_fill(cr);
        } else if (i == markup_layer_hover_idx_) {
            cairo_set_source_rgba(cr, m3::on_surface_variant_r, m3::on_surface_variant_g,
                                  m3::on_surface_variant_b, 0.08);
            cairo_rectangle(cr, px + 4, iy, pw - 8, item_h);
            cairo_fill(cr);
        }

        // Type icon / short label
        cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL,
                               CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(cr, 11);

        const char* type_label = "";
        switch (el.type) {
        case MarkupTool::kPen:       type_label = "\u270E"; break;
        case MarkupTool::kLine:      type_label = "\u2015"; break;
        case MarkupTool::kArrow:     type_label = "\u2192"; break;
        case MarkupTool::kRect:      type_label = "\u25A1"; break;
        case MarkupTool::kEllipse:   type_label = "\u25CB"; break;
        case MarkupTool::kText:      type_label = "T";     break;
        case MarkupTool::kNumbered:  type_label = "#";     break;
        default:                     type_label = "?";     break;
        }

        cairo_set_source_rgba(cr, m3::on_surface_r, m3::on_surface_g,
                              m3::on_surface_b, el.visible ? 0.87 : 0.4);
        cairo_move_to(cr, px + 10, iy + 23);
        cairo_show_text(cr, type_label);

        // Element name
        std::string name;
        switch (el.type) {
        case MarkupTool::kPen:       name = "Pen"; break;
        case MarkupTool::kLine:      name = "Line"; break;
        case MarkupTool::kArrow:     name = "Arrow"; break;
        case MarkupTool::kRect:      name = "Rectangle"; break;
        case MarkupTool::kEllipse:   name = "Ellipse"; break;
        case MarkupTool::kText: {
            name = "Text";
            if (!el.text.empty()) {
                std::string preview = el.text;
                if (preview.size() > 8) preview = preview.substr(0, 8) + "\u2026";
                name += " \"" + preview + "\"";
            }
            break;
        }
        case MarkupTool::kNumbered:  name = "Marker " + el.text; break;
        default:                     name = "Unknown"; break;
        }

        cairo_set_font_size(cr, 12);
        cairo_text_extents_t te;
        cairo_text_extents(cr, name.c_str(), &te);
        if (te.width > pw - 80) {
            while (name.size() > 2) {
                std::string test = name.substr(0, name.size() - 1) + "\u2026";
                cairo_text_extents(cr, test.c_str(), &te);
                if (te.width <= pw - 80) {
                    name = test;
                    break;
                }
                name.pop_back();
            }
        }
        cairo_move_to(cr, px + 30, iy + 23);
        cairo_show_text(cr, name.c_str());

        // Visibility toggle
        int vis_x = px + pw - 54;
        int vis_y = iy + 6;
        cairo_set_source_rgba(cr, m3::on_surface_r, m3::on_surface_g,
                              m3::on_surface_b, el.visible ? 0.6 : 0.3);
        cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL,
                               CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(cr, 14);
        cairo_move_to(cr, vis_x + 4, vis_y + 18);
        cairo_show_text(cr, el.visible ? "\u25C9" : "\u25CE");
        buttons.push_back({vis_x, vis_y, 22, 24, "MLayerVis_" + std::to_string(i), {}, {}});

        // Delete button
        int del_x = px + pw - 28;
        int del_y = iy + 6;
        cairo_set_source_rgba(cr, 1.0, 0.3, 0.3, 0.7);
        cairo_set_font_size(cr, 14);
        cairo_move_to(cr, del_x + 5, del_y + 18);
        cairo_show_text(cr, "\u2715");
        buttons.push_back({del_x, del_y, 22, 24, "MLayerDel_" + std::to_string(i), {}, {}});

        // Click to select (whole row)
        buttons.push_back({px + 4, iy, pw - 8, item_h, "MLayerSel_" + std::to_string(i), {}, {}});
    }

    // Reorder buttons at bottom
    int reorder_y = py + ph - 80;
    cairo_set_source_rgba(cr, m3::outline_variant_r, m3::outline_variant_g,
                          m3::outline_variant_b, 0.4);
    cairo_move_to(cr, px + 8, reorder_y - 4);
    cairo_line_to(cr, px + pw - 8, reorder_y - 4);
    cairo_set_line_width(cr, 1);
    cairo_stroke(cr);

    cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL,
                           CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 12);

    // Move up button
    int up_x = px + 12;
    int up_y = reorder_y + 4;
    int nav_w = pw / 2 - 16;
    int nav_h = 30;
    cairo_set_source_rgba(cr, m3::surface_container_high_r, m3::surface_container_high_g,
                          m3::surface_container_high_b, 0.9);
    overlay_.draw_rounded_rect(cr, up_x, up_y, nav_w, nav_h, 6);
    cairo_fill(cr);
    cairo_set_source_rgba(cr, m3::on_surface_r, m3::on_surface_g,
                          m3::on_surface_b, 0.87);
    cairo_move_to(cr, up_x + nav_w / 2 - 6, up_y + 20);
    cairo_show_text(cr, "\u25B2");
    buttons.push_back({up_x, up_y, nav_w, nav_h, "MLayerUp", {}, {}});

    // Move down button
    int dn_x = px + pw / 2 + 4;
    int dn_y = reorder_y + 4;
    cairo_set_source_rgba(cr, m3::surface_container_high_r, m3::surface_container_high_g,
                          m3::surface_container_high_b, 0.9);
    overlay_.draw_rounded_rect(cr, dn_x, dn_y, nav_w, nav_h, 6);
    cairo_fill(cr);
    cairo_set_source_rgba(cr, m3::on_surface_r, m3::on_surface_g,
                          m3::on_surface_b, 0.87);
    cairo_move_to(cr, dn_x + nav_w / 2 - 6, dn_y + 20);
    cairo_show_text(cr, "\u25BC");
    buttons.push_back({dn_x, dn_y, nav_w, nav_h, "MLayerDown", {}, {}});
}

}
