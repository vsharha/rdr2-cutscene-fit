// Copyright (c) 2026 Viktor Sharha

#include "state.h"

#include <algorithm>
#include <atomic>

namespace state {
namespace {

// Screens within this of 16:9 count as 16:9, so that 1366x768 and similar are left alone.
constexpr double kEpsilon = 0.01;

// 1.778 and not 16/9: the game replaces a top and bottom bar height of exactly zero
// with a bar covering the whole screen, and a target of exactly 16/9 gives zero.
constexpr double kLowestTarget = 1.778;

std::atomic<double> g_aspect{0.0};
std::atomic<bool> g_applicable{false};
std::atomic<float> g_zoom_scale{1.0f};
std::atomic<float> g_target{static_cast<float>(kFilmAspect)};
std::atomic<float> g_side{0.0f};
#ifdef CUTSCENEFIT_COMPARE
std::atomic<CompareMode> g_compare_mode{CompareMode::Normal};
#endif

}  // namespace

void set_aspect(double aspect) {
    const bool wide = aspect > kReferenceAspect + kEpsilon;
    if (wide) {
        g_zoom_scale.store(static_cast<float>(kReferenceAspect / std::min(aspect, kFilmAspect)));
        g_target.store(static_cast<float>(std::max(kFilmAspect * kReferenceAspect / aspect, kLowestTarget)));
        g_side.store(static_cast<float>(std::max(1.0 - kFilmAspect / aspect, 0.0) * 0.5));
    }
    g_aspect.store(aspect);
    g_applicable.store(wide);
}

double aspect() {
    return g_aspect.load();
}

bool applicable() {
#ifdef CUTSCENEFIT_COMPARE
    const CompareMode mode = g_compare_mode.load(std::memory_order_relaxed);
    if (mode == CompareMode::Game || mode == CompareMode::BarsRemoved) return false;
#endif
    return g_applicable.load(std::memory_order_relaxed);
}

#ifdef CUTSCENEFIT_COMPARE
void set_compare_mode(CompareMode mode) { g_compare_mode.store(mode, std::memory_order_relaxed); }
CompareMode compare_mode() { return g_compare_mode.load(std::memory_order_relaxed); }
bool force_zoom() {
    const CompareMode mode = compare_mode();
    return mode == CompareMode::Fit || mode == CompareMode::FitNoBars;
}
#endif

float zoom_scale() {
    return g_zoom_scale.load(std::memory_order_relaxed);
}

float target_aspect() {
    return g_target.load(std::memory_order_relaxed);
}

float side_fraction() {
    return g_side.load(std::memory_order_relaxed);
}

}  // namespace state
