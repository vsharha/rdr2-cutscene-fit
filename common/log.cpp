// Copyright (c) 2026 Viktor Sharha
// Portions adapted from rdr2-ultrawide-proper-black-bar-fix, Copyright (c) 2026 rschi (MIT).

#include "common/log.h"

#include <atomic>
#include <fstream>
#include <mutex>

#include "common/paths.h"

namespace logger {
namespace {

std::atomic<bool> g_enabled{true};
std::mutex g_mutex;
std::ofstream g_file;
std::filesystem::path g_directory;

}  // namespace

void set_enabled(bool enabled) {
    g_enabled.store(enabled);
}

bool open(HMODULE self) {
    if (!g_enabled.load()) {
        return false;
    }
    const std::filesystem::path asi = paths::module_path(self);
    const std::filesystem::path name = asi.stem().concat(L".log");

    std::lock_guard lock(g_mutex);
    for (const std::filesystem::path& directory : {asi.parent_path(), paths::fallback_directory()}) {
        if (directory.empty()) {
            continue;
        }
        g_file.open(directory / name, std::ios::out | std::ios::trunc);
        if (g_file.is_open()) {
            g_directory = directory;
            return true;
        }
        g_file.clear();
    }
    return false;
}

std::filesystem::path directory() {
    std::lock_guard lock(g_mutex);
    return g_directory;
}

void write(std::string_view line) {
    if (!g_enabled.load()) {
        return;
    }
    SYSTEMTIME t{};
    GetLocalTime(&t);
    const std::string stamped = std::format("[{:02}:{:02}:{:02}.{:03}] {}\n", t.wHour, t.wMinute,
                                            t.wSecond, t.wMilliseconds, line);
    OutputDebugStringA(stamped.c_str());

    std::lock_guard lock(g_mutex);
    if (g_file.is_open()) {
        // Flushed per line so the last line survives a crash.
        g_file << stamped << std::flush;
    }
}

std::string utf8(const std::filesystem::path& path) {
    const std::wstring wide = path.wstring();
    if (wide.empty()) {
        return {};
    }
    const int size = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
                                         nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), result.data(), size,
                        nullptr, nullptr);
    return result;
}

}  // namespace logger
