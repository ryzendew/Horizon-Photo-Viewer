#include "decode/decoder.hpp"

#define STB_IMAGE_IMPLEMENTATION
#include <stb/stb_image.h>

#include <cstring>
#include <iostream>

namespace hpv {

// Magic byte detection
namespace {

bool match_magic(const uint8_t* data, size_t size, const uint8_t* magic, size_t magic_len) {
    if (size < magic_len) return false;
    return std::memcmp(data, magic, magic_len) == 0;
}

#ifndef HAVE_LIBJPEG
bool is_jpeg(const uint8_t* data, size_t size) {
    const uint8_t jpeg_magic[] = {0xFF, 0xD8, 0xFF};
    return match_magic(data, size, jpeg_magic, 3);
}
#endif

bool is_png(const uint8_t* data, size_t size) {
    const uint8_t png_magic[] = {0x89, 0x50, 0x4E, 0x47};
    return match_magic(data, size, png_magic, 4);
}

bool is_gif(const uint8_t* data, size_t size) {
    const uint8_t gif_magic[] = {'G', 'I', 'F', '8'};
    return match_magic(data, size, gif_magic, 4);
}

bool is_bmp(const uint8_t* data, size_t size) {
    const uint8_t bmp_magic[] = {'B', 'M'};
    return match_magic(data, size, bmp_magic, 2);
}

#ifndef HAVE_LIBWEBP
bool is_webp(const uint8_t* data, size_t size) {
    if (size < 12) return false;
    return (data[0] == 'R' && data[1] == 'I' && data[2] == 'F' && data[3] == 'F' &&
            data[8] == 'W' && data[9] == 'E' && data[10] == 'B' && data[11] == 'P');
}
#endif

bool is_tiff(const uint8_t* data, size_t size) {
    if (size < 4) return false;
    return (data[0] == 'I' && data[1] == 'I' && data[2] == 0x2A && data[3] == 0x00) ||
           (data[0] == 'M' && data[1] == 'M' && data[2] == 0x00 && data[3] == 0x2A);
}

bool is_heif(const uint8_t* data, size_t size) {
    if (size < 12) return false;
    for (size_t i = 0; i + 8 < size; i++) {
        if (data[i] == 'f' && data[i+1] == 't' && data[i+2] == 'y' && data[i+3] == 'p') {
            if (i + 12 > size) return false;
            return (data[i+4] == 'h' && data[i+5] == 'e' && data[i+6] == 'i' && (data[i+7] == 'c' || data[i+7] == 'x')) ||
                   (data[i+4] == 'm' && data[i+5] == 'i' && data[i+6] == 'f' && data[i+7] == '1') ||
                   (data[i+4] == 'a' && data[i+5] == 'v' && data[i+6] == 'i' && data[i+7] == 'f');
        }
    }
    return false;
}

bool is_avif(const uint8_t* data, size_t size) {
    if (size < 12) return false;
    for (size_t i = 0; i + 12 <= size; i++) {
        if (data[i] == 'f' && data[i+1] == 't' && data[i+2] == 'y' && data[i+3] == 'p') {
            if (i + 12 > size) return false;
            return (data[i+4] == 'a' && data[i+5] == 'v' && data[i+6] == 'i' && data[i+7] == 'f');
        }
    }
    return false;
}

#ifndef HAVE_LIBJXL
bool is_jxl(const uint8_t* data, size_t size) {
    if (size < 12) return false;
    const uint8_t jxl_magic[] = {0x00, 0x00, 0x00, 0x0C, 0x4A, 0x58, 0x4C, 0x20, 0x0D, 0x0A, 0x87, 0x0A};
    return match_magic(data, size, jxl_magic, 12);
}
#endif

}

// --- WuffsDecoder (stub - will use Wuffs C amalgamation later) ---

bool WuffsDecoder::can_decode(const uint8_t* data, size_t size) {
    return is_png(data, size) || is_gif(data, size) || is_bmp(data, size) || is_tiff(data, size);
}

DecodeResult WuffsDecoder::decode(const uint8_t* /*data*/, size_t /*size*/) {
    return DecodeResult{};
}

// --- StbDecoder (universal fallback via stb_image.h) ---

bool StbDecoder::can_decode(const uint8_t* /*data*/, size_t /*size*/) {
    return true; // We try stb_image last for anything
}

DecodeResult StbDecoder::decode(const uint8_t* data, size_t size) {
    DecodeResult result;
    int w = 0, h = 0, channels = 0;

    unsigned char* pixels = stbi_load_from_memory(data, (int)size, &w, &h, &channels, 4);
    if (!pixels) {
        std::cerr << "stb_image: " << stbi_failure_reason() << "\n";
        return result;
    }

    result.width = w;
    result.height = h;
    result.pixels.resize((size_t)w * h * 4);
    std::memcpy(result.pixels.data(), pixels, (size_t)w * h * 4);
    stbi_image_free(pixels);
    return result;
}

#ifndef HAVE_LIBWEBP
// --- WebPDecoder (stub) ---

bool WebPDecoder::can_decode(const uint8_t* data, size_t size) {
    return is_webp(data, size);
}

DecodeResult WebPDecoder::decode(const uint8_t* /*data*/, size_t /*size*/) {
    return DecodeResult{};
}
#endif

#ifndef HAVE_LIBJPEG
// --- JpegDecoder (stub) ---

bool JpegDecoder::can_decode(const uint8_t* data, size_t size) {
    return is_jpeg(data, size);
}

DecodeResult JpegDecoder::decode(const uint8_t* /*data*/, size_t /*size*/) {
    return DecodeResult{};
}
#endif

#ifndef HAVE_LIBHEIF
// --- HeifDecoder (stub) ---

bool HeifDecoder::can_decode(const uint8_t* data, size_t size) {
    return is_heif(data, size) && !is_avif(data, size);
}

DecodeResult HeifDecoder::decode(const uint8_t* /*data*/, size_t /*size*/) {
    return DecodeResult{};
}
#endif

#ifndef HAVE_LIBAVIF
// --- AvifDecoder (stub) ---

bool AvifDecoder::can_decode(const uint8_t* data, size_t size) {
    return is_avif(data, size);
}

DecodeResult AvifDecoder::decode(const uint8_t* /*data*/, size_t /*size*/) {
    return DecodeResult{};
}
#endif

#ifndef HAVE_LIBRAW
// --- RawDecoder (stub) ---

bool RawDecoder::can_decode(const uint8_t* /*data*/, size_t /*size*/) {
    return false; // TODO: detect RAW formats
}

DecodeResult RawDecoder::decode(const uint8_t* /*data*/, size_t /*size*/) {
    return DecodeResult{};
}
#endif

#ifndef HAVE_LIBJXL
// --- JxlDecoder (stub) ---

bool JxlDecoder::can_decode(const uint8_t* data, size_t size) {
    return is_jxl(data, size);
}

DecodeResult JxlDecoder::decode(const uint8_t* /*data*/, size_t /*size*/) {
    return DecodeResult{};
}
#endif

}
