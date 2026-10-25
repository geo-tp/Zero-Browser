#pragma once
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>
namespace zero {
namespace fs = std::filesystem;
double now();
std::string readFile(const fs::path &path, size_t limit = 1024 * 1024);
void atomicWrite(const fs::path &path, const std::string &text);
std::string env(const char *name, const std::string &fallback = "");
std::string normalizeUrl(std::string text);
std::string cleanText(std::string text);
void log(const std::string &event, const std::string &detail = "");
void openSessionLog(const fs::path &path);
struct Paths {
    fs::path config, state, cache, runtime;
    static Paths user();
    void create() const;
};
} // namespace zero
