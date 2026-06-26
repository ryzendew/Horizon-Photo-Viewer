#include "core/viewer/app.hpp"

#include <filesystem>
#include <iostream>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb/stb_image_write.h"

namespace fs = std::filesystem;

namespace hpv {

void App::write_png_file(const std::string& path) {
    if (decoded_image_.rgba.empty()) return;
    stbi_write_png(path.c_str(),
                   decoded_image_.width,
                   decoded_image_.height,
                   4,
                   decoded_image_.rgba.data(),
                   decoded_image_.width * 4);
    std::cout << "Saved: " << path << " ("
              << decoded_image_.width << "x" << decoded_image_.height << ")\n";
}

void App::save_image() {
    if (!image_modified_ || current_path_.empty()) return;
    write_png_file(current_path_);
    image_modified_ = false;
    update_title();
}

void App::save_as() {
    save_dialog_(false);
}

void App::save_as_copy() {
    save_dialog_(true);
}

void App::save_dialog_(bool as_copy) {
    if (decoded_image_.rgba.empty()) return;

    std::string parent_handle = "wayland:wl_surface@";
    uint32_t surface_id = wl_proxy_get_id((struct wl_proxy*)surface_);
    parent_handle += std::to_string(surface_id);

    fs::path current(current_path_);
    std::string suggested = current.empty() ? "image.png" : current.filename().string();
    std::string folder;
    if (!current.empty()) {
        folder = "file://" + current.parent_path().string();
    }

    portal_dialog_.save_file(parent_handle, suggested, folder,
        [this, as_copy](const std::string& path) {
            if (path.empty()) return;
            write_png_file(path);
            image_modified_ = false;
            if (!as_copy) {
                open_file(path);
            } else {
                render();
            }
        });
}

}
