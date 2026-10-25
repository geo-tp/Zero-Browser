#include "Util.h"
#include <algorithm>
#include <arpa/inet.h>
#include <cerrno>
#include <cstdlib>
#include <fcntl.h>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>
namespace zero {
namespace {
fs::path sessionLogPath;
std::ofstream sessionLog;
size_t sessionLogBytes = 0;
void rotateSessionLog() {
    sessionLog.close();
    std::error_code ec;
    // Keep failed launches across the following successful launches, with a
    // bounded disk cost: current log plus three previous files, <= 1 MiB each.
    for (int i = 3; i >= 1; --i) {
        const fs::path source = i == 1 ? sessionLogPath : fs::path(sessionLogPath.string() + "." + std::to_string(i - 1));
        if (fs::exists(source, ec)) fs::rename(source, sessionLogPath.string() + "." + std::to_string(i), ec);
    }
    sessionLog.open(sessionLogPath, std::ios::trunc);
    sessionLogBytes = 0;
    if (sessionLog) fs::permissions(sessionLogPath, fs::perms::owner_read | fs::perms::owner_write, ec);
}
} // namespace
void openSessionLog(const fs::path &path) {
    sessionLogPath = path;
    rotateSessionLog();
    if (!sessionLog) std::cerr << "Cannot write session diagnostics: " << path << '\n';
}
double now() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
std::string env(const char *name, const std::string &fallback) {
    const char *v = std::getenv(name);
    return v && *v ? v : fallback;
}
std::string readFile(const fs::path &p, size_t limit) {
    std::ifstream f(p, std::ios::binary);
    std::string s;
    char b[4096];
    while (f && s.size() < limit) {
        f.read(b, std::min(sizeof(b), limit - s.size()));
        s.append(b, static_cast<size_t>(f.gcount()));
    }
    return s;
}
void atomicWrite(const fs::path &p, const std::string &s) {
    fs::create_directories(p.parent_path());
    auto tmp = p.string() + ".tmp.XXXXXX";
    std::vector<char> n(tmp.begin(), tmp.end());
    n.push_back(0);
    int fd = mkstemp(n.data());
    if (fd < 0)
        throw std::runtime_error("Cannot save " + p.string());
    size_t pos = 0;
    bool ok = true;
    while (pos < s.size()) {
        auto rc = write(fd, s.data() + pos, s.size() - pos);
        if (rc < 0 && errno == EINTR)
            continue;
        if (rc <= 0) {
            ok = false;
            break;
        }
        pos += rc;
    }
    if (fsync(fd) < 0)
        ok = false;
    close(fd);
    if (!ok || rename(n.data(), p.c_str()) < 0) {
        unlink(n.data());
        throw std::runtime_error("Cannot commit " + p.string());
    }
    int dir = open(p.parent_path().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dir >= 0) {
        fsync(dir);
        close(dir);
    }
}
std::string cleanText(std::string s) {
    for (char &c : s)
        if (static_cast<unsigned char>(c) < 32 || c == 127)
            c = ' ';
    return s;
}
std::string normalizeUrl(std::string s) {
    auto a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    if (a == std::string::npos)
        throw std::runtime_error("Enter an HTTP or HTTPS address");
    s = s.substr(a, b - a + 1);
    if (s.size() > 4096 || s.find_first_of("\r\n\t") != std::string::npos)
        throw std::runtime_error("Invalid address");
    if (s == "about:blank")
        return s;
    if (s.find("://") == std::string::npos) {
        auto hostEnd = s.find_first_of("/?#");
        auto hostname = s.substr(0, hostEnd);
        bool domain = hostname.find('.') != std::string::npos &&
                      hostname.front() != '.' && hostname.back() != '.' &&
                      hostname.find(' ') == std::string::npos;
        bool local = hostname == "localhost" || hostname.rfind("localhost:", 0) == 0 ||
                     (hostname.size() > 1 && hostname.front() == '[' && hostname.find(']') != std::string::npos);
        if (!domain && !local) {
            static constexpr char hex[] = "0123456789ABCDEF";
            std::string query;
            for (unsigned char c : s) {
                if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                    (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~')
                    query += static_cast<char>(c);
                else {
                    query += '%';
                    query += hex[c >> 4];
                    query += hex[c & 15];
                }
            }
            return "https://lite.duckduckgo.com/lite/?q=" + query;
        }
        // Local IP addresses typically serve plain HTTP, often without TLS.
        // Keep the existing HTTPS default for normal domain names.
        std::string address = hostname;
        if (!address.empty() && address.front() == '[') {
            // The existing bracketed IPv6 host handling is preserved.
            s = "http://" + s;
        } else {
            // IPv4 may include a port (e.g. 192.168.1.1:8080).
            const auto colon = address.find(':');
            if (colon != std::string::npos)
                address.resize(colon);
            in_addr ipv4{};
            const bool isIpv4 = inet_pton(AF_INET, address.c_str(), &ipv4) == 1;
            s = std::string(isIpv4 || hostname == "localhost" ||
                            hostname.rfind("localhost:", 0) == 0 ? "http://" : "https://") + s;
        }
    }
    if (s.rfind("http://", 0) != 0 && s.rfind("https://", 0) != 0)
        throw std::runtime_error("Only HTTP and HTTPS are supported");
    size_t host = s.find("://") + 3, end = s.find_first_of("/?#", host);
    if (host == s.size() || end == host ||
        s.substr(host, end - host).find_first_of(" @\\") != std::string::npos)
        throw std::runtime_error("Invalid host name");
    for (size_t i = 0; i < s.size(); ++i)
        if (s[i] == ' ') {
            s.replace(i, 1, "%20");
            i += 2;
        }
    return s;
}
void log(const std::string &e, const std::string &d) {
    std::ostringstream line;
    line << std::fixed << std::setprecision(3) << now() << " " << e << " " << cleanText(d) << "\n";
    const auto text = line.str();
    std::cerr << text;
    if (sessionLog.is_open()) {
        const auto saved = text.size() > 16384 ? text.substr(0, 16383) + "\n" : text;
        if (sessionLogBytes + saved.size() > 1024 * 1024) rotateSessionLog();
        sessionLog << saved << std::flush;
        sessionLogBytes += saved.size();
    }
}
Paths Paths::user() {
    auto home = env("HOME");
    if (home.empty())
        throw std::runtime_error("HOME is not set");
    return {fs::path(env("XDG_CONFIG_HOME", home + "/.config")) / "zero-browser",
            fs::path(env("XDG_STATE_HOME", home + "/.local/state")) / "zero-browser",
            fs::path(env("XDG_CACHE_HOME", home + "/.cache")) / "zero-browser",
            fs::path(env("XDG_RUNTIME_DIR", "/run/user/" + std::to_string(getuid()))) / "zero-browser"};
}
void Paths::create() const {
    for (auto &p : {config, state, cache, runtime}) {
        fs::create_directories(p);
        chmod(p.c_str(), 0700);
    }
}
} // namespace zero
