// Copyright (c) 2026 Viktor Sharha
// Portions adapted from rdr2-ultrawide-proper-black-bar-fix, Copyright (c) 2026 rschi (MIT).

#include "bars.h"

#include <atomic>
#include <windows.h>

#include <MinHook.h>

#include "common/log.h"
#include "layout.h"
#include "state.h"

namespace bars {
namespace {

// The immediate of `C6 05 <disp32> <imm8>`.
constexpr std::size_t kImmediateOffset = 6;
constexpr std::uint8_t kShownAlpha = 0xFF;
constexpr std::uint8_t kHiddenAlpha = 0x00;

using VoidFn = void (*)();

VoidFn g_update_original = nullptr;
VoidFn g_draw_original = nullptr;
std::uintptr_t g_anchor = 0;
std::uintptr_t g_immediate = 0;
std::atomic<bool> g_hidden{false};

// A float the game writes and the plugin overwrites with its own value for the duration
// of the update. The game's last value is kept and put back when the plugin stops
// overriding. Only the update hook and restore() use it.
struct Override {
    std::uintptr_t address = 0;
    float game = 0.0f;
    float written = 0.0f;
    bool replaced = false;

    void apply(float wanted) {
        const float current = load<float>(address);
        if (current == wanted) {
            return;
        }
        // Anything but our own last value was written by the game.
        if (!replaced || current != written) {
            game = current;
        }
        store(address, wanted);
        written = wanted;
        replaced = true;
    }

    void restore() {
        if (replaced && load<float>(address) == written) {
            store(address, game);
        }
        replaced = false;
    }
};

// The target aspect used while the letterbox flag is clear, and the one used while it
// is set.
Override g_target;
Override g_request_target;

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

// The targets are set before the update runs: it computes the bar height from them in
// the same call, including on the frame of a camera cut.
void update_detour() {
    if (state::applicable()) {
        const float wanted = state::target_aspect();
        g_target.apply(wanted);
        g_request_target.apply(wanted);
    } else {
        g_target.restore();
        g_request_target.restore();
    }

    g_update_original();

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
void draw_detour() {
#ifdef CUTSCENEFIT_COMPARE
    g_drawn_frames.fetch_add(1, std::memory_order_relaxed);
#endif
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
    g_target.address = addresses.anchor + letterbox::kTargetAspect;
    g_request_target.address = addresses.request_target;
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
        logger::info("bar alpha patch: bars {}", hidden ? "hidden" : "shown");
    }
}

bool hidden() {
    return g_hidden.load();
}

void restore() {
    if (g_immediate != 0 && g_hidden.load() && write_byte(g_immediate, kShownAlpha)) {
        g_hidden.store(false);
    }
    g_target.restore();
    g_request_target.restore();
}

Counters counters() {
    return {g_letterbox_frames.load(std::memory_order_relaxed),
            g_flagged_frames.load(std::memory_order_relaxed)};
}

#ifdef CUTSCENEFIT_COMPARE
std::uint32_t drawn_frames() { return g_drawn_frames.load(std::memory_order_relaxed); }
#endif

}  // namespace bars
