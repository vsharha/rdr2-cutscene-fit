// Copyright (c) 2026 Viktor Sharha

// CutsceneFit: shows cutscenes at their intended 2.35:1 framing on screens wider than
// 16:9, as large as the screen allows and never cropped, by narrowing the cutscene
// camera's field of view and resizing the game's own letterbox bars to fill the rest.
// On screens no wider than 16:9 the zoom and the bar sizes are left alone; hiding the
// bars works on every screen.
//
// Settings are read once at start from "CutsceneFit.ini" next to CutsceneFit.asi,
// section [CutsceneFit]:
//   enabled      1 loads the plugin, 0 does nothing. Default: 1.
//   bars         1 shows the bars at start, 0 hides them (the scene behind them is then
//                rendered). Default: 1.
//   toggle_key   key that shows or hides the bars: a name (F1..F24, A..Z, 0..9, End,
//                Home, Insert, Delete, PageUp, PageDown, arrows, ...) or a virtual-key
//                code such as 0x23. Default: End.
//   zoom_start   cut: the field of view is narrowed from the first camera cut after the
//                letterbox appears. letterbox: from when the letterbox is enabled.
//                Only a letterbox with top and bottom bars counts; one with side bars
//                only boxes a menu to 16:9 and is left to the game.
//                Either way the zoom eases out as the bars slide out, or ends at once
//                if a camera cut came with them. Default: cut.
//   menu_frame   side bars in menus the game boxes to 16:9, such as shops. cinematic:
//                the cutscene's, framing 2.35:1. original: the game's own, framing 16:9.
//                off: none. Menus are never zoomed. Default: cinematic.
//   aspect       screen aspect ratio. 0 measures the game window's client area once a
//                second. Default: 0.
//   log          1 writes CutsceneFit.log next to the .asi, 0 writes nothing. Default: 1.
// Compare-build only:
//   compare_key      starts or cancels the four-state screenshot sequence. Default: F11.
//   compare_hold_ms  state hold in milliseconds without a capture response. Default: 1500.
//   compare_freeze   1 pauses between shots and unpauses briefly for each capture through Script Hook.
//                   0 runs without pausing. Default: 1.

#include <atomic>
#include <chrono>
#include <cmath>
#include <exception>
#include <filesystem>
#include <format>
#include <optional>
#include <string>
#include <thread>
#include <windows.h>

#include <MinHook.h>

#include "bars.h"
#ifdef CUTSCENEFIT_COMPARE
#include "compare.h"
#endif
#include "common/log.h"
#include "common/mem.h"
#include "common/paths.h"
#include "game.h"
#ifdef CUTSCENEFIT_RELOAD
#include "inflight.h"
#endif
#include "screen.h"
#include "settings.h"
#include "state.h"
#include "video.h"
#include "zoom.h"

namespace {

using Clock = std::chrono::steady_clock;

constexpr auto kPollInterval = std::chrono::milliseconds(20);
constexpr auto kAspectInterval = std::chrono::seconds(1);
constexpr auto kTransitionInterval = std::chrono::seconds(1);
constexpr auto kCounterInterval = std::chrono::seconds(10);
constexpr auto kRetryInterval = std::chrono::seconds(2);
constexpr int kMaxAttempts = 30;
// Polls without a game window before the primary monitor is used instead.
constexpr int kWindowMisses = 10;

std::atomic<bool> g_stop{false};
std::atomic<bool> g_hooks_created{false};

#ifdef CUTSCENEFIT_RELOAD
// Set by cutscenefit_start. The loaded file is a temporary copy, so its own path names
// neither the ini nor the log.
HMODULE g_self = nullptr;
std::filesystem::path g_asi;
bool g_append = false;
HANDLE g_thread = nullptr;
#endif

bool pressed(int key, bool& was_down) {
    const bool down = (GetAsyncKeyState(key) & 0x8000) != 0;
    const bool edge = down && !was_down;
    was_down = down;
    return edge;
}

// Sleeps up to `duration`, returning early with true when the plugin is stopping.
bool sleep_or_stop(std::chrono::milliseconds duration) {
    const auto until = Clock::now() + duration;
    while (Clock::now() < until) {
        if (g_stop.load()) {
            return true;
        }
        std::this_thread::sleep_for(kPollInterval);
    }
    return g_stop.load();
}

class AspectTracker {
public:
    explicit AspectTracker(double from_ini) : from_ini_(from_ini) {}

