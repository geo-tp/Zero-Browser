#include "Process.h"
#include <cerrno>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <sstream>
#include <stdexcept>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
namespace zero {
std::map<pid_t, ProcInfo> processTable(const fs::path &proc) {
    std::map<pid_t, ProcInfo> result;
    std::error_code ec;
    for (auto &entry : fs::directory_iterator(proc, ec)) {
        auto name = entry.path().filename().string();
        if (name.empty() || name.find_first_not_of("0123456789") != std::string::npos)
            continue;
        auto s = readFile(entry.path() / "stat", 4096);
        auto pos = s.rfind(')');
        if (pos == std::string::npos)
            continue;
        std::istringstream in(s.substr(pos + 2));
        ProcInfo p;
        try {
            p.pid = std::stoi(name);
        } catch (...) {
            continue;
        }
        in >> p.state >> p.parent >> p.group >> p.session;
        std::string unused;
        for (int field = 7; field <= 21; ++field)
            in >> unused;
        if (in >> p.start)
            result[p.pid] = p;
    }
    return result;
}
void Process::start(const std::vector<std::string> &args,
                    const std::map<std::string, std::string> &environment) {
    if (args.empty() || !empty())
        throw std::runtime_error("Process already active");
    int pipefd[2];
    if (pipe2(pipefd, O_CLOEXEC) < 0)
        throw std::runtime_error("pipe2 failed");
    auto parent = getpid();
    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        throw std::runtime_error("fork failed");
    }
    if (pid == 0) {
        close(pipefd[0]);
        if (setsid() < 0)
            _exit(126);
        prctl(PR_SET_PDEATHSIG, SIGTERM);
        if (getppid() != parent)
            _exit(126);
        int nullfd = open("/dev/null", O_RDONLY);
        if (nullfd >= 0) {
            dup2(nullfd, 0);
            close(nullfd);
        }
        dup2(pipefd[1], 1);
        dup2(pipefd[1], 2);
        close(pipefd[1]);
        for (auto &[key, value] : environment)
            setenv(key.c_str(), value.c_str(), 1);
        std::vector<char *> argv;
        for (auto &a : args)
            argv.push_back(const_cast<char *>(a.c_str()));
        argv.push_back(nullptr);
        signal(SIGINT, SIG_DFL);
        signal(SIGTERM, SIG_DFL);
        signal(SIGPIPE, SIG_DFL);
        execvp(argv[0], argv.data());
        dprintf(2, "exec %s: %s\n", argv[0], strerror(errno));
        _exit(127);
    }
    close(pipefd[1]);
    if (output_ >= 0)
        close(output_);
    output_ = pipefd[0];
    fcntl(output_, F_SETFL, O_NONBLOCK);
    root_ = pid;
    exited_ = false;
    stopping_ = false;
    status_ = -1;
    owned_.clear();
    auto table = processTable();
    auto it = table.find(pid);
    identity_ = it != table.end() ? it->second.start : 0;
    owned_[pid] = identity_;
}
std::vector<pid_t> Process::members() {
    std::vector<pid_t> result;
    if (root_ <= 0)
        return result;
    auto table = processTable();
    auto root = table.find(root_);
    bool sameRoot = root != table.end() && (!identity_ || root->second.start == identity_);
    if (sameRoot && !identity_) {
        identity_ = root->second.start;
        owned_[root_] = identity_;
    }
    bool changed = true;
    while (changed) {
        changed = false;
        for (auto &[pid, p] : table) {
            if (owned_.count(pid))
                continue;
            auto parent = owned_.find(p.parent);
            auto liveParent = table.find(p.parent);
            bool descendant = parent != owned_.end() && liveParent != table.end() &&
                              liveParent->second.start == parent->second;
            // Root identity guards against accidentally claiming a recycled session ID.
            if (descendant || (sameRoot && p.session == root_)) {
                owned_[pid] = p.start;
                changed = true;
            }
        }
    }
    for (auto it = owned_.begin(); it != owned_.end();) {
        auto p = table.find(it->first);
        if (p == table.end() || p->second.start != it->second) {
            it = owned_.erase(it);
            continue;
        }
        if (p->second.state != 'Z' && p->second.state != 'X')
            result.push_back(it->first);
        else if (it->first != root_)
            waitpid(it->first, nullptr, WNOHANG);
        ++it;
    }
    return result;
}
void Process::poll() {
    members();
    if (root_ > 0 && !exited_) {
        int s;
        auto rc = waitpid(root_, &s, WNOHANG);
        if (rc == root_) {
            exited_ = true;
            status_ = WIFEXITED(s) ? WEXITSTATUS(s) : 128 + WTERMSIG(s);
        }
    }
    if (stopping_ && now() >= deadline_) {
        signalOwned(SIGKILL);
        deadline_ = now() + 0.25;
    }
}
std::string Process::drain() {
    std::string s;
    char b[4096];
    if (output_ < 0)
        return s;
    while (s.size() < 65536) {
        auto n = read(output_, b, sizeof b);
        if (n > 0)
            s.append(b, n);
        else if (n < 0 && errno == EINTR)
            continue;
        else
            break;
    }
    return s;
}
void Process::signalOwned(int sig) {
    auto pids = members(); // Signal verified identities, including children with their own process groups.
    for (auto pid : pids)
        if (pid != root_)
            kill(pid, sig);
    for (auto pid : pids)
        if (pid == root_)
            kill(pid, sig);
}
void Process::stop(double grace) {
    if (stopping_)
        return;
    stopping_ = true;
    deadline_ = now() + grace;
    signalOwned(SIGTERM);
}
bool Process::empty() {
    poll();
    return !running() && members().empty();
}
Process::~Process() {
    stop(0.15);
    double end = now() + 1;
    while (!empty() && now() < end) {
        drain();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (!empty())
        signalOwned(SIGKILL);
    if (output_ >= 0)
        close(output_);
}
CommandResult command(const std::vector<std::string> &args, double timeout,
                      const std::map<std::string, std::string> &environment) {
    Process p;
    p.start(args, environment);
    std::string out;
    double end = now() + timeout;
    while (!p.empty()) {
        auto s = p.drain();
        if (out.size() < 131072)
            out += s.substr(0, 131072 - out.size());
        if (now() >= end)
            p.stop(0.1);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    out += p.drain();
    return {now() >= end ? 124 : p.status(), out};
}
} // namespace zero
