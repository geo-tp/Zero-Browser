#include "Display/ScreenSaver.h"
#include "Input/InputManager.h"
#include <fstream>

using namespace zero;
void require(bool, const char *);

void screenSaverTests(const fs::path &dir) {
    using State = ScreenSaver::State;
    const auto root = dir / "backlight";
    const auto panel = root / "backlight_gpio";
    const auto brightness = panel / "brightness";
    atomicWrite(panel / "max_brightness", "255\n");
    atomicWrite(brightness, "200\n");
    auto read = [&] {
        int value = -1;
        std::ifstream(brightness) >> value;
        return value;
    };
    // Write in place: the real sysfs node also remains open throughout the session.
    auto write = [&](int value) { std::ofstream(brightness) << value << '\n'; };
    {
        ScreenSaver screen;
        screen.open(-1, 100, root);
        require(screen.state() == State::Awake && read() == 200, "Opening preserves brightness");
        screen.tick(129.999);
        require(screen.state() == State::Awake && read() == 200, "No dimming before 30 seconds");
        screen.tick(130);
        require(screen.state() == State::Dimmed && read() == 40, "Dim to 20 percent at 30 seconds");
        require(screen.pixel(0xffffff) == 0xffffff, "Hardware dimming does not also dim pixels");
        screen.tick(159.999);
        require(screen.state() == State::Dimmed && read() == 40, "Screen stays dim until 60 seconds");
        screen.tick(160);
        require(screen.state() == State::Off && read() == 0, "Backlight is off at 60 seconds total");
        require(screen.activity(161) && read() == 200 && screen.state() == State::Awake,
                "Any activity restores the brightness saved before dimming");
        screen.tick(190.999);
        require(screen.state() == State::Awake, "Activity starts a new full idle interval");
        screen.tick(191);
        require(screen.state() == State::Dimmed, "Dimming restarts 30 seconds after activity");
        screen.activity(192);
        require(read() == 200, "Activity also wakes a merely dimmed display");
        write(75);
        screen.tick(222);
        require(read() == 15, "Next idle cycle uses the user's updated brightness");
        screen.tick(252);
        require(read() == 0, "Second cycle powers off");
        require(screen.tick(253, true) && read() == 75, "Held input restores the screen");
        screen.tick(265, true);
        require(screen.state() == State::Awake && read() == 75, "Held modifiers prevent sleep");
        screen.tick(295);
        require(screen.state() == State::Dimmed, "Idle timer starts after held input ends");
    }
    require(read() == 75, "Destructor restores brightness when quitting while dimmed");
    {
        ScreenSaver screen;
        screen.open(-1, 0, root);
        screen.tick(60);
        require(read() == 0, "A delayed tick may go directly to off");
        screen.close();
        require(read() == 75, "Closing while off restores the original brightness");
        write(60);
    }
    require(read() == 60, "Repeated cleanup never overwrites later brightness changes");

    atomicWrite(panel / "max_brightness", "1\n");
    write(1);
    {
        ScreenSaver screen;
        screen.open(-1, 0, root);
        screen.tick(30);
        require(read() == 1 && screen.pixel(0xffffff) == 0x333333,
                "Binary backlights dim pixels without powering off early");
        screen.tick(60);
        require(read() == 0 && screen.pixel(0xffffff) == 0, "Binary backlight powers off at 60s");
        screen.activity(61);
        require(read() == 1 && screen.pixel(0xffffff) == 0xffffff, "Binary backlight wakes fully");
    }
    {
        ScreenSaver screen;
        screen.open(-1, 0, dir / "missing-backlight");
        screen.tick(30);
        require(screen.pixel(0xff8040) == 0x33190c, "Software fallback dims each channel");
        screen.tick(60);
        require(screen.pixel(0xffffff) == 0, "Missing hardware falls back to a black frame");
        screen.activity(61);
        require(screen.pixel(0xff8040) == 0xff8040, "Fallback preserves the original colors on wake");
    }
    {
        ScreenSaver screen;
        screen.open(-1, 0, {});
        screen.tick(30);
        require(screen.pixel(0xffffff) == 0x333333 && read() == 1,
                "Preview mode uses software dimming without touching hardware");
    }
    auto event = [](int type, int code, int value) {
        input_event e{};
        e.type = type;
        e.code = code;
        e.value = value;
        return InputManager::isActivity(e);
    };
    require(event(EV_KEY, KEY_FN, 1) && event(EV_KEY, KEY_LEFTSHIFT, 0),
            "Modifier presses and releases count as activity");
    require(event(EV_KEY, KEY_A, 2), "Kernel key repeat counts as activity");
    require(event(EV_REL, REL_X, 1) && event(EV_REL, REL_WHEEL, -1) &&
            event(EV_KEY, BTN_LEFT, 1) && event(EV_ABS, ABS_X, 0),
            "Mouse movement, wheel, buttons and touch count as activity");
    require(!event(EV_SYN, SYN_REPORT, 0) && !event(EV_SYN, SYN_DROPPED, 0) &&
            !event(EV_MSC, MSC_SCAN, 42) && !event(EV_REL, REL_X, 0),
            "Bookkeeping and zero motion do not postpone sleep");
    require(!event(EV_KEY, KEY_A, -1) && !event(EV_KEY, KEY_A, 3),
            "Invalid key events do not wake the screen");
}
