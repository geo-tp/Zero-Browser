#pragma once
#include "Config/Config.h"
#include <deque>
namespace zero {
struct MemorySample {
    double time = 0, availableMb = -1, pssMb = -1, psiSome = -1, psiFull = -1, zramPhysicalMb = -1,
           zramStoredMb = -1, swapUsedMb = -1;
    uint64_t swapIn = 0, swapOut = 0;
    bool swapValid = false;
    int processes = 0, pssReadable = 0;
};
enum class MemoryState { Normal, Pressure, Critical };
const char *memoryName(MemoryState);
class MemoryManager {
    Config config_;
    MemoryState state_ = MemoryState::Normal;
    double badSince_ = -1, goodSince_ = -1;
    MemorySample previous_;
    std::deque<double> restarts_;

  public:
    explicit MemoryManager(const Config &c) : config_(c) {}
    static MemorySample sample(const std::vector<pid_t> &pids, const fs::path &proc = "/proc",
                               const fs::path &sys = "/sys");
    MemoryState update(const MemorySample &, bool degraded);
    bool allowRestart(double time);
    void restarted(double time);
    void resetPressure();
    MemoryState state() const {
        return state_;
    }
};
} // namespace zero
