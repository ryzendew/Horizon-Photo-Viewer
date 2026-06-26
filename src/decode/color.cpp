#include "decode/decoder.hpp"
#include <lcms2.h>
#include <vector>
#include <cstdint>
#include <iostream>

namespace hpv {

static bool apply_icc_to_srgb(std::vector<uint8_t>& pixels, int width, int height,
                               const std::vector<uint8_t>& icc_profile) {
    if (pixels.empty() || icc_profile.empty() || width <= 0 || height <= 0) return false;

    cmsHPROFILE src = cmsOpenProfileFromMem(icc_profile.data(), (cmsUInt32Number)icc_profile.size());
    if (!src) {
        std::cerr << "lcms: failed to open ICC profile\n";
        return false;
    }

    cmsHPROFILE dst = cmsCreate_sRGBProfile();
    if (!dst) {
        cmsCloseProfile(src);
        return false;
    }

    cmsHTRANSFORM xform = cmsCreateTransform(
        src, TYPE_RGBA_8,
        dst, TYPE_RGBA_8,
        INTENT_PERCEPTUAL, 0
    );

    if (!xform) {
        std::cerr << "lcms: failed to create transform\n";
        cmsCloseProfile(dst);
        cmsCloseProfile(src);
        return false;
    }

    cmsDoTransform(xform, pixels.data(), pixels.data(),
                   (cmsUInt32Number)(width * height));

    cmsDeleteTransform(xform);
    cmsCloseProfile(dst);
    cmsCloseProfile(src);
    return true;
}

void apply_color_management(DecodeResult& result) {
    if (result.icc_profile.empty()) return;
    apply_icc_to_srgb(result.pixels, result.width, result.height, result.icc_profile);
}

}
