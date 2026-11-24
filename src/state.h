// Copyright (c) 2026 Viktor Sharha

#pragma once

// The screen aspect and the values derived from it. The worker thread sets it; the
// game's threads read the derived values without locks.
namespace state {

#ifdef CUTSCENEFIT_COMPARE
enum class CompareMode { Normal, Game, BarsRemoved, Fit, FitNoBars };
#endif

constexpr double kReferenceAspect = 16.0 / 9.0;
// The aspect of the cutscene frame.
constexpr double kFilmAspect = 2.35;

void set_aspect(double aspect);
double aspect();

// False until an aspect is set, and while the screen is no wider than 16:9. The zoom
// and the bar sizes are left alone then; hiding the bars does not depend on it.
bool applicable();
#ifdef CUTSCENEFIT_COMPARE
void set_compare_mode(CompareMode mode);
CompareMode compare_mode();
bool force_zoom();
#else
inline bool force_zoom() { return false; }
#endif

// k in vFOV' = 2 * atan(k * tan(vFOV / 2)).
float zoom_scale();

// Height of each top and bottom bar as a fraction of the screen height.
float top_bottom_fraction();

// Width of each side bar as a fraction of the screen width, before the letterbox
// weight is applied.
float side_fraction();

}  // namespace state
