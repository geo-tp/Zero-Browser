#pragma once
#include "Display/DisplayManager.h"
#include "Input/InputManager.h"
#include "UI/UIManager.h"
#include <memory>
#include <set>
namespace zero {
struct Options {
    fs::path preview, frameFile;
    bool noInput = false, debug = false;
    std::string mode, url;
    double seconds = 0;
};
class Application {
    Options options_;
    Paths paths_;
    Config config_;
    DisplayManager display_;
    InputManager input_;
    UIManager ui_;
    std::unique_ptr<BrowserManager> browser_;
    Mode requestedMode_ = Mode::QtWebKit;
    std::string startupError_;
    int lock_ = -1, control_ = -1;
    struct ControlClient {
        int fd;
        double deadline;
        std::string response;
    };
    std::vector<ControlClient> controlClients_;
    fs::path socket_, sessionDir_;
    bool quit_ = false, pointer_ = true;
    bool leftMouseHeld_ = false;
    static bool qtDownloadsFocused(const WaylandBridge &);
    void releaseMouse();
    int pointerX_ = 160, pointerY_ = 85;
    bool heldUp_ = false, heldDown_ = false, heldLeft_ = false, heldRight_ = false;
    bool heldScrollUp_ = false, heldScrollDown_ = false, heldScrollLeft_ = false, heldScrollRight_ = false;
    double pointerRepeat_ = 0, pointerHeldSince_ = 0, pointerFracX_ = 0, pointerFracY_ = 0;
    double scrollHeldSince_ = 0, scrollFracX_ = 0, scrollFracY_ = 0, edgeScrollX_ = 0, edgeScrollY_ = 0;
    void updatePointer(double t);
    double noticeUntil_ = 0, clickHintUntil_ = 0;
    double addressSubmittedAt_ = -1;
    double escapePressedAt_ = -1;
    std::string notice_;
    std::set<uint16_t> suppressed_;
    std::set<uint16_t> shortcutHeld_;
    void start(Mode);
    void action(const Action &);
    void click(bool right = false);
    void key(const Key &);
    void toggleOverlay();
    void control();
    std::string execute(const std::string &);
    void render();

  public:
    explicit Application(Options);
    ~Application();
    int run();
};
} // namespace zero
