#include "decode/decoder.hpp"
#include "decode/svg_render.hpp"

#include <nanosvg/nanosvg.h>
#include <nanosvg/nanosvgrast.h>

#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include <stb/stb_image.h>

namespace hpv {

// ---- Base64 decode (skips non-alphabet chars like XML entities) ----

static std::vector<uint8_t> base64_decode(const char* data, size_t len) {
    static const signed char kTable[256] = {
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,62,-1,-1,-1,63,
        52,53,54,55,56,57,58,59,60,61,-1,-1,-1,-1,-1,-1,
        -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,
        15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,-1,
        -1,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,
        41,42,43,44,45,46,47,48,49,50,51,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    };
    std::vector<uint8_t> out;
    out.reserve(len * 3 / 4 + 4);
    int buf = 0;
    int bits = 0;
    for (size_t i = 0; i < len; i++) {
        int c = kTable[(unsigned char)data[i]];
        if (c < 0) continue;
        buf = (buf << 6) | c;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back((buf >> bits) & 0xff);
        }
    }
    return out;
}

// ---- SvgDecoder (raster fallback) ----

bool SvgDecoder::can_decode(const uint8_t* data, size_t size) {
    if (size < 4) return false;
    if (data[0] == '<' && data[1] == 's' && data[2] == 'v' && data[3] == 'g') return true;
    if (data[0] == '<' && data[1] == '?' && data[2] == 'x' && data[3] == 'm') return true;
    if (data[0] == '<' && data[1] == 'S' && data[2] == 'V' && data[3] == 'G') return true;
    return false;
}

DecodeResult SvgDecoder::decode(const uint8_t* data, size_t size,
                                 int target_width, int target_height) {
    DecodeResult result;
    if (size == 0) return result;

    std::string svg_str(reinterpret_cast<const char*>(data), size);

    NSVGimage* img = nsvgParse(const_cast<char*>(svg_str.c_str()), "px", 96);
    if (!img) {
        std::cerr << "SVG: nsvgParse failed\n";
        return result;
    }

    int w, h;
    if (target_width > 0 && target_height > 0) {
        w = target_width;
        h = target_height;
    } else {
        w = (int)img->width;
        h = (int)img->height;
        if (w <= 0 || h <= 0) { w = 512; h = 512; }
    }
    if (w > 4096 || h > 4096) {
        float scale = 4096.0f / (float)std::max(w, h);
        w = (int)(w * scale);
        h = (int)(h * scale);
    }

    float scale = (float)w / img->width;
    if ((float)h / img->height < scale) scale = (float)h / img->height;

    size_t npix = (size_t)w * h;
    std::vector<uint8_t> rgba(npix * 4, 0);

    NSVGrasterizer* rast = nsvgCreateRasterizer();
    if (!rast) {
        std::cerr << "SVG: nsvgCreateRasterizer failed\n";
        nsvgDelete(img);
        return result;
    }

    nsvgRasterize(rast, img, 0, 0, scale, rgba.data(), w, h, w * 4);
    nsvgDeleteRasterizer(rast);
    nsvgDelete(img);

    result.width = w;
    result.height = h;
    result.pixels = std::move(rgba);
    result.format_name = "SVG";
    return result;
}

// ---- SVG parsing ----

NSVGimage* svg_parse(const uint8_t* data, size_t size) {
    if (!data || size == 0) return nullptr;
    std::string svg_str(reinterpret_cast<const char*>(data), size);
    return nsvgParse(const_cast<char*>(svg_str.c_str()), "px", 96);
}

// ---- Vector rendering (with fill-batch optimisation) ----

