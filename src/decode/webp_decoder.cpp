#include "decode/decoder.hpp"
#include <webp/decode.h>
#include <cstring>
#include <iostream>

namespace hpv {

static bool is_webp_magic(const uint8_t* data, size_t size) {
    if (size < 12) return false;
    return data[0] == 'R' && data[1] == 'I' && data[2] == 'F' && data[3] == 'F' &&
           data[8] == 'W' && data[9] == 'E' && data[10] == 'B' && data[11] == 'P';
}

bool WebPDecoder::can_decode(const uint8_t* data, size_t size) {
    return is_webp_magic(data, size);
}

DecodeResult WebPDecoder::decode(const uint8_t* data, size_t size) {
    DecodeResult result;
    result.format_name = "WebP";

    int w = 0, h = 0;
    uint8_t* rgba = WebPDecodeRGBA(data, size, &w, &h);
    if (!rgba) {
        std::cerr << "WebP decode failed\n";
        return result;
    }

    result.width = w;
    result.height = h;
    result.pixels.resize((size_t)w * h * 4);
    std::memcpy(result.pixels.data(), rgba, (size_t)w * h * 4);
    WebPFree(rgba);

    return result;
}

}
