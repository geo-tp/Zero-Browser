#pragma once
#include "Config/Config.h"
#include "Utils/Process.h"
#include "WaylandBridge.h"
#include "WaylandSession.h"
#include "DisplayStartup.h"
#include "ScreenSaver.h"
#include <linux/fb.h>
namespace zero {
class DisplayManager {
    int fb_ = -1;
    uint8_t *fbMap_ = nullptr;
    size_t fbMapLength_ = 0;
    fb_var_screeninfo var_{};
    fb_fix_screeninfo fix_{};
    std::vector<uint8_t> row_;
    WaylandSession session_;
    fs::path preview_;
    double nextCapture_ = 0;
    ScreenSaver screen_;
    std::string captureError_;

  public:
    WaylandBridge wayland;
    std::vector<uint32_t> frame = std::vector<uint32_t>(320 * 170, 0x0b111a);
    ~DisplayManager();
    void open(const Config &, const fs::path &preview = {});
    void startSession(const Paths &, Startup &, int height, const std::string &renderer);
    void stopSession(const std::function<void()> &heartbeat = {});
    void contentTop(int pixels);
    void tick(int fps);
    void present();
    void activity(double time);
    void idle(double time, bool held);
    void restoreScreen() noexcept { screen_.close(); }
    bool screenOff() const { return screen_.state() == ScreenSaver::State::Off; }
    const char *screenState() const { return screen_.name(); }
    const std::map<std::string, std::string> &environment() const {
        return session_.environment();
    }
    bool sessionAlive();
    static uint32_t packPixel(uint32_t rgb, const fb_var_screeninfo &);
};
} // namespace zero
