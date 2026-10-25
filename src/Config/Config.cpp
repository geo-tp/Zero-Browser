#include "Config.h"
#include <sstream>
namespace zero {
void Config::load(const Paths &p) {
    std::istringstream in(readFile(p.config / "settings", 16384));
    std::string line;
    while (std::getline(in, line)) {
        auto eq = line.find('=');
        if (eq == std::string::npos)
            continue;
        auto key = line.substr(0, eq), value = line.substr(eq + 1);
        try {
            if (key == "home")
                home = normalizeUrl(value);
            else if (key == "fps")
                fps = std::stoi(value);
            else if (key == "auto_recover")
                autoRecover = value == "1";
            else if (key == "debug")
                debug = value == "1";
            else if (key == "pressure_mb")
                pressureMb = std::stod(value);
            else if (key == "critical_mb")
                criticalMb = std::stod(value);
            else if (key == "pressure_seconds")
                pressureSeconds = std::stod(value);
            else if (key == "web_limit_mb")
                webLimit = std::stoi(value);
            else if (key == "net_limit_mb")
                netLimit = std::stoi(value);
        } catch (...) {
            log("config.invalid", key);
        }
    }
    if (fps < 2 || fps > 30)
        fps = 10;
    if (!(criticalMb >= 16 && criticalMb < pressureMb && pressureMb <= 200)) {
        criticalMb = 40;
        pressureMb = 80;
    }
    if (!(pressureSeconds >= 5 && pressureSeconds <= 120))
        pressureSeconds = 12;
    if (webLimit < 64 || webLimit > 200)
        webLimit = 160;
    if (netLimit < 32 || netLimit > 100)
        netLimit = 80;
    fb = env("ZERO_BROWSER_FB", fb);
    input = env("ZERO_BROWSER_INPUT", input);
}
void Config::save(const Paths &p) const {
    std::ostringstream s;
    s << "home=" << home << "\nfps=" << fps << "\nauto_recover=" << autoRecover << "\ndebug=" << debug
      << "\npressure_mb=" << pressureMb << "\ncritical_mb=" << criticalMb
      << "\npressure_seconds=" << pressureSeconds << "\nweb_limit_mb=" << webLimit
      << "\nnet_limit_mb=" << netLimit << "\n";
    atomicWrite(p.config / "settings", s.str());
}
std::vector<std::string> Config::cogArguments() const {
    return {"cog",
            "-P",
            "wl",
            "--gapplication-app-id=org.cardputer.ZeroBrowser.Cog",
            "--doc-viewer",
            "--enable-page-cache=false",
            "--web-mem-limit=" + std::to_string(webLimit),
            "--web-conservative-threshold=" + std::to_string(conservative),
            "--web-strict-threshold=" + std::to_string(strict),
            "--net-mem-limit=" + std::to_string(netLimit)};
}
} // namespace zero
