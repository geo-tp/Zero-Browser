#include "Browser/QtWebKitEngine.h"
#include <csignal>
#include <iostream>
#include <thread>
#include <unistd.h>

using namespace zero;

namespace {
void check(bool value, const char *message) {
    if (!value) throw std::runtime_error(message);
}
void pauseTick() { std::this_thread::sleep_for(std::chrono::milliseconds(20)); }
}

int main(int argc, char **argv) {
    try {
        const auto scenario = env("ZERO_TEST_GUARD_SCENARIO");
        if (!env("ZERO_QT_CONTROL_FD").empty()) {
            // A real owned child, deliberately ignoring TERM to test escalation.
            signal(SIGTERM, SIG_IGN);
            std::cout << "ZERO_QT_EVENT ready 0 \"\" \"\"\n" << std::flush;
            const double begun = now();
            while (true) {
                const double elapsed = now() - begun;
                if (scenario != "no-heartbeat" &&
                    (scenario != "heartbeat-timeout" || elapsed < .1) &&
                    (scenario != "heartbeat-reset" || elapsed < 4))
                    std::cout << "ZERO_QT_HEARTBEAT " << elapsed << std::endl;
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
            }
        }
        check(argc == 2, "Expected test directory");
        fs::path root = argv[1];
        Paths paths{root / "config", root / "state", root / "cache", root / "run"};
        paths.create();
        const fs::path proc = env("ZERO_TEST_GUARD_PROC");
        auto memory = [&](int available, const std::string &pressure) {
            atomicWrite(proc / "meminfo", available < 0 ? "MemFree: 100 kB\n" :
                        "MemAvailable: " + std::to_string(available * 1024) + " kB\n");
            atomicWrite(proc / "memory", pressure);
        };
        const std::string healthy = "some avg10=0\nfull avg10=0\n";
        const std::string full = "some avg10=0\nfull avg10=8\n";
        memory(128, healthy);
        atomicWrite(proc / "io", healthy);
        if (scenario == "memory-full" || scenario == "transient") memory(47, full);
        if (scenario == "memory-some") memory(47, "some avg10=30\nfull avg10=0\n");
        if (scenario == "low-only") memory(1, healthy);
        if (scenario == "psi-only") memory(128, full);
        if (scenario == "invalid-psi") memory(1, "some avg10=nan\nfull avg10=-1\n");
        if (scenario == "missing-available") memory(-1, full);
        if (scenario == "memory-boundary") memory(48, full);

        Config config;
        WaylandBridge input;
        std::string failure;
        double failedAt = -1, readyAt = -1;
        int failures = 0;
        QtWebKitEngine engine(config, input, paths, {}, [&](const EngineEvent &event) {
            if (event.kind == EngineEvent::Ready) readyAt = now();
            if (event.kind == EngineEvent::Crashed) {
                failure = event.message;
                failedAt = now();
                ++failures;
            }
        });
        setenv("ZERO_BROWSER_QTWEBKIT_BINARY", fs::absolute(argv[0]).c_str(), 1);
        engine.start("https://example.org/");
        const auto pid = engine.pid();
        const double begun = now();
        const bool timeout = scenario == "heartbeat-timeout" || scenario == "heartbeat-reset";
        const bool pressure = scenario == "memory-full" || scenario == "memory-some" || scenario == "transient";
        const double duration = scenario == "heartbeat-reset" ? 37 :
            (scenario == "heartbeat-timeout" || scenario == "heartbeat-live" || scenario == "no-heartbeat") ? 32 :
            scenario == "transient" ? 15 : 9;
        int phase = 0;
        while (now() - begun < duration && failure.empty()) {
            if (scenario == "transient") {
                if (phase == 0 && now() - begun > 2.5) { memory(128, healthy); phase = 1; }
                if (phase == 1 && now() - begun > 5.5) { memory(47, full); phase = 2; }
            }
            engine.tick();
            pauseTick();
        }
        check(readyAt >= 0, "Fake helper never became ready");
        if (timeout || pressure) {
            check(failures == 1, "Expected exactly one guard event");
            check(failure.find(timeout ? "unresponsive for 30 seconds" : "critical system memory pressure") !=
                  std::string::npos, "Wrong shutdown reason");
            const double elapsed = failedAt - readyAt;
            const double minimum = scenario == "heartbeat-reset" ? 33 : timeout ? 30 :
                                   scenario == "transient" ? 12 : 6;
            check(elapsed >= minimum && elapsed < minimum + 2, "Guard timing changed");
            check(kill(pid, 0) == 0, "TERM-ignoring helper exited before escalation");
            while (!engine.stopped() && now() - failedAt < 4) pauseTick();
            check(engine.stopped() && kill(pid, 0) == -1, "Guard did not reap the helper");
            check(now() - failedAt >= 1.8, "Two-second TERM grace was shortened");
            std::cout << "PASS " << scenario << " guard at " << elapsed << "s; child reaped\n";
        } else {
            check(failure.empty() && kill(pid, 0) == 0, "Guard killed a healthy/insufficient-evidence case");
            engine.stop();
            const double stoppedAt = now();
            while (!engine.stopped() && now() - stoppedAt < 6) pauseTick();
            check(engine.stopped() && kill(pid, 0) == -1, "Stopped helper survived cleanup");
            std::cout << "PASS " << scenario << " no false guard; child reaped\n";
        }
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "GUARD TEST FAILED: " << e.what() << '\n';
        return 1;
    }
}
