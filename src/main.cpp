#include "Application/Application.h"
#include <iostream>
int main(int argc, char **argv) {
    try {
        zero::Options options;
        for (int i = 1; i < argc; ++i) {
            std::string arg = argv[i];
            auto value = [&]() {
                if (i + 1 >= argc)
                    throw std::runtime_error("Missing argument for " + arg);
                return std::string(argv[++i]);
            };
            if (arg == "--version") {
                std::cout << "Zero-Browser " ZERO_BROWSER_VERSION "\n";
                return 0;
            }
            if (arg == "--help") {
                std::cout << "Zero-Browser for Cardputer Zero\n  --mode lite|qtwebkit|full  --url URL  --debug\n  "
                             "--preview FILE.ppm  (render selector and exit)\n  --no-input  --frame-file "
                             "FILE.ppm  --run-seconds N  (test tools)\n";
                return 0;
            }
            if (arg == "--preview")
                options.preview = value();
            else if (arg == "--mode") {
                options.mode = value();
                if (options.mode != "lite" && options.mode != "qtwebkit" && options.mode != "full")
                    throw std::runtime_error("Mode must be lite, qtwebkit or full");
            } else if (arg == "--url")
                options.url = value();
            else if (arg == "--debug")
                options.debug = true;
            else if (arg == "--no-input")
                options.noInput = true;
            else if (arg == "--frame-file")
                options.frameFile = value();
            else if (arg == "--run-seconds")
                options.seconds = std::stod(value());
            else
                throw std::runtime_error("Unknown option: " + arg);
        }
        zero::Application app(options);
        return app.run();
    } catch (const std::exception &e) {
        zero::log("fatal", e.what());
        return 1;
    }
}
