// Copyright (c) 2026 Viktor Sharha

#pragma once

#include <cstdint>

#include "game.h"

namespace zoom {

enum class Start {
    // Active from the first camera cut after the letterbox appears until it goes.
    Cut,
    // Active whenever the letterbox is enabled.
    Letterbox,
};

const char* name(Start start);

// Creates the hook on the viewport builder without enabling it.
bool create(const game::Addresses& addresses, Start start);

// Whether the field of view is being narrowed, and how many times that has changed
// since start. Both are written by the game's thread.
bool active();
std::uint32_t transitions();

// When the bars start to slide out, the zoom either switches off at once (a camera cut
// on that frame or the one before) or eases out with the bars. These count each case.
std::uint32_t slide_out_exits();
std::uint32_t ease_outs();

struct Fov {
    float input = 0.0f;
    float output = 0.0f;
};
// The last pair written while active, in degrees.
Fov last_fov();

}  // namespace zoom
