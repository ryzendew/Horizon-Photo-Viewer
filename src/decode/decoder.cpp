#include "decode/decoder.hpp"

namespace hpv {

void DecoderRegistry::register_decoder(std::unique_ptr<ImageDecoder> decoder) {
    decoders_.push_back(std::move(decoder));
}

DecodeResult DecoderRegistry::decode(const uint8_t* data, size_t size) {
    for (auto& decoder : decoders_) {
        if (decoder->can_decode(data, size)) {
            auto result = decoder->decode(data, size);
            if (!result.pixels.empty()) return result;
        }
    }
    return DecodeResult{};
}

}
