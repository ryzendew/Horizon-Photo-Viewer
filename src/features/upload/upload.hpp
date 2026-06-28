#pragma once

#include <string>

namespace hpv {

bool upload_to_imgur(const std::string& png_data, const std::string& client_id,
                     std::string& out_url, std::string& out_error);

}