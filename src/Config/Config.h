#pragma once
#include "Utils/Util.h"
namespace zero {
struct Config {
    std::string home = "https://lite.duckduckgo.com/lite/", fb = "/dev/fb0", input;
    int fps = 10, webLimit = 160, netLimit = 80;
    double conservative = .25, strict = .50;
    double pressureMb = 80, criticalMb = 40, hysteresisMb = 12;
    double pressureSeconds = 12, recoverySeconds = 8;
    double cooldown = 60, restartWindow = 600;
    int maxRestarts = 2;
    double commandTimeout = 12, loadTimeout = 90, stopTimeout = 4;
    bool debug = false, autoRecover = true;
    void load(const Paths &);
    void save(const Paths &) const;
    std::vector<std::string> cogArguments() const;
};
} // namespace zero
