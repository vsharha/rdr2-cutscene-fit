// Copyright (c) 2026 Viktor Sharha
// Portions adapted from rdr2-ultrawide-proper-black-bar-fix, Copyright (c) 2026 rschi (MIT).

#include "bars.h"

#include <algorithm>
#include <atomic>
#include <windows.h>

#include <MinHook.h>

#include "common/log.h"
#include "inflight.h"
#include "layout.h"
#include "state.h"

namespace bars {
namespace {

// The immediate of `C6 05 <disp32> <imm8>`.
constexpr std::size_t kImmediateOffset = 6;
constexpr std::uint8_t kShownAlpha = 0xFF;
constexpr std::uint8_t kHiddenAlpha = 0x00;
// While the letterbox weight is positive the update replaces a computed height of exactly
// zero with 1.0, a bar covering the whole screen, so the plugin never writes zero.
constexpr float kMinTopBottom = 6.25e-5f;

using VoidFn = void (*)();

VoidFn g_update_original = nullptr;
VoidFn g_draw_original = nullptr;
std::uintptr_t g_anchor = 0;
std::uintptr_t g_immediate = 0;
std::atomic<bool> g_hidden{false};

std::atomic<std::uint32_t> g_letterbox_frames{0};
std::atomic<std::uint32_t> g_flagged_frames{0};
#ifdef CUTSCENEFIT_COMPARE
std::atomic<std::uint32_t> g_drawn_frames{0};
#endif

bool write_byte(std::uintptr_t address, std::uint8_t value) {
    void* const target = reinterpret_cast<void*>(address);
    DWORD previous = 0;
    if (!VirtualProtect(target, 1, PAGE_EXECUTE_READWRITE, &previous)) {
        logger::info("VirtualProtect on +0x{:X} failed, error {}", address, GetLastError());
        return false;
    }
    *static_cast<volatile std::uint8_t*>(target) = value;
    DWORD ignored = 0;
    VirtualProtect(target, 1, previous, &ignored);
    FlushInstructionCache(GetCurrentProcess(), target, 1);
    return true;
}

// Puts the alpha the game would have stored into both copies, without waiting for the
// game to execute the storing instruction again.
// The computed copy goes first: if the game copies it to the drawn copy between the two
// stores, the drawn copy still receives 255.
void store_shown_alpha() {
    store<std::uint8_t>(g_anchor + letterbox::kAlpha, kShownAlpha);
    store<std::uint8_t>(g_anchor + letterbox::kDrawnAlpha, kShownAlpha);
}

void update_detour() {
    [[maybe_unused]] const inflight::Guard guard;
    g_update_original();

    // The instruction patch makes the computed alpha zero from the moment the game
    // stores it; this store keeps it zero if anything else has written 255.
    if (g_hidden.load(std::memory_order_relaxed)) {
        store<std::uint8_t>(g_anchor + letterbox::kAlpha, kHiddenAlpha);
    }

    // The game's box aspect snaps to preset ratios, so its calculated height is wrong
    // on some ultrawide screens. Write the screen-based height into the computed copy.
    if (state::applicable() && load<std::uint8_t>(g_anchor + letterbox::kEnable) != 0) {
        const float weight = load<float>(g_anchor + letterbox::kWeight);
        if (weight > 0.0f) {
            const float height = std::max(state::top_bottom_fraction(), kMinTopBottom) * weight;
            store(g_anchor + letterbox::kTopBottom, height);
        }
    }

    if (load<float>(g_anchor + letterbox::kWeight) > 0.0f) {
        g_letterbox_frames.fetch_add(1, std::memory_order_relaxed);
        if (load<std::uint8_t>(g_anchor + letterbox::kFlag) != 0) {
            g_flagged_frames.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

// Side bars have no input in the game's computation, so their width is written into
// the drawn copy, which the game has just filled and nothing else touches before the
// drawing reads it. Below 2.35:1 this removes the game's own side bars.
//
// While the bars are hidden the drawn alpha is also zeroed here, so the drawing never
// reads an opaque bar, including on a frame where the update did not run (paused, or
// the first frame after unpausing).
void draw_detour() {
    [[maybe_unused]] const inflight::Guard guard;
#ifdef CUTSCENEFIT_COMPARE
    g_drawn_frames.fetch_add(1, std::memory_order_relaxed);
#endif
    if (g_hidden.load(std::memory_order_relaxed)) {
        store<std::uint8_t>(g_anchor + letterbox::kDrawnAlpha, kHiddenAlpha);
    }
    if (state::applicable() && load<std::uint8_t>(g_anchor + letterbox::kDrawnEnable) != 0) {
        const float weight = load<float>(g_anchor + letterbox::kWeight);
        const float width = weight > 0.0f ? state::side_fraction() * weight : 0.0f;
        store(g_anchor + letterbox::kDrawnSideWidth, width);
        if (width > 0.0f) {
            store<std::uint8_t>(g_anchor + letterbox::kDrawnSideOn, 1);
        }
    }
    g_draw_original();
}

bool create_hook(const char* name, std::uintptr_t target, void* detour, VoidFn& original) {
    const MH_STATUS status = MH_CreateHook(reinterpret_cast<LPVOID>(target), detour,
                                           reinterpret_cast<LPVOID*>(&original));
    if (status != MH_OK) {
        logger::info("{} hook: MH_CreateHook failed: {}", name, MH_StatusToString(status));
        return false;
    }
    return true;
}

}  // namespace

bool create(const game::Addresses& addresses) {
    g_anchor = addresses.anchor;
    g_immediate = addresses.alpha_store + kImmediateOffset;
    return create_hook("letterbox update", addresses.update, reinterpret_cast<void*>(&update_detour),
                       g_update_original) &&
           create_hook("letterbox drawing", addresses.draw, reinterpret_cast<void*>(&draw_detour),
                       g_draw_original);
}

void set_hidden(bool hidden) {
    if (g_immediate == 0 || hidden == g_hidden.load()) {
        return;
    }
    if (write_byte(g_immediate, hidden ? kHiddenAlpha : kShownAlpha)) {
        g_hidden.store(hidden);
        if (!hidden) {
            store_shown_alpha();
        }
        logger::info("bar alpha patch: bars {}", hidden ? "hidden" : "shown");
    }
}

bool hidden() {
    return g_hidden.load();
}

void restore() {
    if (g_immediate != 0 && g_hidden.load() && write_byte(g_immediate, kShownAlpha)) {
        g_hidden.store(false);
        store_shown_alpha();
    }
}

Counters counters() {
    return {g_letterbox_frames.load(std::memory_order_relaxed),
            g_flagged_frames.load(std::memory_order_relaxed)};
}

#ifdef CUTSCENEFIT_COMPARE
std::uint32_t drawn_frames() { return g_drawn_frames.load(std::memory_order_relaxed); }
#endif

}  // namespace bars