    void refresh() {
        std::optional<screen::Measurement> m;
        if (from_ini_ > 0.0) {
            m = screen::Measurement{from_ini_, 0, 0, "ini"};
        } else {
            m = screen::game_window();
            if (m) {
                seen_window_ = true;
            } else if (!seen_window_ && ++misses_ >= kWindowMisses) {
                m = screen::primary_monitor();
            }
        }
        if (!m || (std::abs(m->aspect - state::aspect()) < 1e-4 && m->source == source_)) {
            return;
        }
        source_ = m->source;
        state::set_aspect(m->aspect);

        const std::string size = m->width > 0 ? std::format(" {}x{}", m->width, m->height) : "";
        if (state::applicable()) {
            logger::info("aspect {:.4f} ({}{}), zoom k {:.5f}, top/bottom fraction {:.5f}, side bars "
                         "{:.5f} of the width each",
                         m->aspect, m->source, size, state::zoom_scale(), state::top_bottom_fraction(),
                         state::side_fraction());
        } else {
            logger::info("aspect {:.4f} ({}{}): not wider than 16:9, zoom and bar sizes are left alone",
                         m->aspect, m->source, size);
        }
    }

private:
    double from_ini_;
    std::string source_;
    bool seen_window_ = false;
    int misses_ = 0;
};

// Logs zoom changes from the worker thread. The hook only counts them, and several can
// pass between two polls, so lines are limited to one per second and say how many
// changes they cover.
class ZoomLog {
public:
    void tick(Clock::time_point now) {
        const std::uint32_t count = zoom::transitions();
        if (count == seen_ || now - last_ < kTransitionInterval) {
            return;
        }
        const zoom::Fov fov = zoom::last_fov();
        logger::info("zoom {} ({} change(s) since the last line; {} ease-out(s) and {} immediate slide-out exit(s) in "
                     "total; last field of view {:.3f} -> {:.3f})",
                     zoom::active() ? "active" : "inactive", count - seen_, zoom::ease_outs(),
                     zoom::slide_out_exits(),
                     fov.input, fov.output);
        seen_ = count;
        last_ = now;
    }

private:
    std::uint32_t seen_ = 0;
    Clock::time_point last_{};
};

class CounterLog {
public:
    void tick(Clock::time_point now) {
        if (now - last_ < kCounterInterval) {
            return;
        }
        const bars::Counters c = bars::counters();
        if (c.letterbox_frames == seen_.letterbox_frames) {
            return;
        }
        logger::info("letterbox: {} frame(s) so far, {} of them with the letterbox flag set",
                     c.letterbox_frames, c.flagged_frames);
        seen_ = c;
        last_ = now;
    }

private:
    bars::Counters seen_;
    Clock::time_point last_{};
};

void shutdown() {
#ifdef CUTSCENEFIT_COMPARE
    compare::shutdown();
#endif
    bars::restore();
    if (g_hooks_created.exchange(false)) {
        MH_DisableHook(MH_ALL_HOOKS);
#ifdef CUTSCENEFIT_RELOAD
        // MH_Uninitialize frees the trampolines a game thread may still be running.
        inflight::drain();
#endif
        MH_Uninitialize();
    }
}

bool install(const mem::Region& module, const settings::Settings& s) {
    game::Addresses addresses;
    game::Resolve result = game::Resolve::Missing;
    for (int attempt = 1; attempt <= kMaxAttempts; ++attempt) {
        const bool verbose = attempt == 1 || attempt == kMaxAttempts;
        result = game::resolve(module, addresses, verbose);
        if (result == game::Resolve::Found && !verbose) {
            game::resolve(module, addresses, true);
        }
        if (result != game::Resolve::Missing) {
            break;
        }
        if (attempt == 1) {
            logger::info("retrying every {} s for up to {} attempts", kRetryInterval.count(), kMaxAttempts);
        }
        if (sleep_or_stop(std::chrono::duration_cast<std::chrono::milliseconds>(kRetryInterval))) {
            return false;
        }
    }
    if (result != game::Resolve::Found) {
        logger::info("not installing any hook");
        return false;
    }

    if (const MH_STATUS status = MH_Initialize(); status != MH_OK) {
        logger::info("MH_Initialize failed: {}", MH_StatusToString(status));
        return false;
    }
    g_hooks_created.store(true);
    if (!zoom::create(addresses, s.zoom_start) || !bars::create(addresses, s.menu_frame)) {
        shutdown();
        logger::info("not installing any hook");
        return false;
    }
    if (!video::create()) {
        logger::info("video hooks not installed; the plugin works without them");
    }
    if (const MH_STATUS status = MH_EnableHook(MH_ALL_HOOKS); status != MH_OK) {
        logger::info("MH_EnableHook failed: {}", MH_StatusToString(status));
        shutdown();
        return false;
    }
    logger::info("hooks installed: zoom +0x{:X}, letterbox update +0x{:X}, letterbox drawing +0x{:X}",
                 addresses.zoom - module.base, addresses.update - module.base,
                 addresses.draw - module.base);
    return true;
}

DWORD WINAPI run(LPVOID parameter) {
    [[maybe_unused]] const auto self = static_cast<HMODULE>(parameter);
    try {
#ifdef CUTSCENEFIT_RELOAD
        const std::filesystem::path asi = g_asi;
        const bool append = g_append;
#else
        const std::filesystem::path asi = paths::module_path(self);
        const bool append = false;
#endif
        const std::wstring ini = std::filesystem::path(asi).replace_extension(L".ini").wstring();
        const settings::Settings s = settings::read(ini);

        logger::set_enabled(s.log);
        logger::open(asi, append);
        for (const std::string& warning : s.warnings) {
            logger::info("settings: {}", warning);
        }

        const mem::Region module = mem::main_module();
        if (!module) {
            logger::info("the main module has no valid PE header");
            return 1;
        }
        const std::string version = mem::main_module_version();
        logger::info("module base 0x{:X}, size 0x{:X}, game version {}", module.base, module.size, version);
        if (version != game::kExpectedVersion) {
            logger::info("WARNING: offsets and patterns are for {}", game::kExpectedVersion);
        }
        logger::info("settings: enabled={} bars={} toggle_key=0x{:X} zoom_start={} menu_frame={} aspect={} log={}",
                     s.enabled, s.bars, s.toggle_key, zoom::name(s.zoom_start), bars::name(s.menu_frame), s.aspect,
                     s.log);
        if (!s.enabled) {
            logger::info("disabled in the ini; doing nothing");
            return 0;
        }

        if (!install(module, s)) {
            return 1;
        }
#ifdef CUTSCENEFIT_COMPARE
        compare::initialize(self, asi, s.compare_freeze);
        logger::info("compare settings: key=0x{:X} hold_ms={} freeze={}", s.compare_key,
                     s.compare_hold_ms, s.compare_freeze);
#endif

        AspectTracker aspect(s.aspect);
        ZoomLog zoom_log;
        CounterLog counter_log;
        bool hide = !s.bars;
        bool key_down = false;
#ifdef CUTSCENEFIT_COMPARE
        bool compare_key_down = false;
#endif
        auto last_aspect = Clock::now() - kAspectInterval;

        while (!g_stop.load()) {
            const auto now = Clock::now();
            if (now - last_aspect >= kAspectInterval) {
                aspect.refresh();
                last_aspect = now;
            }
            if (pressed(s.toggle_key, key_down)) {
                hide = !hide;
                logger::info("toggle key: bars {}", hide ? "hidden" : "shown");
            }
#ifdef CUTSCENEFIT_COMPARE
            if (pressed(s.compare_key, compare_key_down)) {
                compare::run(s.compare_key, s.compare_hold_ms, s.compare_freeze);
                compare_key_down = false;
            }
#endif
            bars::set_hidden(hide);
            zoom_log.tick(now);
            counter_log.tick(now);
            std::this_thread::sleep_for(kPollInterval);
        }
        return 0;
    } catch (const std::exception& e) {
        logger::info("failed: {}", e.what());
        return 1;
    }
}

}  // namespace

