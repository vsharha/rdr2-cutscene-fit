// Copyright (c) 2026 Viktor Sharha
// Portions adapted from rdr2-ultrawide-proper-black-bar-fix, Copyright (c) 2026 rschi (MIT).

#pragma once

#include <cstdint>

#include "game.h"

namespace bars {

// How the side bars are sized while the game boxes a menu, such as a shop, to 16:9:
// a letterbox with side bars only. Menus are never zoomed.
enum class MenuFrame {
    // The cutscene's side bars, framing 2.35:1 on screens wider than that.
    Cinematic,
    // The game's own side bars, framing 16:9.
    Original,
    // No side bars.
    Off,
};

const char* name(MenuFrame frame);

// Creates the hooks on the letterbox update and drawing without enabling them.
bool create(const game::Addresses& addresses, MenuFrame menu_frame);

// Hides the bars by setting their alpha to zero, or shows them again with the game's
// own 0xFF. Hidden, the immediate of the instruction that stores the computed alpha is
// patched, and the update and drawing hooks also store zero into the computed and drawn
// alpha. Showing restores the immediate and stores 0xFF once into both alphas, and the
// hooks then leave the alpha alone. Logs when the state changes.
void set_hidden(bool hidden);
bool hidden();

// Puts back the alpha immediate changed by the plugin and stops hiding.
void restore();

struct Counters {
    // Frames the update ran with a letterbox weight above zero.
    std::uint32_t letterbox_frames = 0;
    // Of those, frames on which the update found the letterbox flag set.
    std::uint32_t flagged_frames = 0;
};
Counters counters();
#ifdef CUTSCENEFIT_COMPARE
std::uint32_t drawn_frames();
#endif

}  // namespace bars
