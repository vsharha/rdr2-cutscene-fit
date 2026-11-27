// Copyright (c) 2026 Viktor Sharha

#pragma once

#include <string>
#include <vector>

#include "bars.h"
#include "zoom.h"

namespace settings {

struct Settings {
    bool enabled = true;
    bool bars = true;
    int toggle_key = 0;
    zoom::Start zoom_start = zoom::Start::Cut;
    bars::MenuFrame menu_frame = bars::MenuFrame::Cinematic;
    double aspect = 0.0;
    bool log = true;
#ifdef CUTSCENEFIT_COMPARE
    int compare_key = 0;
    int compare_hold_ms = 1500;
    bool compare_freeze = true;
#endif

    // Problems found while reading, for the log once it is open.
    std::vector<std::string> warnings;
};

// Reads section [CutsceneFit] of the ini file at `path`. Missing or invalid values keep
// their defaults.
Settings read(const std::wstring& path);

}  // namespace settings
