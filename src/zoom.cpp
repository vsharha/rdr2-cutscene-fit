// Copyright (c) 2026 Viktor Sharha

#include "zoom.h"

#include <atomic>
#include <cmath>
#include <cstring>

#include <MinHook.h>

#include "common/log.h"
#include "layout.h"
#include "state.h"

namespace zoom {

const char* name(Start start) {
    switch (start) {
        case Start::Cut:
            return "cut";
        case Start::Letterbox:
            return "letterbox";
    }
    return "?";
}

namespace {

// Offsets in the camera frame passed to the viewport builder.
constexpr std::uintptr_t kFovOffset = 0x60;  // float, vertical field of view in degrees
constexpr std::uintptr_t kFlagsOffset = 0xCC;  // u16
// Set, with 0x40, on the frame of a camera cut.
constexpr std::uint16_t kCutFlag = 0x80;
constexpr double kPi = 3.14159265358979323846;

using Fn = char (*)(std::int64_t*, std::uint8_t*, std::int64_t*, std::uint8_t*);

Fn g_original = nullptr;
std::uintptr_t g_enable = 0;
Start g_start = Start::Cut;
std::atomic<bool> g_active{false};
std::atomic<std::uint32_t> g_transitions{0};
std::atomic<float> g_fov_input{0.0f};
std::atomic<float> g_fov_output{0.0f};

float correct(float fov, float scale) {
    const double half = static_cast<double>(fov) * (kPi / 360.0);
    return static_cast<float>(2.0 * std::atan(static_cast<double>(scale) * std::tan(half)) *
                              (180.0 / kPi));
}

void set_active(bool active) {
    if (g_active.load(std::memory_order_relaxed) != active) {
        g_active.store(active, std::memory_order_relaxed);
        g_transitions.fetch_add(1, std::memory_order_relaxed);
    }
}

// Runs on a game thread once per frame: no allocation, logging or locks.
char detour(std::int64_t* p1, std::uint8_t* frame, std::int64_t* p3, std::uint8_t* owner) {
    if (frame == nullptr || !state::applicable()) {
        set_active(false);
        return g_original(p1, frame, p3, owner);
    }

    const bool letterbox = load<std::uint8_t>(g_enable) != 0;
    bool active = g_active.load(std::memory_order_relaxed);
    if (state::force_zoom() || g_start == Start::Letterbox) {
        active = letterbox;
    } else if (!letterbox) {
        active = false;
    } else if (!active) {
        active = (load<std::uint16_t>(reinterpret_cast<std::uintptr_t>(frame) + kFlagsOffset) & kCutFlag) != 0;
    }
    set_active(active);

    const auto fov_address = reinterpret_cast<std::uintptr_t>(frame) + kFovOffset;
    const float input = load<float>(fov_address);
    const float scale = state::zoom_scale();
    if (!active || !(input > 0.0f && input < 180.0f) || !(scale > 0.0f && std::isfinite(scale))) {
        return g_original(p1, frame, p3, owner);
    }

    const float corrected = correct(input, scale);
    store(fov_address, corrected);
    const char result = g_original(p1, frame, p3, owner);
    // The gameplay camera reads this field back as its next input, so leaving the
    // corrected value there compounds the zoom every frame.
    store(fov_address, input);

    g_fov_input.store(input, std::memory_order_relaxed);
    g_fov_output.store(corrected, std::memory_order_relaxed);
    return result;
}

}  // namespace

bool create(const game::Addresses& addresses, Start start) {
    g_enable = addresses.anchor + letterbox::kDrawnEnable;
    g_start = start;
    const MH_STATUS status = MH_CreateHook(reinterpret_cast<LPVOID>(addresses.zoom),
                                           reinterpret_cast<LPVOID>(&detour),
                                           reinterpret_cast<LPVOID*>(&g_original));
    if (status != MH_OK) {
        logger::info("zoom hook: MH_CreateHook failed: {}", MH_StatusToString(status));
        return false;
    }
    return true;
}

bool active() {
    return g_active.load(std::memory_order_relaxed);
}

std::uint32_t transitions() {
    return g_transitions.load(std::memory_order_relaxed);
}

Fov last_fov() {
    return {g_fov_input.load(std::memory_order_relaxed), g_fov_output.load(std::memory_order_relaxed)};
}

}  // namespace zoom
