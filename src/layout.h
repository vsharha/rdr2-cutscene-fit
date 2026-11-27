// Copyright (c) 2026 Viktor Sharha
// Portions adapted from rdr2-ultrawide-proper-black-bar-fix, Copyright (c) 2026 rschi (MIT).

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

// The letterbox state of RDR2.exe 1.0.1491.50. The game computes it once per frame
// in the letterbox update, then copies it 0x20 bytes on to the "drawn" copy that
// DrawLetterbox and the renderer read. Offsets are relative to the alpha byte of the
// computed copy, which the game stores 0xFF into at the end of the update.
namespace letterbox {

// Computed copy.
constexpr std::ptrdiff_t kFlag = -0xA;           // u8, set while a caller requests letterboxing
constexpr std::ptrdiff_t kWeight = -8;           // float, 0 in gameplay, 1 fully letterboxed
constexpr std::ptrdiff_t kAlpha = 0;             // u8
constexpr std::ptrdiff_t kEnable = 1;            // u8
constexpr std::ptrdiff_t kTopBottom = 4;         // float, computed height of each top and bottom bar

// Drawn copy.
constexpr std::ptrdiff_t kDrawnAlpha = 0x20;     // u8, the renderer scissors only while 255
constexpr std::ptrdiff_t kDrawnEnable = 0x21;    // u8
constexpr std::ptrdiff_t kDrawnTopBottom = 0x24; // float, height of each top and bottom bar
constexpr std::ptrdiff_t kDrawnSideWidth = 0x28; // float, width of each side bar
constexpr std::ptrdiff_t kDrawnSideOn = 0x2C;    // u8
// u8. Set in cutscenes and the cinematic camera. Clear when the game uses the letterbox
// to box a menu to 16:9 with side bars only, such as the general store's outfit menu.
constexpr std::ptrdiff_t kDrawnTopBottomOn = 0x2D;

}  // namespace letterbox

template <typename T>
T load(std::uintptr_t address) {
    T value;
    std::memcpy(&value, reinterpret_cast<const void*>(address), sizeof(value));
    return value;
}

template <typename T>
void store(std::uintptr_t address, T value) {
    std::memcpy(reinterpret_cast<void*>(address), &value, sizeof(value));
}
