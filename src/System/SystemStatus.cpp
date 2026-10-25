#include "SystemStatus.h"
#include "Utils/Util.h"
#include <algorithm>
#include <filesystem>
#include <fstream>

namespace zero {
namespace {
int batteryPercent() {
    namespace fs = std::filesystem;
    for (const char* name : {"max170xx_battery", "battery", "BAT0", "BAT1"}) {
        std::ifstream in(std::string("/sys/class/power_supply/") + name + "/capacity");
        int pct = -1;
        if (in >> pct) return std::clamp(pct, 0, 100);
    }
    std::error_code ec;
    for (const auto& e : fs::directory_iterator("/sys/class/power_supply", ec)) {
        std::ifstream in(e.path() / "capacity");
        int pct = -1;
        if (in >> pct) return std::clamp(pct, 0, 100);
    }
    return -1;
}
bool wifiConnected() {
    for (const char* name : {"wlan0", "wlan1"}) {
        std::ifstream in(std::string("/sys/class/net/") + name + "/operstate");
        std::string state;
        if (in >> state; state == "up") return true;
    }
    return false;
}
} // namespace

const SystemStatus &SystemStatus::sample() {
    static SystemStatus status;
    static double checked = -10;
    if (now() - checked > 5) {
        status.batteryPercent = zero::batteryPercent();
        status.wifiConnected = zero::wifiConnected();
        checked = now();
    }
    return status;
}
} // namespace zero
