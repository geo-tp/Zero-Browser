#include "BrowserEngine.h"
namespace zero {
void BrowserEngine::pollCommand() {
    command_.poll();
    auto output = command_.drain();
    if (config_.debug && !output.empty())
        log("engine.command_output", output);
    if (commandName_.empty())
        return;
    if (command_.running() && now() - commandStarted_ > config_.commandTimeout)
        command_.stop(.2);
    if (!command_.running()) {
        auto name = commandName_;
        commandName_.clear();
        double seconds = now() - commandStarted_;
        if (command_.status() != 0)
            emit_({EngineEvent::Failed, "",
                   seconds >= config_.commandTimeout ? "Engine command timed out"
                                                     : "Engine command failed: " + name,
                   seconds});
        else
            emit_({EngineEvent::CommandDone, "", name, seconds});
    }
}
void BrowserEngine::stop() {
    stopping_ = true;
    command_.stop(.2);
    process_.stop(config_.stopTimeout);
}
bool BrowserEngine::stopped() {
    command_.poll();
    process_.poll();
    command_.drain();
    process_.drain();
    return command_.empty() && process_.empty();
}
} // namespace zero
