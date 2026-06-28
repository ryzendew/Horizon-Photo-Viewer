#pragma once

#include <string>

namespace hpv {

struct Config {
    static Config load();
    static bool save(const Config& c);
    static std::string config_path();

    int slideshow_interval_ms = 5000;
    bool show_overlay = false;
    bool enable_color_management = true;
    float default_zoom = 1.0f;
    float bg_alpha = 1.0f;
    std::string theme = "dark";
    static constexpr auto kDefaultImgurClientId = "a0691805609650e";
    std::string imgur_client_id = kDefaultImgurClientId;
};

}
