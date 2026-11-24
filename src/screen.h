// Copyright (c) 2026 Viktor Sharha

#pragma once

#include <optional>
#include <string>

namespace screen {

struct Measurement {
    double aspect = 0.0;
    int width = 0;
    int height = 0;
    std::string source;
};

// The client area of the largest visible window of this process. Empty if there is
// none, for example while the window is minimised or not yet created.
std::optional<Measurement> game_window();

// The primary monitor's resolution.
std::optional<Measurement> primary_monitor();

}  // namespace screen
