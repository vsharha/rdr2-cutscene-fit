// Copyright (c) 2026 Viktor Sharha

#include "compare.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <fstream>
#include <string>
#include <thread>
#include <windows.h>

#include "bars.h"
#include "common/log.h"
#include "state.h"

namespace compare {
namespace {

using Clock = std::chrono::steady_clock;
using Register = void (*)(HMODULE, void (*)());
using Unregister = void (*)(HMODULE);
using Wait = void (*)(DWORD);
using NativeInit = void (*)(std::uint64_t);
using NativePush = void (*)(std::uint64_t);
using NativeCall = std::uint64_t* (*)();
HMODULE g_sh = nullptr;

template <typename Function>
Function resolve(const char* name) {
    const FARPROC address = GetProcAddress(g_sh, name);
    Function function = nullptr;
    static_assert(sizeof(function) == sizeof(address));
    std::memcpy(&function, &address, sizeof(function));
    return function;
}

Unregister g_unregister = nullptr;
Wait g_wait = nullptr;
NativeInit g_native_init = nullptr;
NativePush g_native_push = nullptr;
NativeCall g_native_call = nullptr;
std::atomic<bool> g_pause_requested{false};
std::atomic<bool> g_registered{false};
std::atomic<bool> g_cancel{false};
std::filesystem::path g_handshake;
std::filesystem::path g_done;
HMODULE g_module = nullptr;
constexpr std::uint64_t kSetGamePaused = 0xFAEC088D28B1DE4AULL;
constexpr auto kFrameWaitTimeout = std::chrono::seconds(2);

void script_main() {
    int last = -1;
    while (true) {
        const int requested = g_pause_requested.load(std::memory_order_relaxed) ? 1 : 0;
        if (requested != last) {
            g_native_init(kSetGamePaused);
            g_native_push(static_cast<std::uint64_t>(requested));
            g_native_call();
            logger::info("Script Hook game pause set to {}", requested);
            last = requested;
        }
        g_wait(0);
    }
}

bool setup_script() {
    g_sh = LoadLibraryW(L"ScriptHookRDR2.dll");
    if (g_sh == nullptr) {
        logger::info("Script Hook unavailable; compare sequence will not pause the game");
        return false;
    }
    const auto reg = resolve<Register>("?scriptRegister@@YAXPEAUHINSTANCE__@@P6AXXZ@Z");
    g_unregister = resolve<Unregister>("?scriptUnregister@@YAXPEAUHINSTANCE__@@@Z");
    g_wait = resolve<Wait>("?scriptWait@@YAXK@Z");
    g_native_init = resolve<NativeInit>("?nativeInit@@YAX_K@Z");
    g_native_push = resolve<NativePush>("?nativePush64@@YAX_K@Z");
    g_native_call = resolve<NativeCall>("?nativeCall@@YAPEA_KXZ");
    if (reg == nullptr || g_unregister == nullptr || g_wait == nullptr || g_native_init == nullptr ||
        g_native_push == nullptr || g_native_call == nullptr) {
        logger::info("Script Hook exports incomplete; compare sequence will not pause the game");
        FreeLibrary(g_sh);
        g_sh = nullptr;
        return false;
    }
    g_registered.store(true, std::memory_order_relaxed);
    reg(g_module, &script_main);
    return true;
}

bool write_atomic(const std::filesystem::path& path, const std::string& value) {
    auto temp = path;
    temp += L".tmp";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!out || !(out << value)) {
            return false;
        }
    }
    return MoveFileExW(temp.c_str(), path.c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
}

bool acknowledged(unsigned sequence) {
    std::ifstream in(g_done);
    unsigned value = 0;
    return in >> value && value == sequence;
}

bool cancel_pressed(int key, bool& key_down) {
    const bool down = (GetAsyncKeyState(key) & 0x8000) != 0;
    const bool edge = down && !key_down;
    key_down = down;
    if (edge) {
        g_cancel.store(true, std::memory_order_relaxed);
    }
    return edge;
}

struct FrameWait {
    bool continue_sequence;
    bool timed_out;
    std::uint32_t drawn;
};

FrameWait wait_frames(std::uint32_t count, int key, bool& key_down) {
    const std::uint32_t start = bars::drawn_frames();
    const auto deadline = Clock::now() + kFrameWaitTimeout;
    while (bars::drawn_frames() - start < count && Clock::now() < deadline) {
        if (g_cancel.load(std::memory_order_relaxed)) {
            return {false, false, bars::drawn_frames() - start};
        }
        if (cancel_pressed(key, key_down)) {
            return {false, false, bars::drawn_frames() - start};
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    const std::uint32_t drawn = bars::drawn_frames() - start;
    return {true, drawn < count, drawn};
}

void log_frame_wait(const char* reason, const FrameWait& result) {
    if (result.timed_out) {
        logger::info("compare: {} wait timed out after {} s; frames stopped advancing ({} frame(s) drawn)",
                     reason, kFrameWaitTimeout.count(), result.drawn);
    } else {
        logger::info("compare: {} wait drew {} frame(s){}", reason, result.drawn,
                     result.continue_sequence ? "" : " before cancellation");
    }
}

}  // namespace

void initialize(HMODULE module, const std::filesystem::path& asi, bool freeze) {
    g_module = module;
    g_handshake = asi.parent_path() / L"CutsceneFit.compare";
    g_done = asi.parent_path() / L"CutsceneFit.compare.done";
    if (freeze) {
        setup_script();
    }
}

void run(int key, int hold_ms, bool freeze) {
    struct Entry {
        state::CompareMode mode;
        const char* name;
        bool hidden;
    };
    constexpr Entry entries[] = {
        {state::CompareMode::Game, "game", false},
        {state::CompareMode::BarsRemoved, "bars_removed", true},
        {state::CompareMode::Fit, "fit", false},
        {state::CompareMode::FitNoBars, "fit_no_bars", true},
    };
    const bool was_hidden = bars::hidden();
    bool key_down = true;
    unsigned sequence = 0;
    bool script_seen = false;
    const bool pause_enabled = freeze && g_registered.load(std::memory_order_relaxed);
    DeleteFileW(g_done.c_str());
    g_cancel.store(false, std::memory_order_relaxed);
    if (pause_enabled) {
        g_pause_requested.store(true, std::memory_order_relaxed);
        const FrameWait paused = wait_frames(5, key, key_down);
        log_frame_wait("initial pause", paused);
        if (!paused.continue_sequence) {
            g_cancel.store(true, std::memory_order_relaxed);
        }
    }
    for (const Entry& entry : entries) {
        if (g_cancel.load(std::memory_order_relaxed)) {
            break;
        }
        state::set_compare_mode(entry.mode);
        bars::set_hidden(entry.hidden);
        const std::uint32_t frame_start = bars::drawn_frames();
        const std::uint32_t required_frames = pause_enabled ? 3 : 5;
        if (pause_enabled) {
            g_pause_requested.store(false, std::memory_order_relaxed);
        }
        const auto unpaused_start = Clock::now();
        const FrameWait unpaused_wait = wait_frames(required_frames, key, key_down);
        if (pause_enabled) {
            g_pause_requested.store(true, std::memory_order_relaxed);
        }
        const auto unpaused_end = Clock::now();
        const std::uint32_t drawn = bars::drawn_frames() - frame_start;
        const auto unpaused_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(unpaused_end - unpaused_start).count();
        if (pause_enabled) {
            logger::info("compare: {} unpaused wait drew {} frame(s) in {} ms{}", entry.name, drawn,
                         unpaused_ms, unpaused_wait.timed_out ? "; frames stopped advancing" : "");
        } else {
            log_frame_wait(entry.name, unpaused_wait);
        }
        if (unpaused_wait.continue_sequence) {
            ++sequence;
        }
        bool answered = false;
        bool handshake_written = false;
        if (unpaused_wait.continue_sequence) {
            if (pause_enabled) {
                const FrameWait paused_wait = wait_frames(3, key, key_down);
                log_frame_wait("paused state", paused_wait);
                if (!paused_wait.continue_sequence) {
                    g_cancel.store(true, std::memory_order_relaxed);
                }
            }
        }
        if (unpaused_wait.continue_sequence && !g_cancel.load(std::memory_order_relaxed)) {
            handshake_written = write_atomic(g_handshake, std::to_string(sequence) + " " + entry.name + "\n");
            if (!handshake_written) {
                logger::info("compare: could not write handshake for {}", entry.name);
            }
        }
        const auto deadline = Clock::now() +
                              std::chrono::milliseconds(script_seen ? 10000 : hold_ms);
        while (handshake_written && Clock::now() < deadline &&
               !g_cancel.load(std::memory_order_relaxed)) {
            if (acknowledged(sequence)) {
                answered = true;
                script_seen = true;
                break;
            }
            if (cancel_pressed(key, key_down)) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        if (answered) {
            logger::info("compare: {} capture acknowledged", entry.name);
        } else if (handshake_written) {
            logger::info("compare: {} {}", entry.name,
                         g_cancel.load(std::memory_order_relaxed) ? "cancelled" : "hold timed out");
        }
        if (g_cancel.load(std::memory_order_relaxed)) {
            break;
        }
        if (!unpaused_wait.continue_sequence || !handshake_written) {
            break;
        }
    }
    g_pause_requested.store(false, std::memory_order_relaxed);
    state::set_compare_mode(state::CompareMode::Normal);
    bars::set_hidden(was_hidden);
    DeleteFileW(g_handshake.c_str());
    logger::info("compare sequence finished after {} state(s)", sequence);
    g_cancel.store(false);
}

void shutdown() {
    g_cancel.store(true, std::memory_order_relaxed);
    state::set_compare_mode(state::CompareMode::Normal);
    g_pause_requested.store(false, std::memory_order_relaxed);
    if (!g_handshake.empty()) {
        DeleteFileW(g_handshake.c_str());
    }
    if (g_registered.exchange(false, std::memory_order_relaxed) && g_unregister != nullptr) {
        g_unregister(g_module);
    }
}

}  // namespace compare

