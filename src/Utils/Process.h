#pragma once
#include "Util.h"
#include <map>
#include <sys/types.h>
namespace zero {
struct ProcInfo {
    pid_t pid = 0, parent = 0, group = 0, session = 0;
    uint64_t start = 0;
    char state = 0;
};
std::map<pid_t, ProcInfo> processTable(const fs::path &proc = "/proc");
class Process {
    pid_t root_ = 0;
    uint64_t identity_ = 0;
    int output_ = -1, status_ = -1;
    bool exited_ = true, stopping_ = false;
    double deadline_ = 0;
    std::map<pid_t, uint64_t> owned_;
    void signalOwned(int sig);

  public:
    Process() = default;
    Process(const Process &) = delete;
    Process &operator=(const Process &) = delete;
    ~Process();
    void start(const std::vector<std::string> &args,
               const std::map<std::string, std::string> &environment = {});
    void poll();
    std::string drain();
    void stop(double grace = 3);
    bool empty();
    bool running() const {
        return root_ > 0 && !exited_;
    }
    int status() const {
        return status_;
    }
    pid_t pid() const {
        return root_;
    }
    std::vector<pid_t> members();
};
struct CommandResult {
    int status;
    std::string output;
};
CommandResult command(const std::vector<std::string> &args, double timeout = 5,
                      const std::map<std::string, std::string> &environment = {});
} // namespace zero
