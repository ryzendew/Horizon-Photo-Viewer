#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace hpv {

class ThumbCache {
public:
    static std::string cache_dir();
    static std::string cache_path(const std::string& filepath);

    // Save RGBA pixels as WebP to disk cache. Returns true on success.
    static bool save(const std::string& filepath, const uint8_t* rgba,
                     int w, int h);

    // Load from disk cache. Returns true and fills out/rgba on hit.
    static bool load(const std::string& filepath, std::vector<uint8_t>& rgba,
                     int& out_w, int& out_h);

private:
    static uint64_t fnv1a(const std::string& s);
    static uint64_t file_mtime(const std::string& path);
};

} // namespace hpv
