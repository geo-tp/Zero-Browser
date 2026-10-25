#include "ScreenSaver.h"
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <fstream>
#include <linux/fb.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace zero {
namespace {
int readBrightness(int fd) {
    char text[32]{};
    ssize_t count;
    do {
        count = pread(fd, text, sizeof text - 1, 0);
    } while (count < 0 && errno == EINTR);
    int value = -1;
    return count > 0 && std::sscanf(text, "%d", &value) == 1 ? value : -1;
}
}

ScreenSaver::~ScreenSaver() { close(); }

void ScreenSaver::open(int framebuffer, double time, const fs::path &root) {
    close();
    framebuffer_ = framebuffer;
    lastActivity_ = time;
    retryAt_ = 0;
    if (root.empty()) return;
    std::vector<fs::path> devices;
    // Prefer the Cardputer panel over an unrelated external display.
    devices.push_back(root / "backlight_gpio");
    std::error_code error;
    for (fs::directory_iterator it(root, error), end; !error && it != end; it.increment(error))
        if (it->path() != devices.front()) devices.push_back(it->path());
    for (const auto &device : devices) {
        int maximum = 0;
        if (!(std::ifstream(device / "max_brightness") >> maximum) || maximum <= 0) continue;
        int fd = ::open((device / "brightness").c_str(), O_RDWR | O_CLOEXEC);
        if (fd < 0) continue;
        int current = readBrightness(fd);
        if (current < 0 || current > maximum) {
            ::close(fd);
            continue;
        }
        brightness_ = fd;
        log("screen.backlight", device.string());
        break;
    }
}

bool ScreenSaver::writeBrightness(int value) noexcept {
    if (brightness_ < 0) return false;
    char text[32];
    int length = std::snprintf(text, sizeof text, "%d\n", value);
    ssize_t count;
    do {
        count = pwrite(brightness_, text, length, 0);
    } while (count < 0 && errno == EINTR);
    return count == length;
}

void ScreenSaver::restoreHardware() noexcept {
    if (brightnessChanged_ && writeBrightness(savedBrightness_)) brightnessChanged_ = false;
    if (framebufferBlanked_ && ioctl(framebuffer_, FBIOBLANK, FB_BLANK_UNBLANK) == 0)
        framebufferBlanked_ = false;
}

void ScreenSaver::close() noexcept {
    restoreHardware();
    if (brightnessChanged_ || framebufferBlanked_)
        std::fputs("zero-browser: could not restore display brightness\n", stderr);
    if (brightness_ >= 0) ::close(brightness_);
    brightness_ = framebuffer_ = savedBrightness_ = -1;
    brightnessChanged_ = framebufferBlanked_ = false;
    state_ = State::Awake;
    softwarePercent_ = 100;
}

bool ScreenSaver::change(State next) {
    if (state_ == next) return false;
    if (next == State::Awake) {
        restoreHardware();
        softwarePercent_ = 100;
    } else {
        // Snapshot each idle cycle, so a user's brightness adjustment is preserved.
        if (state_ == State::Awake && !brightnessChanged_)
            savedBrightness_ = readBrightness(brightness_);
        if (next == State::Dimmed) {
            softwarePercent_ = DimPercent;
            if (savedBrightness_ > 1 &&
                writeBrightness(std::max(1, savedBrightness_ / (100 / DimPercent)))) {
                brightnessChanged_ = true;
                softwarePercent_ = 100;
            }
        } else {
            softwarePercent_ = 0;
            if (savedBrightness_ > 0 && writeBrightness(0)) {
                brightnessChanged_ = true;
            } else if (framebuffer_ >= 0 && ioctl(framebuffer_, FBIOBLANK, FB_BLANK_POWERDOWN) == 0) {
                framebufferBlanked_ = true;
            } else if (framebuffer_ >= 0) {
                log("screen.fallback", "Backlight/blanking unavailable; showing a black frame");
            }
        }
    }
    state_ = next;
    log("screen.state", name());
    return true;
}

bool ScreenSaver::activity(double time) {
    lastActivity_ = time;
    return change(State::Awake);
}

bool ScreenSaver::tick(double time, bool held) {
    const bool woke = held && activity(time);
    if (state_ == State::Awake && (brightnessChanged_ || framebufferBlanked_) && time >= retryAt_) {
        restoreHardware();
        retryAt_ = time + 1;
    }
    const double idle = time - lastActivity_;
    return change(idle >= OffAfter ? State::Off : idle >= DimAfter ? State::Dimmed : State::Awake) || woke;
}

const char *ScreenSaver::name() const {
    switch (state_) {
    case State::Dimmed: return "dimmed";
    case State::Off: return "off";
    default: return "awake";
    }
}

uint32_t ScreenSaver::pixel(uint32_t rgb) const {
    if (softwarePercent_ == 100) return rgb;
    if (softwarePercent_ == 0) return 0;
    auto dim = [this](uint32_t channel) { return channel * softwarePercent_ / 100; };
    return (dim((rgb >> 16) & 255) << 16) | (dim((rgb >> 8) & 255) << 8) | dim(rgb & 255);
}
} // namespace zero
