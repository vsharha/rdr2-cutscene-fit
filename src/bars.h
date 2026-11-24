// Copyright (c) 2026 Viktor Sharha
// Portions adapted from rdr2-ultrawide-proper-black-bar-fix, Copyright (c) 2026 rschi (MIT).

#pragma once

#include <cstdint>

#include "game.h"

namespace bars {

// Creates the hooks on the letterbox update and drawing without enabling them.
bool create(const game::Addresses& addresses);

// Sets the bar alpha to zero (hidden) or back to the game's 0xFF (shown) by patching
// the immediate of the instruction that stores it. Logs when the state changes.
void set_hidden(bool hidden);
bool hidden();

// Puts back everything the plugin changed: the alpha immediate and the target aspects.
void restore();

struct Counters {
    // Frames the update ran with a letterbox weight above zero.
    std::uint32_t letterbox_frames = 0;
    // Of those, frames on which the update found the letterbox flag set and so took
    // its target from the request native's variable.
    std::uint32_t flagged_frames = 0;
};
Counters counters();
#ifdef CUTSCENEFIT_COMPARE
std::uint32_t drawn_frames();
#endif

}  // namespace bars
