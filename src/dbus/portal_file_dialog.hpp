#pragma once

#include <functional>
#include <memory>
#include <string>

typedef struct DBusConnection DBusConnection;
typedef struct DBusWatch DBusWatch;

namespace hpv {

class PortalFileDialog {
public:
    using Callback = std::function<void(const std::string& path)>;

    PortalFileDialog();
    ~PortalFileDialog();

    PortalFileDialog(const PortalFileDialog&) = delete;
    PortalFileDialog& operator=(const PortalFileDialog&) = delete;

    bool init();
    void open_file(const std::string& parent_window_id, Callback callback);
    int get_fd() const;
    bool dispatch();
    bool needs_flush() const;

private:
    DBusConnection* conn_ = nullptr;
    Callback pending_callback_;
    std::string pending_handle_;
    std::string parent_window_id_;
};

}