void render_svg_cairo(cairo_t* cr, const NSVGimage* img,
                       float target_w, float target_h) {
    if (!img || img->width <= 0 || img->height <= 0) return;

    float scale = target_w / img->width;
    float sy = target_h / img->height;
    if (sy < scale) scale = sy;

    cairo_save(cr);
    cairo_scale(cr, scale, scale);

    // Batch state for consecutive fill-only solid-colour shapes
    bool batch_active = false;
    unsigned int batch_color = 0;
    int batch_fill_rule = 0;
    float batch_opacity = 1.0f;

    for (const NSVGshape* shape = img->shapes; shape; shape = shape->next) {
        if (!(shape->flags & NSVG_FLAGS_VISIBLE)) continue;

        bool do_fill = shape->fill.type == NSVG_PAINT_COLOR ||
                       shape->fill.type == NSVG_PAINT_LINEAR_GRADIENT ||
                       shape->fill.type == NSVG_PAINT_RADIAL_GRADIENT;
        bool do_stroke = (shape->stroke.type == NSVG_PAINT_COLOR ||
                          shape->stroke.type == NSVG_PAINT_LINEAR_GRADIENT ||
                          shape->stroke.type == NSVG_PAINT_RADIAL_GRADIENT) &&
                         shape->strokeWidth > 0.0f;

        if (!do_fill && !do_stroke) continue;

        // Can this shape be merged into the current fill batch?
        bool can_batch = do_fill && !do_stroke &&
                         shape->fill.type == NSVG_PAINT_COLOR;

        if (can_batch) {
            unsigned int c = shape->fill.color;
            int fr = shape->fillRule;
            float op = shape->opacity;

            if (batch_active &&
                (c != batch_color || fr != batch_fill_rule || op != batch_opacity)) {
                cairo_fill(cr);
                batch_active = false;
            }

            if (!batch_active) {
                batch_active = true;
                batch_color = c;
                batch_fill_rule = fr;
                batch_opacity = op;
                float fa = ((c >> 24) & 0xff) / 255.0f * op;
                cairo_set_source_rgba(cr,
                    (c & 0xff) / 255.0,
                    ((c >> 8) & 0xff) / 255.0,
                    ((c >> 16) & 0xff) / 255.0,
                    fa);
                cairo_set_fill_rule(cr, fr == NSVG_FILLRULE_EVENODD
                    ? CAIRO_FILL_RULE_EVEN_ODD : CAIRO_FILL_RULE_WINDING);
                cairo_new_path(cr);
            }

            for (const NSVGpath* path = shape->paths; path; path = path->next) {
                cairo_move_to(cr, path->pts[0], path->pts[1]);
                for (int i = 0; i < path->npts - 1; i += 3) {
                    const float* p = &path->pts[i * 2];
                    cairo_curve_to(cr, p[2], p[3], p[4], p[5], p[6], p[7]);
                }
                if (path->closed) cairo_close_path(cr);
            }
            continue;
        }

        // Flush any active batch before a non-batchable shape
        if (batch_active) {
            cairo_fill(cr);
            batch_active = false;
        }

        // Build path
        for (const NSVGpath* path = shape->paths; path; path = path->next) {
            cairo_move_to(cr, path->pts[0], path->pts[1]);
            for (int i = 0; i < path->npts - 1; i += 3) {
                const float* p = &path->pts[i * 2];
                cairo_curve_to(cr, p[2], p[3], p[4], p[5], p[6], p[7]);
            }
            if (path->closed) cairo_close_path(cr);
        }

        // Fill
        if (do_fill) {
            if (shape->fill.type == NSVG_PAINT_COLOR) {
                unsigned int c = shape->fill.color;
                double fa = ((c >> 24) & 0xff) / 255.0 * shape->opacity;
                cairo_set_source_rgba(cr,
                    (c & 0xff) / 255.0,
                    ((c >> 8) & 0xff) / 255.0,
                    ((c >> 16) & 0xff) / 255.0,
                    fa);
            } else if (shape->fill.type == NSVG_PAINT_LINEAR_GRADIENT) {
                const NSVGgradient* g = shape->fill.gradient;
                if (g && g->nstops > 0) {
                    cairo_pattern_t* pat = cairo_pattern_create_linear(0, 0, 0, 1);
                    cairo_matrix_t mat;
                    cairo_matrix_init(&mat, g->xform[0], g->xform[1],
                                            g->xform[2], g->xform[3],
                                            g->xform[4], g->xform[5]);
                    cairo_pattern_set_matrix(pat, &mat);
                    for (int i = 0; i < g->nstops; i++) {
                        unsigned int sc = g->stops[i].color;
                        cairo_pattern_add_color_stop_rgba(pat, g->stops[i].offset,
                            (sc & 0xff) / 255.0,
                            ((sc >> 8) & 0xff) / 255.0,
                            ((sc >> 16) & 0xff) / 255.0,
                            ((sc >> 24) & 0xff) / 255.0);
                    }
                    cairo_set_source(cr, pat);
                    cairo_pattern_destroy(pat);
                } else {
                    do_fill = false;
                }
            } else if (shape->fill.type == NSVG_PAINT_RADIAL_GRADIENT) {
                const NSVGgradient* g = shape->fill.gradient;
                if (g && g->nstops > 0) {
                    cairo_pattern_t* pat = cairo_pattern_create_radial(
                        g->fx, g->fy, 0, 0, 0, 1);
                    cairo_matrix_t mat;
                    cairo_matrix_init(&mat, g->xform[0], g->xform[1],
                                            g->xform[2], g->xform[3],
                                            g->xform[4], g->xform[5]);
                    cairo_pattern_set_matrix(pat, &mat);
                    for (int i = 0; i < g->nstops; i++) {
                        unsigned int sc = g->stops[i].color;
                        cairo_pattern_add_color_stop_rgba(pat, g->stops[i].offset,
                            (sc & 0xff) / 255.0,
                            ((sc >> 8) & 0xff) / 255.0,
                            ((sc >> 16) & 0xff) / 255.0,
                            ((sc >> 24) & 0xff) / 255.0);
                    }
                    cairo_set_source(cr, pat);
                    cairo_pattern_destroy(pat);
                } else {
                    do_fill = false;
                }
            }

            if (do_fill) {
                cairo_set_fill_rule(cr, shape->fillRule == NSVG_FILLRULE_EVENODD
                    ? CAIRO_FILL_RULE_EVEN_ODD : CAIRO_FILL_RULE_WINDING);
                cairo_fill_preserve(cr);
            }
        }

        // Stroke
        if (do_stroke) {
            if (shape->stroke.type == NSVG_PAINT_COLOR) {
                unsigned int c = shape->stroke.color;
                double sa = ((c >> 24) & 0xff) / 255.0 * shape->opacity;
                cairo_set_source_rgba(cr,
                    (c & 0xff) / 255.0,
                    ((c >> 8) & 0xff) / 255.0,
                    ((c >> 16) & 0xff) / 255.0,
                    sa);
                cairo_set_line_width(cr, shape->strokeWidth);
                switch (shape->strokeLineCap) {
                    case NSVG_CAP_BUTT:   cairo_set_line_cap(cr, CAIRO_LINE_CAP_BUTT); break;
                    case NSVG_CAP_ROUND:  cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND); break;
                    case NSVG_CAP_SQUARE: cairo_set_line_cap(cr, CAIRO_LINE_CAP_SQUARE); break;
                }
                switch (shape->strokeLineJoin) {
                    case NSVG_JOIN_MITER: cairo_set_line_join(cr, CAIRO_LINE_JOIN_MITER); break;
                    case NSVG_JOIN_ROUND: cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND); break;
                    case NSVG_JOIN_BEVEL: cairo_set_line_join(cr, CAIRO_LINE_JOIN_BEVEL); break;
                }
                cairo_set_miter_limit(cr, shape->miterLimit);
                if (shape->strokeDashCount > 0) {
                    double dashes[8];
                    for (int i = 0; i < shape->strokeDashCount; i++)
                        dashes[i] = shape->strokeDashArray[i];
                    cairo_set_dash(cr, dashes, shape->strokeDashCount, shape->strokeDashOffset);
                }
                cairo_stroke(cr);
            } else {
                do_stroke = false;
            }
        }

        if (!do_stroke && do_fill)
            cairo_new_path(cr);
    }

    // Flush any remaining batch
    if (batch_active)
        cairo_fill(cr);

    cairo_restore(cr);

    // Debug: count shapes and total paths in this image
    int shape_count = 0, path_count = 0, fill_batch_count = 0;
    for (const NSVGshape* shape = img->shapes; shape; shape = shape->next) {
        if (!(shape->flags & NSVG_FLAGS_VISIBLE)) continue;
        shape_count++;
        bool can_batch = (shape->fill.type == NSVG_PAINT_COLOR) &&
                         !((shape->stroke.type == NSVG_PAINT_COLOR ||
                            shape->stroke.type == NSVG_PAINT_LINEAR_GRADIENT ||
                            shape->stroke.type == NSVG_PAINT_RADIAL_GRADIENT) &&
                           shape->strokeWidth > 0.0f);
        if (can_batch) fill_batch_count++;
        for (const NSVGpath* p = shape->paths; p; p = p->next)
            path_count++;
    }
    std::cerr << "[svg] shapes=" << shape_count
              << " paths=" << path_count
              << " batchable=" << fill_batch_count
              << "\n";
}

