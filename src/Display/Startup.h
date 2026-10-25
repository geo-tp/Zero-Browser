#pragma once
#include "Utils/Process.h"
#include <functional>
#include <stdexcept>
#include <thread>

namespace zero {
// Kept identical in Zero Browser and Zero USBImager. Recovery is only entered
// before handing input to the application, never during an imaging operation.
struct StartupCancelled : std::exception {
    const char *what() const noexcept override { return "Startup cancelled"; }
};
enum class StartupDomain { Wayland, Application };
struct StartupFailure : std::runtime_error {
    StartupDomain domain;
    std::string stage;
    StartupFailure(StartupDomain d, const std::string &s, const std::string &why)
        : std::runtime_error(s + ": " + why), domain(d), stage(s) {}
};
class Startup {
  public:
    using Progress = std::function<bool(int, const std::string &, bool)>;
    static constexpr int Attempts = 3;
    explicit Startup(Progress progress) : progress_(std::move(progress)) {}
    int attempt() const { return attempt_; }
    // Cleanup must finish even when cancellation is requested. Remember it and
    // keep drawing/reading input while owned children terminate.
    void service() { if (progress_ && !progress_(attempt_, stage_, recovering_)) cancelled_ = true; }
    void pulse() { service(); if (cancelled_) throw StartupCancelled{}; }
    void stage(const std::string &s) {
        stage_ = s;
        log("startup.stage", "attempt=" + std::to_string(attempt_) + "/3 stage=" + s);
        pulse();
    }
    template<class F> void step(const std::string &s, StartupDomain domain, F action) {
        stage(s);
        try { action(); }
        catch (const StartupCancelled &) { throw; }
        catch (const StartupFailure &) { throw; }
        catch (const std::exception &e) { throw StartupFailure(domain, s, e.what()); }
    }
    template<class F> void wait(double timeout, F ready, const std::string &why) {
        const auto deadline = now() + timeout;
        for (;;) {
            pulse();
            if (ready()) return;
            if (now() >= deadline) throw std::runtime_error(why);
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    template<class Start, class Cleanup> void run(Start start, Cleanup cleanup) {
        for (attempt_ = 1; attempt_ <= Attempts; ++attempt_) {
            recovering_ = attempt_ > 1;
            try {
                pulse();
                start();
                log("startup.ready", "attempt=" + std::to_string(attempt_) + "/3");
                return;
            } catch (const StartupFailure &e) {
                log(e.domain == StartupDomain::Wayland ? "wayland.failed" : "application.failed",
                    "attempt=" + std::to_string(attempt_) + "/3 stage=" + e.stage + " reason=" + e.what());
                recovering_ = true;
                stage_ = "cleanup";
                cleanup();
                pulse();
                if (e.domain != StartupDomain::Wayland || attempt_ == Attempts) throw;
                log("wayland.retry", "attempt=" + std::to_string(attempt_ + 1) + "/3");
            } catch (...) {
                cleanup();
                throw;
            }
        }
    }
  private:
    Progress progress_;
    int attempt_ = 1;
    std::string stage_ = "socket";
    bool recovering_ = false, cancelled_ = false;
};
} // namespace zero
