#include "Browser/CogEngine.h"
#include "Browser/BrowserEngine.h"
#include "Display/DisplayManager.h"
#include "History/History.h"
#include "Input/InputManager.h"
#include "Memory/MemoryManager.h"
#include "UI/UIManager.h"
#include "Utils/Process.h"
#include <csignal>
#include <iostream>
#include <stdexcept>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
using namespace zero;
static int checks = 0;
void screenSaverTests(const fs::path &);
void browserTests(const fs::path &);
void require(bool ok, const char *message) {
    checks++;
    if (!ok)
        throw std::runtime_error(message);
}
int main(int argc, char **argv) {
    if (argc > 1 && std::string(argv[1]) == "--child-tree") {
        auto child = fork();
        if (child == 0) {
            setsid();
            signal(SIGTERM, SIG_IGN);
            for (;;)
                pause();
        }
        std::cout << child << std::endl;
        for (;;)
            pause();
    }
    try {
        // ENTER in Full overlay must enter the address field, never activate arrows.
        UIManager overlay;
        require(static_cast<int>(UIManager::View::Select) == 0 &&
                static_cast<int>(UIManager::View::Web) == 1 &&
                static_cast<int>(UIManager::View::Menu) == 2 &&
                static_cast<int>(UIManager::View::Address) == 3 &&
                static_cast<int>(UIManager::View::Settings) == 5 &&
                static_cast<int>(UIManager::View::CleanupConfirm) == 6 &&
                static_cast<int>(UIManager::View::Keybinds) == 7 &&
                static_cast<int>(UIManager::View::Transition) == 8,
                "Control socket view identifiers remain stable");
        overlay.show(UIManager::View::Menu);
        Key enter{};
        enter.code = KEY_ENTER;
        enter.value = 1;
        require(overlay.key(enter, nullptr).type == Action::None &&
                overlay.view() == UIManager::View::Address,
                "Full overlay ENTER focuses address rather than arrows");
        overlay.address("https://example.com/test");
        Key repeatedEnter = enter;
        repeatedEnter.value = 2;
        require(overlay.key(repeatedEnter, nullptr).type == Action::None,
                "Repeated ENTER does not submit URL");
        Action open = overlay.key(enter, nullptr);
        require(open.type == Action::Open && open.text == "https://example.com/test",
                "ENTER submits address in Full overlay");
        for (auto [code, expected] : {std::pair<int,char>{KEY_F,'f'}, {KEY_X,'x'}, {KEY_Z,'z'}, {KEY_C,'c'}}) {
            require(InputManager::character(code, 128u) == expected, "Fn direction produces lowercase");
            require(InputManager::character(code, 0u) == expected, "Bare direction key retains base character");
        }
        require(InputManager::character(KEY_B, 1u) == '.', "Shift+B enters dot");
        require(InputManager::character(KEY_B, 128u) == '.', "Fn+B enters dot");
        require(InputManager::character(KEY_S, 128u) == ':', "Fn/SYM+S enters colon");
        require(InputManager::character(KEY_N, 128u) == '/', "Fn/SYM+N enters slash");
        char pattern[] = "/tmp/zero-browser-tests.XXXXXX";
        auto name = mkdtemp(pattern);
        if (!name)
            throw std::runtime_error("mkdtemp");
        fs::path dir = name;
        screenSaverTests(dir);
        browserTests(dir);
        require(normalizeUrl("example.org") == "https://example.org", "URL default scheme");
        require(normalizeUrl("192.168.1.1") == "http://192.168.1.1", "IPv4 uses HTTP");
        require(normalizeUrl("192.168.1.10:8080/admin") == "http://192.168.1.10:8080/admin", "IPv4 with port uses HTTP");
        require(normalizeUrl("127.0.0.1") == "http://127.0.0.1", "Loopback uses HTTP");
        require(normalizeUrl("localhost:3000") == "http://localhost:3000", "Localhost port uses HTTP");
        require(normalizeUrl("[::1]:8080") == "http://[::1]:8080", "IPv6 uses HTTP");
        require(normalizeUrl("https://192.168.1.1") == "https://192.168.1.1", "Explicit HTTPS retained");
        require(normalizeUrl("hello world") == "https://lite.duckduckgo.com/lite/?q=hello%20world", "Search phrases");
        require(normalizeUrl("foobar") == "https://lite.duckduckgo.com/lite/?q=foobar", "Single word search");
        require(normalizeUrl(" http://localhost/a b ") == "http://localhost/a%20b", "URL spaces");
        bool invalid = false;
        try {
            normalizeUrl("file:///etc/passwd");
        } catch (...) {
            invalid = true;
        }
        require(invalid, "Reject non-web scheme");
        History h;
        h.navigate("https://a.org/", "LITE");
        h.navigate("https://b.org/", "FULL");
        h.navigate("https://c.org/", "FULL");
        require(h.back() && h.current() == "https://b.org/", "Back stack");
        h.save(dir / "history");
        History loaded;
        loaded.load(dir / "history");
        require(loaded.position() == 1 && loaded.canForward(), "Persist position and forward stack");
        auto count = loaded.visits().size();
        loaded.navigate(loaded.current(), "FULL");
        require(loaded.visits().size() == count, "Restore does not duplicate visits");
        loaded.redirect("https://b.org/new", "FULL");
        require(loaded.position() == 1 && loaded.canForward(), "Redirect preserves cursor");
        loaded.navigate("https://d.org/", "LITE");
        require(!loaded.canForward() && loaded.stack().size() == 3, "New navigation truncates forward stack");
        atomicWrite(dir / "broken", "ZERO-HISTORY 1\n999999 2 0\n");
        History bad;
        bad.load(dir / "broken");
        require(bad.current().empty(), "Corrupt history ignored");
        Config config;
        MemoryManager mem(config);
        MemorySample s;
        s.time = 100;
        s.availableMb = 100;
        s.psiSome = 0;
        s.psiFull = 0;
        require(mem.update(s, false) == MemoryState::Normal, "Normal memory");
        s.time = 102;
        s.availableMb = 25;
        s.psiSome = 15;
        s.psiFull = 5;
        require(mem.update(s, false) == MemoryState::Pressure, "Transient pressure is not yet critical");
        s.time = 114;
        require(mem.update(s, false) == MemoryState::Critical, "Persistent concordant pressure");
        s.time = 115;
        s.availableMb = 88;
        s.psiSome = 0;
        s.psiFull = 0;
        require(mem.update(s, false) == MemoryState::Critical, "Hysteresis prevents bounce");
        s.time = 116;
        s.availableMb = 100;
        mem.update(s, false);
        s.time = 124;
        require(mem.update(s, false) == MemoryState::Normal, "Sustained recovery");
        require(mem.allowRestart(125), "First restart allowed");
        mem.restarted(125);
        require(!mem.allowRestart(140), "Restart cooldown");
        require(mem.allowRestart(190), "Cooldown expires");
        mem.restarted(190);
        require(!mem.allowRestart(300), "Retry budget");
        require(mem.allowRestart(800), "Window expires");
        MemoryManager navigationPressure(config);
        MemorySample ptest;
        ptest.time = 200;
        ptest.availableMb = 70;
        ptest.psiSome = 6;
        ptest.psiFull = 0;
        require(navigationPressure.update(ptest, false) == MemoryState::Pressure,
                "Initial moderate pressure");
        ptest.time = 207;
        require(navigationPressure.update(ptest, false) == MemoryState::Pressure,
                "Sustained moderate pressure remains noncritical");
        navigationPressure.resetPressure();
        require(navigationPressure.state() == MemoryState::Normal,
                "New session resets memory state");
        MemoryManager solo(config);
        s.availableMb = 35;
        s.psiFull = 0;
        s.psiSome = 0;
        s.time = 1;
        solo.update(s, false);
        s.time = 14;
        require(solo.update(s, false) != MemoryState::Critical,
                "Single low indicator needs longer confirmation");
        s.time = 26;
        require(solo.update(s, false) == MemoryState::Critical,
                "Persistent low availability becomes critical");
        atomicWrite(dir / "proc/meminfo", "MemAvailable: 51200 kB\n");
        atomicWrite(dir / "proc/pressure/memory",
                    "some avg10=12.50 avg60=1 total=20\nfull avg10=4.00 total=10\n");
        atomicWrite(dir / "proc/vmstat", "pswpin 42\npswpout 84\n");
        atomicWrite(dir / "proc/12/smaps_rollup", "Pss: 10240 kB\nRss: 999999 kB\n");
        atomicWrite(dir / "sys/block/zram0/mm_stat", "104857600 100 10485760\n");
        auto sample = MemoryManager::sample({12, 13}, dir / "proc", dir / "sys");
        require(sample.availableMb == 50 && sample.pssMb == 10 && sample.pssReadable == 1 &&
                    sample.processes == 2,
                "PSS and partial coverage");
        require(sample.psiSome == 12.5 && sample.psiFull == 4 && sample.swapIn == 42 &&
                    sample.zramPhysicalMb == 10,
                "PSI swap and zram parsing");
        EngineEvent e{};
        require(CogEngine::parseLine("Cog-Message: <https://example.org/> Load started.", e) &&
                    e.kind == EngineEvent::LoadStarted,
                "Cog start parser");
        require(CogEngine::parseLine("Cog-Message: <https://example.org/> Loaded successfully.", e) &&
                    e.kind == EngineEvent::Loaded,
                "Cog completion parser");
        require(!CogEngine::parseLine("Noise about https://example.org", e), "Reject unrelated log lines");
        fb_var_screeninfo v{};
        v.red = {11, 5, 0};
        v.green = {5, 6, 0};
        v.blue = {0, 5, 0};
        require(DisplayManager::packPixel(0xff0000, v) == 0xf800 &&
                    DisplayManager::packPixel(0x00ff00, v) == 0x7e0,
                "RGB565 conversion");
        require(InputManager::character(KEY_F, 0) == 'f' && InputManager::character(KEY_F, 128u) == 'f' &&
                    InputManager::character(KEY_SLASH, 1) == '?',
                "Literal keyboard mapping");
        prctl(PR_SET_CHILD_SUBREAPER, 1);
        Process unrelated, tree;
        unrelated.start({"/bin/sleep", "30"});
        tree.start({fs::absolute(argv[0]).string(), "--child-tree"});
        double until = now() + 3;
        std::string output;
        while (now() < until && output.find('\n') == std::string::npos) {
            tree.poll();
            output += tree.drain();
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        require(!output.empty(), "Process fixture ready");
        auto members = tree.members();
        require(members.size() == 2, "Track descendant with separate session");
        tree.stop(.1);
        until = now() + 2;
        while (!tree.empty() && now() < until)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        require(tree.empty(), "Escalation cleans engine descendants");
        require(unrelated.running() && kill(unrelated.pid(), 0) == 0, "Unrelated process survives");
        unrelated.stop(.1);
        auto result = command({"/bin/sleep", "5"}, .1);
        require(result.status == 124, "Command deadline");
        fs::remove_all(dir);
        std::cout << checks << " checks passed\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "TEST FAILED: " << e.what() << "\n";
        return 1;
    }
}