// ---- Embedded image extraction ----

EmbeddedImage extract_embedded_image(const uint8_t* svg_data, size_t svg_size) {
    EmbeddedImage result;
    if (!svg_data || svg_size == 0) return result;

    std::string svg_str(reinterpret_cast<const char*>(svg_data), svg_size);
    const char* s = svg_str.c_str();

    // Find the first <image tag
    const char* img_start = std::strstr(s, "<image");
    if (!img_start) {
        std::cerr << "[svg] debug: <image tag not found\n";
        return result;
    }

    // Locate the closing > of this tag
    const char* tag_end = std::strchr(img_start, '>');
    if (!tag_end) {
        std::cerr << "[svg] debug: image tag > not found\n";
        return result;
    }

    // Debug: show the tag region
    ptrdiff_t tag_len = tag_end - img_start + 1;
    std::string tag_snippet(img_start, std::min<ptrdiff_t>(tag_len, 200));
    std::cerr << "[svg] debug: image tag starts with: "
              << tag_snippet.substr(0, 120) << "\n";

    // Helper: extract an attribute value double-quoted within [img_start, tag_end]
    auto get_attr = [&](const char* name) -> std::string {
        std::string pat = name;
        pat += "=\"";
        const char* a = std::strstr(img_start, pat.c_str());
        if (!a || a > tag_end) return {};
        a += pat.size();
        const char* b = std::strchr(a, '"');
        if (!b || b > tag_end) return {};
        return std::string(a, b - a);
    };

    std::string href = get_attr("xlink:href");
    if (href.empty()) {
        // Try without xlink: prefix
        href = get_attr("href");
        if (href.empty()) {
            std::cerr << "[svg] debug: xlink:href not found in image tag\n";
            // Search for 'href=' anywhere in the tag
            const char* h = std::strstr(img_start, "href=");
            if (h && h < tag_end)
                std::cerr << "[svg] debug: found 'href=' at offset "
                          << (h - img_start) << "\n";
            return result;
        }
        std::cerr << "[svg] debug: using href fallback\n";
    }

    // Must be a base64-encoded PNG
    const char kPrefix[] = "data:image/png;base64,";
    auto pos = href.find(kPrefix);
    if (pos == std::string::npos) {
        std::cerr << "[svg] debug: href doesn't start with base64 PNG prefix: "
                  << href.substr(0, 120) << "\n";
        return result;
    }
    std::cerr << "[svg] debug: href len=" << href.size()
              << " ends_with=\""
              << (href.size() > 20 ? href.substr(href.size() - 20) : href)
              << "\"\n";

    std::string b64 = href.substr(pos + sizeof(kPrefix) - 1);

    std::cerr << "[svg] debug: b64_pre=" << b64.size()
              << " ends=" << (b64.size() > 20 ? b64.substr(b64.size() - 20) : b64)
              << "\n";

    // Strip XML entity references (e.g. &#10;) that corrupt base64 decoding
    {
        std::string clean;
        clean.reserve(b64.size());
        for (size_t i = 0; i < b64.size(); ) {
            if (b64[i] == '&') {
                while (i < b64.size() && b64[i] != ';') i++;
                if (i < b64.size()) i++; // skip ';'
            } else {
                clean += b64[i];
                i++;
            }
        }
        b64 = std::move(clean);
    }

    std::vector<uint8_t> png_bytes = base64_decode(b64.data(), b64.size());
    if (png_bytes.empty()) {
        std::cerr << "[svg] debug: base64 decode returned empty\n";
        return result;
    }

    // Decode PNG via stb_image
    int iw = 0, ih = 0, ch = 0;
    // Check for leftover entities in base64
    {
        bool has_entities = false;
        for (size_t i = 0; i + 1 < b64.size(); i++) {
            if (b64[i] == '&') { has_entities = true; break; }
        }
        std::cerr << "[svg] debug: b64 size=" << b64.size()
                  << " leftover_entities=" << (has_entities ? "YES" : "no")
                  << " first_16=[";
        for (int i = 0; i < 16 && i < (int)b64.size(); i++)
            std::cerr << (char)b64[i];
        std::cerr << "]\n";
    }
    // Debug: write PNG to file for inspection
    {
        FILE* pf = fopen("/tmp/embedded_test.png", "wb");
        if (pf) {
            fwrite(png_bytes.data(), 1, png_bytes.size(), pf);
            fclose(pf);
        }
        std::cerr << "[svg] debug: png_bytes=" << png_bytes.size()
                  << " first_hex=";
        for (int i = 0; i < 8 && i < (int)png_bytes.size(); i++)
            std::cerr << std::hex << (int)png_bytes[i] << " ";
        std::cerr << std::dec << "\n";
    }

    unsigned char* pixels = stbi_load_from_memory(
        png_bytes.data(), (int)png_bytes.size(), &iw, &ih, &ch, 4);
    if (!pixels) {
        std::cerr << "[svg] debug: stbi_load_from_memory failed ("
                  << png_bytes.size() << " bytes)\n";
        return result;
    }

    // Convert RGBA → BGRA for Cairo
    result.bgra.resize((size_t)iw * ih * 4);
    for (int i = 0; i < iw * ih; i++) {
        result.bgra[i * 4 + 0] = pixels[i * 4 + 2];
        result.bgra[i * 4 + 1] = pixels[i * 4 + 1];
        result.bgra[i * 4 + 2] = pixels[i * 4 + 0];
        result.bgra[i * 4 + 3] = pixels[i * 4 + 3];
    }
    stbi_image_free(pixels);

    result.img_w = iw;
    result.img_h = ih;

    // Parse placement attributes (safe defaults if missing)
    try {
        std::string v;
        v = get_attr("x"); if (!v.empty()) result.x = std::stod(v);
        v = get_attr("y"); if (!v.empty()) result.y = std::stod(v);
        v = get_attr("width");  if (!v.empty()) result.w = std::stod(v);
        v = get_attr("height"); if (!v.empty()) result.h = std::stod(v);
    } catch (...) {}

    if (result.w <= 0) result.w = (double)iw;
    if (result.h <= 0) result.h = (double)ih;

    return result;
}

} // namespace hpv
