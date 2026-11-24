// Copyright (c) 2026 Viktor Sharha

#pragma once

#include <string>
#include <vector>

#include "zoom.h"

namespace settings {

struct Settings {
    bool enabled = true;
    bool bars = true;
    int toggle_key = 0;
    zoom::Start zoom_start = zoom::Start::Cut;
    double aspect = 0.0;
    bool log = true;

    // Problems found while reading, for the log once it is open.
    std::vector<std::string> warnings;
};

// Reads section [CutsceneFit] of the ini file at `path`. Missing or invalid values keep
// their defaults.
Settings read(const std::wstring& path);

}  // namespace settings
