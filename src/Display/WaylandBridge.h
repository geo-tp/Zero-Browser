#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>
namespace zero {
struct Toplevel {
    uint64_t id = 0, parent = 0;
    std::string title, appId;
    bool active = false, complete = false, mapped = false;
};
class WaylandBridge {
    struct Impl;
    std::unique_ptr<Impl> impl_;

  public:
    WaylandBridge();
    ~WaylandBridge();
    void disconnect();
    void connect(const std::string &display, const std::function<void()> &heartbeat = {});
    void pump();
    void capture();
    const std::vector<uint32_t> &pixels() const;
    bool connected() const;
    uint64_t capturedFrames() const;
    double lastFrameAt() const;
    bool outputReady(int width, int height) const;
    bool captureSize(int width, int height) const;
    void resetCapture();
    void contentTop(int pixels);
    int contentHeight() const;
    void key(uint16_t code, bool down, uint32_t modifiers = 0);
    void tap(uint16_t code, uint32_t modifiers = 0);
    void releaseKeys();
    void paste(const std::string &text);
    void requestClipboard();
    bool clipboardResult(std::string &text);
    void pointer(int x, int y);
    void click(bool right = false);
    void mouseButton(bool down);
    void scroll(int horizontal, int vertical);
    std::string error() const;
    std::vector<Toplevel> toplevels() const;
    // A close always names one live handle; never synthesize Alt+F4.
    bool closeToplevel(uint64_t id, const std::string &appId);
    bool activateToplevel(const std::string &appId);
};
} // namespace zero
