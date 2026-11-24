// Copyright (c) 2026 Viktor Sharha
// Portions adapted from rdr2-ultrawide-proper-black-bar-fix, Copyright (c) 2026 rschi (MIT).

#pragma once

#include <cstdint>

#include "common/mem.h"

namespace game {

constexpr const char* kExpectedVersion = "1.0.1491.50";

struct Addresses {
    // The function that builds the viewport from the camera frame, called once per
    // frame.
    std::uintptr_t zoom = 0;
    // The function that computes the letterbox state once per frame.
    std::uintptr_t update = 0;
    // The function that draws the letterbox bars from the drawn copy of that state.
    std::uintptr_t draw = 0;
    // The instruction `mov byte ptr [anchor], 0FFh` in the update; its immediate is
    // the bar alpha.
    std::uintptr_t alpha_store = 0;
    // The alpha byte of the computed letterbox state (see layout.h).
    std::uintptr_t anchor = 0;
};

enum class Resolve {
    Found,
    // At least one pattern matched nothing and none matched more than once. The code
    // may not be unpacked yet.
    Missing,
    // A pattern matched more than once, or the matches disagree about the layout.
    Failed,
};

// Searches every executable section of `module` and logs each pattern's result.
// With `verbose` false only the patterns that did not match exactly once are logged.
Resolve resolve(const mem::Region& module, Addresses& out, bool verbose);

}  // namespace game
