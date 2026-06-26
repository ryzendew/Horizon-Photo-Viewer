#include "core/viewer/app.hpp"

#include <cstdio>
#include <cstdlib>
#include <iostream>

int main(int argc, char** argv) {
    hpv::App app(argc, argv);

    if (!app.init()) {
        std::cerr << "Failed to initialize Horizon Photo Viewer\n";
        return 1;
    }

    return app.run();
}
