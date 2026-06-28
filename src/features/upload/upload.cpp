#include "features/upload/upload.hpp"

#include <curl/curl.h>
#include <cstring>
#include <sstream>
#include <vector>

namespace hpv {

static const char kBase64Table[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static std::string base64_encode(const std::string& in) {
    std::string out;
    out.reserve(((in.size() + 2) / 3) * 4);
    size_t i = 0;
    while (i < in.size()) {
        uint8_t b0 = (uint8_t)in[i++];
        uint8_t b1 = i < in.size() ? (uint8_t)in[i] : 0;
        uint8_t b2 = i + 1 < in.size() ? (uint8_t)in[i + 1] : 0;
        out += kBase64Table[b0 >> 2];
        out += kBase64Table[((b0 & 0x03) << 4) | (b1 >> 4)];
        if (i < in.size()) {
            out += kBase64Table[((b1 & 0x0F) << 2) | (b2 >> 6)];
            if (i + 1 < in.size()) {
                out += kBase64Table[b2 & 0x3F];
                i += 2;
            } else {
                out += '=';
                i += 1;
            }
        } else {
            out += '=';
            out += '=';
            i += 0;
        }
    }
    return out;
}

struct CurlWriteBuffer {
    std::string data;
};

static size_t curl_write_callback(void* contents, size_t size, size_t nmemb, void* userp) {
    size_t total = size * nmemb;
    auto* buf = static_cast<CurlWriteBuffer*>(userp);
    buf->data.append(static_cast<const char*>(contents), total);
    return total;
}

bool upload_to_imgur(const std::string& png_data, const std::string& client_id,
                     std::string& out_url, std::string& out_error) {
    out_url.clear();
    out_error.clear();

    CURL* curl = curl_easy_init();
    if (!curl) {
        out_error = "Failed to initialize curl";
        return false;
    }

    std::string b64 = base64_encode(png_data);

    std::string post_data = "image=" + b64 + "&type=base64";

    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/x-www-form-urlencoded");
    std::string auth = "Authorization: Client-ID " + client_id;
    headers = curl_slist_append(headers, auth.c_str());

    CurlWriteBuffer response;
    curl_easy_setopt(curl, CURLOPT_URL, "https://api.imgur.com/3/image");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, post_data.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)post_data.size());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "Horizon-Photo/1.0");

    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        out_error = curl_easy_strerror(res);
        return false;
    }

    if (http_code != 200) {
        out_error = "HTTP " + std::to_string(http_code) + ": " + response.data;
        return false;
    }

    // Parse JSON response for "data":{"link":"..."}
    auto link_pos = response.data.find("\"link\":\"");
    if (link_pos == std::string::npos) {
        out_error = "No link in response";
        return false;
    }
    link_pos += 8; // past "link":" 
    auto end_pos = response.data.find('"', link_pos);
    if (end_pos == std::string::npos) {
        out_error = "Malformed link field";
        return false;
    }

    // Unescape any JSON escapes
    std::string raw = response.data.substr(link_pos, end_pos - link_pos);
    for (size_t i = 0; i < raw.size(); i++) {
        if (raw[i] == '\\' && i + 1 < raw.size()) {
            char c = raw[i + 1];
            if (c == '/' || c == '\\' || c == '"') {
                raw.erase(i, 1);
            }
            i++;
        }
    }
    out_url = raw;
    return true;
}

}
