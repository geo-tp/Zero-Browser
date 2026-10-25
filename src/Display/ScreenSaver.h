#pragma once
#include "Utils/Util.h"

namespace zero {
class ScreenSaver {
  public:
    enum class State { Awake, Dimmed, Off };
    static constexpr double DimAfter = 30, OffAfter = 60;
    static constexpr int DimPercent = 20;

    ScreenSaver() = default;
    ScreenSaver(const ScreenSaver &) = delete;
    ScreenSaver &operator=(const ScreenSaver &) = delete;
    ~ScreenSaver();
    // An empty root disables hardware access (previews and frame-file mode).
    void open(int framebuffer, double time, const fs::path &root = "/sys/class/backlight");
    void close() noexcept;
    bool activity(double time);
    bool tick(double time, bool held = false);
    State state() const { return state_; }
    const char *name() const;
    uint32_t pixel(uint32_t rgb) const;

  private:
    int framebuffer_ = -1, brightness_ = -1, savedBrightness_ = -1;
    int softwarePercent_ = 100;
    bool brightnessChanged_ = false, framebufferBlanked_ = false;
    double lastActivity_ = 0, retryAt_ = 0;
    State state_ = State::Awake;
    bool change(State);
    bool writeBrightness(int) noexcept;
    void restoreHardware() noexcept;
};
} // namespace zero
