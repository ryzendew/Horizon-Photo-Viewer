#pragma once

#include <cairo.h>
#include <cstddef>
#include <cstdint>
#include <vector>

struct NSVGimage;

namespace hpv {

// Result of extracting an embedded raster image from an SVG.
struct EmbeddedImage {
    std::vector<uint8_t> bgra;  // BGRA pixels (suitable for CAIRO_FORMAT_ARGB32)
    int img_w = 0;              // decoded raster width
    int img_h = 0;              // decoded raster height
    double x = 0;               // SVG x position
    double y = 0;               // SVG y position
    double w = 0;               // SVG display width
    double h = 0;               // SVG display height

    bool valid() const { return !bgra.empty() && img_w > 0 && img_h > 0; }
};

// Parse SVG data into a NanoSVG image. The caller owns the returned pointer
// and must free it with nsvgDelete(). Returns nullptr on failure.
NSVGimage* svg_parse(const uint8_t* data, size_t size);

// Render a pre-parsed NanoSVG image directly to Cairo as vector paths.
// The SVG is drawn at its native viewbox coordinates, maintaining aspect ratio
// to fill (0,0,target_w,target_h). Caller should set up any additional transforms.
void render_svg_cairo(cairo_t* cr, const NSVGimage* img,
                       float target_w, float target_h);

// Scan raw SVG data for <image> elements with base64-encoded PNG data,
// decode the first one found, and return its BGRA pixels + placement info.
// Returns EmbeddedImage{} (invalid) if no embedded PNG is found.
EmbeddedImage extract_embedded_image(const uint8_t* svg_data, size_t svg_size);

}
