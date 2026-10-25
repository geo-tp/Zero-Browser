#include "QtWebKitEngine.h"
#include <fcntl.h>
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <sys/socket.h>
#include <unistd.h>

namespace zero {
namespace {
// Linux PSI reports real stalls under resource pressure (not allocated memory).
struct PsiSnapshot { double some = 0, full = 0; bool valid = false; };
PsiSnapshot psi(const char *path) {
    std::ifstream file(path);
    PsiSnapshot result;
    std::string line;
    while (std::getline(file, line)) {
        std::istringstream fields(line);
        std::string category, token;
        if (!(fields >> category) || (category != "some" && category != "full")) continue;
        while (fields >> token) {
            constexpr const char *prefix = "avg10=";
            if (token.compare(0, 6, prefix) != 0) continue;
            try {
                const double value = std::stod(token.substr(6));
                if (!std::isfinite(value) || value < 0 || value > 100) continue;
                if (category == "some") result.some = value;
                else result.full = value;
                result.valid = true;
            } catch (...) { /* Missing PSI data is not evidence of a stall. */ }
        }
    }
    return result;
}
// MemAvailable accounts for reclaimable page cache; unlike MemFree, it is a
// useful estimate of what remains without swapping. Fail closed (no kill) if
// the kernel does not expose it.
double availableMemoryMiB() {
    std::ifstream file("/proc/meminfo");
    std::string key;
    double value = -1;
    std::string unit;
    while (file >> key >> value >> unit) {
        if (key == "MemAvailable:") return value / 1024.0;
    }
    return -1;
}
}

QtWebKitEngine::~QtWebKitEngine() {
    if (control_ >= 0) close(control_);
}
void QtWebKitEngine::record(const std::string &name, const std::string &detail) {
    log(name, detail);
    if (diagnostics_)
        diagnostics_ << std::fixed << std::setprecision(3) << now() << ' ' << name << ' '
                     << cleanText(detail) << std::endl;
}
void QtWebKitEngine::start(const std::string &url) {
    launched_ = now();
    lastHeartbeat_ = launched_;
    heartbeatSeen_ = false;
    nextPressureCheck_ = 0;
    pressureSince_ = -1;
    diagnostics_.open(paths_.state / "qtwebkit.log", std::ios::trunc);
    if (!diagnostics_) log("qtwebkit.log_error", "Cannot write diagnostic file");
    int pair[2];
    if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0, pair) < 0)
        throw std::runtime_error("QtWebKit control socket failed");
    control_ = pair[0];
    fcntl(pair[1], F_SETFD, 0); // Only the helper inherits this endpoint.
    environment_["ZERO_QT_CONTROL_FD"] = std::to_string(pair[1]);
    environment_["ZERO_QT_VIEW_HEIGHT"] = std::to_string(input_.contentHeight());
    environment_["QT_QPA_PLATFORM"] = "wayland";
    environment_["QT_WAYLAND_DISABLE_WINDOWDECORATION"] = "1";
    environment_["QT_AUTO_SCREEN_SCALE_FACTOR"] = "0";
    environment_["QT_SCALE_FACTOR"] = "1";
    // Avoid BCM2835 V4L2 H.264 DMA/CMA allocations on the 512 MiB Zero.
    // Only the QtWebKit helper is affected; audio GStreamer stays available.
    environment_["GST_PLUGIN_FEATURE_RANK"] = "v4l2h264dec:NONE";
    environment_["XDG_CONFIG_HOME"] = (paths_.config / "qtwebkit").string();
    environment_["XDG_CACHE_HOME"] = (paths_.cache / "qtwebkit").string();
    // Preserve the exact private WAYLAND_DISPLAY and XDG_RUNTIME_DIR supplied
    // by DisplayManager. Never fall back to another display or /dev/fb0.
    auto sibling = fs::read_symlink("/proc/self/exe").parent_path() / "zero-qtwebkit";
    auto helper = env("ZERO_BROWSER_QTWEBKIT_BINARY", fs::exists(sibling) ? sibling.string()
                               : "/usr/lib/zero-browser/zero-qtwebkit");
    try {
        process_.start({helper, url}, environment_);
    } catch (...) {
        close(pair[1]);
        throw;
    }
    close(pair[1]);
    record("engine.start", "QTWEBKIT pid=" + std::to_string(process_.pid()) + " platform=wayland");
}
bool QtWebKitEngine::send(const std::string &message) {
    return !stopping_ && control_ >= 0 &&
           ::send(control_, message.data(), message.size(), MSG_NOSIGNAL | MSG_DONTWAIT) ==
               static_cast<ssize_t>(message.size());
}
bool QtWebKitEngine::open(const std::string &url) {
    return ready_ && url.size() <= 8192 && send("open " + url);
}
bool QtWebKitEngine::reload() { return ready_ && send("reload"); }
bool QtWebKitEngine::parseLine(const std::string &line, EngineEvent &e) {
    std::istringstream in(line);
    std::string prefix, kind;
    if (!(in >> prefix >> kind >> e.seconds >> std::quoted(e.url) >> std::quoted(e.message)) ||
        prefix != "ZERO_QT_EVENT") return false;
    if (kind == "ready") e.kind = EngineEvent::Ready;
    else if (kind == "start") e.kind = EngineEvent::LoadStarted;
    else if (kind == "progress") {
        e.kind = EngineEvent::LoadProgress;
        if (!(in >> e.progress) || e.progress < 0 || e.progress > 100 ||
            (e.message != "Fetching" && e.message != "Processing")) return false;
    }
    else if (kind == "url") e.kind = EngineEvent::Redirected;
    else if (kind == "rendering") { e.kind = EngineEvent::LoadProgress; e.message = "Rendering"; e.progress = 100; }
    else if (kind == "rendered") { e.kind = EngineEvent::CommandDone; e.message = "rendered"; }
    else if (kind == "notice" || kind == "hover") { e.kind = EngineEvent::CommandDone; }
    else if (kind == "loaded") e.kind = EngineEvent::Loaded;
    else if (kind == "failed") e.kind = EngineEvent::Failed;
    else return false;
    if (!e.url.empty()) {
        try { e.url = normalizeUrl(e.url); }
        catch (...) { return false; }
    }
    return true;
}
void QtWebKitEngine::sample() {
    const auto t = now();
    if (t < nextSample_ || !process_.running()) return;
    nextSample_ = t + 2;
    const auto root = fs::path("/proc") / std::to_string(process_.pid());
    std::istringstream memory(readFile(root / "statm"));
    uint64_t total = 0, resident = 0;
    memory >> total >> resident;
    auto stat = readFile(root / "stat");
    auto end = stat.rfind(')');
    if (end == std::string::npos || !memory) return;
    std::istringstream fields(stat.substr(end + 2));
    std::string ignored;
    for (int i = 3; i < 14; ++i) fields >> ignored;
    uint64_t user = 0, system = 0;
    if (!(fields >> user >> system)) return;
    const auto ticks = user + system;
    if (sampledAt_ > 0 && t > sampledAt_ && ticks >= cpuTicks_) {
        // 100% = one fully occupied CPU core; a four-core process may reach 400%.
        const double cpu = 100.0 * (ticks - cpuTicks_) / sysconf(_SC_CLK_TCK) / (t - sampledAt_);
        double peakKb = 0, swapKb = 0;
        std::istringstream status(readFile(root / "status"));
        std::string line;
        while (std::getline(status, line)) {
            std::istringstream value(line);
            std::string key;
            value >> key;
            if (key == "VmHWM:") value >> peakKb;
            if (key == "VmSwap:") value >> swapKb;
        }
        record("qtwebkit.process", "rss_mb=" + std::to_string(resident * sysconf(_SC_PAGESIZE) / 1048576.0) +
            " rss_peak_mb=" + std::to_string(peakKb / 1024) + " swap_mb=" + std::to_string(swapKb / 1024) +
            " cpu_percent=" + std::to_string(cpu) + " phase=" + (!ready_ ? "startup" : loading_ ? "loading" : "idle"));
    }
    sampledAt_ = t;
    cpuTicks_ = ticks;
}
void QtWebKitEngine::tick() {
    process_.poll();
    partial_ += process_.drain();
    if (partial_.size() > 131072) partial_.erase(0, partial_.size() - 65536);
    size_t pos;
    while ((pos = partial_.find('\n')) != std::string::npos) {
        auto line = partial_.substr(0, pos);
        partial_.erase(0, pos + 1);
        // Heartbeats are sent by Qt's GUI thread, not a secondary timer/thread.
        // A frozen event loop cannot send them.
        if (line.rfind("ZERO_QT_HEARTBEAT ", 0) == 0) {
            lastHeartbeat_ = now();
            heartbeatSeen_ = true;
            continue;
        }
        EngineEvent e{};
        if (parseLine(line, e)) {
            record("qtwebkit.event", line);
            if (e.kind == EngineEvent::Ready) ready_ = true;
            if (e.kind == EngineEvent::LoadStarted) loading_ = true;
            if (e.kind == EngineEvent::Loaded || e.kind == EngineEvent::Failed) loading_ = false;
            emit_(e);
        } else if (!line.empty()) {
            // Includes platform/plugin stderr even when debug is disabled.
            record("qtwebkit", line);
        }
    }
    sample();
    // Independent emergency guard: an alive Qt GUI can still exhaust memory
    // and stall the whole device. Sample at most every two seconds; require
    // both critically low MemAvailable and sustained kernel memory stalls.
    // High RSS, filled swap, or CPU usage alone are NOT grounds for killing.
    const double pressureNow = now();
    if (!stopping_ && ready_ && process_.running() && pressureNow >= nextPressureCheck_) {
        nextPressureCheck_ = pressureNow + 2.0;
        const auto mem = psi("/proc/pressure/memory");
        const double available = availableMemoryMiB();
        const bool severe = mem.valid && available >= 0 && available < 48.0 &&
                            (mem.full >= 8.0 || mem.some >= 30.0);
        if (severe) {
            if (pressureSince_ < 0) pressureSince_ = pressureNow;
        } else {
            pressureSince_ = -1;
        }
        // Requires sustained pressure across >= 6 s (at least 4 readings).
        if (severe && pressureNow - pressureSince_ >= 6.0) {
            const auto io = psi("/proc/pressure/io");
            record("qtwebkit.guard", "reason=critical_memory_pressure available_mb=" +
                   std::to_string(available) + " mem_some10=" + std::to_string(mem.some) +
                   " mem_full10=" + std::to_string(mem.full) +
                   " io_some10=" + std::to_string(io.some) +
                   " io_full10=" + std::to_string(io.full) + " action=terminate");
            stopping_ = true;
            crashReported_ = true;
            process_.stop(2.0);
            emit_({EngineEvent::Crashed, "", "QtWebKit stopped: critical system memory pressure"});
            return;
        }
    }
    // A 30-second silence from the Qt GUI event loop means the browser is no
    // longer processing interactions, regardless of whether the OS is under
    // memory pressure. This is intentionally independent of loading progress.
    // Sample Linux PSI for diagnostics only; never use it to gate recovery.
    const double current = now();
    if (!stopping_ && ready_ && heartbeatSeen_ && process_.running() &&
        current - lastHeartbeat_ >= 30.0) {
        const auto memory = psi("/proc/pressure/memory");
        const auto io = psi("/proc/pressure/io");
        record("qtwebkit.guard", "no_heartbeat_s=" + std::to_string(current - lastHeartbeat_) +
               " mem_some10=" + std::to_string(memory.some) +
               " mem_full10=" + std::to_string(memory.full) +
               " io_some10=" + std::to_string(io.some) +
               " io_full10=" + std::to_string(io.full) +
               " action=terminate");
        stopping_ = true;
        crashReported_ = true;
        process_.stop(2.0); // SIGTERM; Process::poll escalates to SIGKILL.
        emit_({EngineEvent::Crashed, "", "QtWebKit stopped: engine unresponsive for 30 seconds"});
        return;
    }
    if (!stopping_ && !process_.running() && !crashReported_) {
        crashReported_ = true;
        if (!partial_.empty()) record("qtwebkit", partial_);
        emit_({EngineEvent::Crashed, "", "QtWebKit exited (" + std::to_string(process_.status()) +
            "). See qtwebkit.log"});
    }
}
void QtWebKitEngine::stop() {
    if (stopping_) return;
    send("quit");
    BrowserEngine::stop(); // SIGTERM handled by Qt; existing deadline enforces cleanup.
}
} // namespace zero
