#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace hpv {

struct DecodeResult {
    std::vector<uint8_t> pixels;  // RGBA non-premultiplied
    int width = 0;
    int height = 0;
    int exif_orientation = 1;
    std::string format_name;
    std::vector<uint8_t> icc_profile;
};

class ImageDecoder {
public:
    virtual ~ImageDecoder() = default;
    virtual bool can_decode(const uint8_t* data, size_t size) = 0;
    virtual DecodeResult decode(const uint8_t* data, size_t size) = 0;
    virtual const char* name() const = 0;
};

class WuffsDecoder : public ImageDecoder {
public:
    bool can_decode(const uint8_t* data, size_t size) override;
    DecodeResult decode(const uint8_t* data, size_t size) override;
    const char* name() const override { return "Wuffs"; }
};

class StbDecoder : public ImageDecoder {
public:
    bool can_decode(const uint8_t* data, size_t size) override;
    DecodeResult decode(const uint8_t* data, size_t size) override;
    const char* name() const override { return "stb_image"; }
};

class WebPDecoder : public ImageDecoder {
public:
    bool can_decode(const uint8_t* data, size_t size) override;
    DecodeResult decode(const uint8_t* data, size_t size) override;
    const char* name() const override { return "WebP"; }
};

class JpegDecoder : public ImageDecoder {
public:
    bool can_decode(const uint8_t* data, size_t size) override;
    DecodeResult decode(const uint8_t* data, size_t size) override;
    const char* name() const override { return "JPEG"; }
};

class HeifDecoder : public ImageDecoder {
public:
    bool can_decode(const uint8_t* data, size_t size) override;
    DecodeResult decode(const uint8_t* data, size_t size) override;
    const char* name() const override { return "HEIF"; }
};

class AvifDecoder : public ImageDecoder {
public:
    bool can_decode(const uint8_t* data, size_t size) override;
    DecodeResult decode(const uint8_t* data, size_t size) override;
    const char* name() const override { return "AVIF"; }
};

class RawDecoder : public ImageDecoder {
public:
    bool can_decode(const uint8_t* data, size_t size) override;
    DecodeResult decode(const uint8_t* data, size_t size) override;
    const char* name() const override { return "RAW"; }
};

class JxlDecoder : public ImageDecoder {
public:
    bool can_decode(const uint8_t* data, size_t size) override;
    DecodeResult decode(const uint8_t* data, size_t size) override;
    const char* name() const override { return "JPEG-XL"; }
};

class DecoderRegistry {
public:
    void register_decoder(std::unique_ptr<ImageDecoder> decoder);

    DecodeResult decode(const uint8_t* data, size_t size);

private:
    std::vector<std::unique_ptr<ImageDecoder>> decoders_;
};

#ifdef HAVE_LCMS2
void apply_color_management(DecodeResult& result);
#endif

}
