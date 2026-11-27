// Copyright (c) 2026 Viktor Sharha

#pragma once

#include <cstdint>

// Watches the game's Bink video playback through the exports of bink2w64.dll.
namespace video {

// Creates hooks on BinkOpenWithOptions, BinkNextFrame and BinkClose without enabling
// them. Returns false, after logging why, when bink2w64.dll or an export is missing;
// the plugin works without these hooks.
bool create();

// Whether a video advanced a frame within the last kPlayingWindowMs, or the latest video
// opened has neither advanced nor closed and was opened less than kPendingLimitMs ago.
// The game opens a video seconds before its first frame. Videos are drawn as a 16:9
// picture with the game's own side bars covering the rest of the screen, so the plugin
// leaves the letterbox alone meanwhile.
constexpr std::uint64_t kPlayingWindowMs = 250;
constexpr std::uint64_t kPendingLimitMs = 10000;
bool playing();

}  // namespace video