#ifdef CUTSCENEFIT_RELOAD
// The reload loader (tools/reload) calls these instead of the plugin starting itself
// when it is loaded. `asi` is the path the ini and the log are named after. A nonzero
// `reload` appends to the log instead of truncating it. Returns nonzero if the worker
// thread started.
extern "C" __declspec(dllexport) int cutscenefit_start(const wchar_t* asi, int reload) {
    if (g_thread != nullptr || asi == nullptr) {
        return 0;
    }
    try {
        g_asi = asi;
    } catch (const std::exception&) {
        return 0;
    }
    g_append = reload != 0;
    g_thread = CreateThread(nullptr, 0, run, g_self, 0, nullptr);
    return g_thread != nullptr ? 1 : 0;
}

// Stops the worker thread, removes every hook and patch, and waits until no game
// thread is inside a hook. Afterwards the DLL can be freed.
extern "C" __declspec(dllexport) void cutscenefit_stop() {
    g_stop.store(true);
#ifdef CUTSCENEFIT_COMPARE
    // A running sequence watches only its own cancel flag, so it must be cancelled
    // before the worker thread can be waited for.
    compare::shutdown();
#endif
    if (g_thread != nullptr) {
        WaitForSingleObject(g_thread, INFINITE);
        CloseHandle(g_thread);
        g_thread = nullptr;
    }
    shutdown();
    logger::info("stopped for a reload");
}
#endif

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(instance);
#ifdef CUTSCENEFIT_RELOAD
        g_self = instance;
#else
        if (HANDLE thread = CreateThread(nullptr, 0, run, instance, 0, nullptr)) {
            CloseHandle(thread);
        }
#endif
    } else if (reason == DLL_PROCESS_DETACH && reserved == nullptr) {
        g_stop.store(true);
        shutdown();
    }
    return TRUE;
}
