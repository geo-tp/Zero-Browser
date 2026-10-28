#include "DisplayManager.h"
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <stdexcept>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <thread>
#include <unistd.h>
namespace zero {
DisplayManager::~DisplayManager() {
    screen_.close();
    if (fbMap_)
        munmap(fbMap_, fbMapLength_);
    if (fb_ >= 0)
        close(fb_);
}
void DisplayManager::open(const Config &c, const fs::path &preview) {
    preview_ = preview;
    if (!preview.empty()) {
        screen_.open(-1, now(), {});
        return;
    }
    fb_ = ::open(c.fb.c_str(), O_RDWR | O_CLOEXEC);
    if (fb_ < 0)
        throw std::runtime_error("Cannot open LCD " + c.fb + ": " + strerror(errno));
    if (ioctl(fb_, FBIOGET_VSCREENINFO, &var_) < 0 || ioctl(fb_, FBIOGET_FSCREENINFO, &fix_) < 0)
        throw std::runtime_error("Cannot read LCD geometry");
    if (var_.xres != 320 || var_.yres != 170 ||
        (var_.bits_per_pixel != 16 && var_.bits_per_pixel != 24 && var_.bits_per_pixel != 32) ||
        fix_.type != FB_TYPE_PACKED_PIXELS || fix_.visual != FB_VISUAL_TRUECOLOR)
        throw std::runtime_error("LCD must be 320 x 170, packed true colour");
    for (auto field : {var_.red, var_.green, var_.blue, var_.transp})
        if (field.msb_right || field.length > 8 || field.offset + field.length > var_.bits_per_pixel)
            throw std::runtime_error("Unsupported LCD pixel layout");
    if (uint64_t(var_.yoffset + 170) * fix_.line_length > fix_.smem_len ||
        uint64_t(var_.xoffset + 320) * (var_.bits_per_pixel / 8) > fix_.line_length)
        throw std::runtime_error("Invalid LCD stride or offsets");
    row_.resize(320 * (var_.bits_per_pixel / 8));
    // SPI fbdev drivers may rely on mmap/deferred IO dirty-page tracking.
    // A successful pwrite() can update fb memory without updating the panel.
    fbMapLength_ = fix_.smem_len;
    void *mapped = mmap(nullptr, fbMapLength_, PROT_READ | PROT_WRITE, MAP_SHARED, fb_, 0);
    if (mapped != MAP_FAILED) {
        fbMap_ = static_cast<uint8_t *>(mapped);
        log("display.fb", "mmap framebuffer enabled");
    } else {
        log("display.fb", "mmap unavailable, using pwrite fallback: " + std::string(strerror(errno)));
    }
    log("display.fb", std::to_string(var_.bits_per_pixel) + "bpp stride=" + std::to_string(fix_.line_length));
    screen_.open(fb_, now());
}
void DisplayManager::activity(double time) {
    if (screen_.activity(time)) {
        nextCapture_ = 0;
        present();
    }
}
void DisplayManager::idle(double time, bool held) {
    if (screen_.tick(time, held)) present();
}
void DisplayManager::startSession(const Paths &p, Startup &startup, int height, const std::string &renderer) {
    startup.step("socket", StartupDomain::Wayland, [&] {
        session_.start(p, renderer, [&](int, double) { startup.pulse(); return true; });
    });
    auto heartbeat = [&] {
        startup.pulse();
        if (!sessionAlive()) throw std::runtime_error("Private compositor or bus exited");
    };
    startup.step("protocols", StartupDomain::Wayland, [&] {
        // An absolute socket avoids mutating the supervisor's XDG_RUNTIME_DIR.
        wayland.connect(environment().at("XDG_RUNTIME_DIR") + "/" + environment().at("WAYLAND_DISPLAY"), heartbeat);
    });
    configureDisplay(startup, wayland, environment(), height, heartbeat);
    session_.validated();
    log("display.session", "private labwc, direct SHM capture");
}
void DisplayManager::stopSession(const std::function<void()> &heartbeat) {
    wayland.disconnect();
    session_.shutdown(heartbeat);
    captureError_.clear();
    nextCapture_ = 0;
}
bool DisplayManager::sessionAlive() { return session_.alive(); }
void DisplayManager::contentTop(int pixels) {
    pixels = std::clamp(pixels, 0, 40);
    if (wayland.contentHeight() == 170 - pixels) return;
    // Resize the existing private output: page pixels remain 1:1 on the LCD.
    auto result = command({"wlr-randr", "--output", "HEADLESS-1", "--custom-mode",
                           "320x" + std::to_string(170 - pixels)}, 5, environment());
    if (result.status) throw std::runtime_error("Cannot resize browser viewport (status " +
                                              std::to_string(result.status) + "): " + result.output);
    wayland.contentTop(pixels);
    nextCapture_ = 0;
}
void DisplayManager::tick(int fps) {
    wayland.pump();
    const auto error = wayland.error();
    if (error != captureError_) {
        log(error.empty() ? "display.capture_recovered" : "display.capture_error", error);
        captureError_ = error;
    }
    if (!screenOff() && now() >= nextCapture_) {
        wayland.capture();
        nextCapture_ = now() + 1.0 / fps;
    }
}
uint32_t DisplayManager::packPixel(uint32_t rgb, const fb_var_screeninfo &v) {
    auto c = [](unsigned value, fb_bitfield f) {
        return f.length ? ((value * ((1u << f.length) - 1) + 127) / 255) << f.offset : 0u;
    };
    return c((rgb >> 16) & 255, v.red) | c((rgb >> 8) & 255, v.green) | c(rgb & 255, v.blue) |
           c(255, v.transp);
}
void DisplayManager::present() {
    if (fb_ < 0) {
        if (preview_.empty())
            return;
        std::string ppm = "P6\n320 170\n255\n";
        ppm.reserve(320 * 170 * 3 + 32);
        for (auto pixel : frame) {
            pixel = screen_.pixel(pixel);
            ppm += static_cast<char>(pixel >> 16);
            ppm += static_cast<char>(pixel >> 8);
            ppm += static_cast<char>(pixel);
        }
        atomicWrite(preview_, ppm);
        return;
    }
    unsigned bytes = var_.bits_per_pixel / 8;
    for (unsigned y = 0; y < 170; ++y) {
        for (unsigned x = 0; x < 320; ++x) {
            uint32_t p = packPixel(screen_.pixel(frame[y * 320 + x]), var_);
            std::memcpy(row_.data() + x * bytes, &p, bytes);
        }
        size_t pos = 0;
        off_t offset = (y + var_.yoffset) * fix_.line_length + var_.xoffset * bytes;
        if (fbMap_) {
            std::memcpy(fbMap_ + offset, row_.data(), row_.size());
            continue;
        }
        while (pos < row_.size()) {
            ssize_t n = pwrite(fb_, row_.data() + pos, row_.size() - pos, offset + pos);
            if (n < 0 && errno == EINTR)
                continue;
            if (n <= 0)
                throw std::runtime_error("LCD write failed");
            pos += n;
        }
    }
}
} // namespace zero
