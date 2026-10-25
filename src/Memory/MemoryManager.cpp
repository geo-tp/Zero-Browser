#include "MemoryManager.h"
#include <algorithm>
#include <sstream>
namespace zero {
const char *memoryName(MemoryState s) {
    return s == MemoryState::Normal ? "NORMAL" : s == MemoryState::Pressure ? "PRESSURE" : "CRITICAL";
}
MemorySample MemoryManager::sample(const std::vector<pid_t> &pids, const fs::path &proc,
                                   const fs::path &sys) {
    MemorySample s;
    s.time = now();
    s.processes = pids.size();
    std::string line, key;
    double val;
    std::istringstream mem(readFile(proc / "meminfo", 65536));
    double swapTotalKb = -1, swapFreeKb = -1;
    while (std::getline(mem, line)) {
        std::istringstream row(line);
        if (row >> key >> val) {
            if (key == "MemAvailable:") s.availableMb = val / 1024;
            else if (key == "SwapTotal:") swapTotalKb = val;
            else if (key == "SwapFree:") swapFreeKb = val;
        }
    }
    if (swapTotalKb >= 0 && swapFreeKb >= 0)
        s.swapUsedMb = std::max(0.0, swapTotalKb - swapFreeKb) / 1024;
    std::istringstream psi(readFile(proc / "pressure/memory", 4096));
    while (std::getline(psi, line)) {
        std::istringstream row(line);
        row >> key;
        std::string part;
        while (row >> part)
            if (part.rfind("avg10=", 0) == 0)
                try {
                    if (key == "some")
                        s.psiSome = std::stod(part.substr(6));
                    if (key == "full")
                        s.psiFull = std::stod(part.substr(6));
                } catch (...) {
                }
    }
    std::istringstream vm(readFile(proc / "vmstat", 65536));
    uint64_t count;
    int found = 0;
    while (vm >> key >> count) {
        if (key == "pswpin") {
            s.swapIn = count;
            found++;
        }
        if (key == "pswpout") {
            s.swapOut = count;
            found++;
        }
    }
    s.swapValid = found == 2;
    std::istringstream z(readFile(sys / "block/zram0/mm_stat", 4096));
    double orig, compressed, used;
    if (z >> orig >> compressed >> used) {
        s.zramPhysicalMb = used / 1048576;
        s.zramStoredMb = orig / 1048576;
    }
    double pss = 0;
    for (auto pid : pids) {
        std::istringstream in(readFile(proc / std::to_string(pid) / "smaps_rollup", 16384));
        while (std::getline(in, line)) {
            std::istringstream row(line);
            if (row >> key >> val && key == "Pss:") {
                pss += val / 1024;
                s.pssReadable++;
                break;
            }
        }
    }
    if (s.pssReadable)
        s.pssMb = pss;
    return s;
}
MemoryState MemoryManager::update(const MemorySample &s, bool degraded) {
    double swapRate = 0;
    if (s.swapValid && previous_.swapValid && s.time > previous_.time && s.swapIn >= previous_.swapIn &&
        s.swapOut >= previous_.swapOut)
        swapRate =
            double(s.swapIn - previous_.swapIn + s.swapOut - previous_.swapOut) / (s.time - previous_.time);
    bool low = s.availableMb >= 0 && s.availableMb < config_.criticalMb;
    bool pressure =
        (s.availableMb >= 0 && s.availableMb < config_.pressureMb) || s.psiSome >= 5 || s.psiFull >= 2;
    bool severe = (low && (s.psiSome >= 10 || s.psiFull >= 3 || swapRate > 256 || degraded)) ||
                  s.psiFull >= 15 || (degraded && pressure);
    // A sustained MemAvailable below 40 MiB is sufficient after twice the confirmation period.
    bool bad = low || severe;
    if (bad) {
        if (badSince_ < 0)
            badSince_ = s.time;
    } else
        badSince_ = -1;
    bool recovered = s.availableMb >= config_.pressureMb + config_.hysteresisMb && s.psiSome < 3 &&
                     s.psiFull < 1 && !degraded;
    if (recovered) {
        if (goodSince_ < 0)
            goodSince_ = s.time;
    } else
        goodSince_ = -1;
    if (badSince_ >= 0 && s.time - badSince_ >= config_.pressureSeconds * (severe ? 1 : 2)) {
        state_ = MemoryState::Critical;
    } else if (state_ == MemoryState::Normal && pressure)
        state_ = MemoryState::Pressure;
    if (goodSince_ >= 0 && s.time - goodSince_ >= config_.recoverySeconds) {
        state_ = MemoryState::Normal;
        badSince_ = -1;
    }
    previous_ = s;
    return state_;
}
bool MemoryManager::allowRestart(double time) {
    while (!restarts_.empty() && time - restarts_.front() > config_.restartWindow)
        restarts_.pop_front();
    return restarts_.size() < static_cast<size_t>(config_.maxRestarts) &&
           (restarts_.empty() || time - restarts_.back() >= config_.cooldown);
}
void MemoryManager::resetPressure() {
    state_ = MemoryState::Normal;
    badSince_ = goodSince_ = -1;
    previous_ = {};
}
void MemoryManager::restarted(double t) {
    restarts_.push_back(t);
    resetPressure();
}
} // namespace zero
