// Copyright (c) 2026 Viktor Sharha
// Portions adapted from rdr2-ultrawide-proper-black-bar-fix, Copyright (c) 2026 rschi (MIT).

#pragma once

#include <filesystem>
#include <format>
#include <string>
#include <string_view>
#include <utility>
#include <windows.h>

// Not `log`: <math.h> already declares ::log.
namespace logger {

// While disabled, open() and write() do nothing. Enabled by default.
void set_enabled(bool enabled);

// Truncates and opens "<asi name>.log" next to the .asi at `self`. The game folder
// can be read-only for normal users, so on failure it uses
// %LOCALAPPDATA%\CutsceneFit instead. Every line is also sent to OutputDebugString.
// Returns false if no location was writable.
bool open(HMODULE self);

// The directory holding the log, empty if open() failed.
std::filesystem::path directory();

void write(std::string_view line);

template <typename... Args>
void info(std::format_string<Args...> fmt, Args&&... args) {
    write(std::format(fmt, std::forward<Args>(args)...));
}

std::string utf8(const std::filesystem::path& path);

}  // namespace logger
