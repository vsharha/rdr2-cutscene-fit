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
std::atomic<float> g_time_scale{1.0f};
std::atomic<bool> g_registered{false};
std::atomic<bool> g_cancel{false};
std::filesystem::path g_handshake;
std::filesystem::path g_done;
HMODULE g_module = nullptr;
constexpr std::uint64_t kSetTimeScale = 0x9682AF6050854856ULL;

void script_main() {
    float last = -1.0f;
    while (true) {
        const float requested = g_time_scale.load(std::memory_order_relaxed);
        if (requested != last) {
            std::uint32_t bits = 0;
            std::memcpy(&bits, &requested, sizeof(bits));
            g_native_init(kSetTimeScale);
            g_native_push(static_cast<std::uint64_t>(bits));
            g_native_call();
            logger::info("Script Hook time scale set to {:.3f}", requested);
            last = requested;
        }
        g_wait(0);
    }
}

bool setup_script() {
    g_sh = LoadLibraryW(L"ScriptHookRDR2.dll");
    if (g_sh == nullptr) {
        logger::info("Script Hook unavailable; compare sequence will not freeze the game");
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
        logger::info("Script Hook exports incomplete; compare sequence will not freeze the game");
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

bool wait_frames(std::uint32_t count, int key, bool& key_down) {
    const std::uint32_t start = bars::drawn_frames();
    while (bars::drawn_frames() - start < count) {
        if (g_cancel.load(std::memory_order_relaxed)) {
            return false;
        }
        if (cancel_pressed(key, key_down)) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return true;
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
    DeleteFileW(g_done.c_str());
    g_cancel.store(false, std::memory_order_relaxed);
    if (freeze && g_registered.load(std::memory_order_relaxed)) {
        g_time_scale.store(0.0f, std::memory_order_relaxed);
        if (!wait_frames(5, key, key_down)) {
            g_cancel.store(true, std::memory_order_relaxed);
        }
    }
    for (const Entry& entry : entries) {
        if (g_cancel.load(std::memory_order_relaxed)) {
            break;
        }
        state::set_compare_mode(entry.mode);
        bars::set_hidden(entry.hidden);
        if (!wait_frames(5, key, key_down)) {
            break;
        }
        ++sequence;
        if (!write_atomic(g_handshake, std::to_string(sequence) + " " + entry.name + "\n")) {
            logger::info("compare: could not write handshake for {}", entry.name);
            break;
        }
        const auto deadline = Clock::now() + std::chrono::milliseconds(script_seen ? 10000 : hold_ms);
        bool answered = false;
        while (Clock::now() < deadline && !g_cancel.load(std::memory_order_relaxed)) {
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
        logger::info("compare: {} {}", entry.name,
                     answered ? "capture acknowledged" : "hold timed out or cancelled");
        if (g_cancel.load(std::memory_order_relaxed)) {
            break;
        }
    }
    state::set_compare_mode(state::CompareMode::Normal);
    bars::set_hidden(was_hidden);
    DeleteFileW(g_handshake.c_str());
    g_time_scale.store(1.0f, std::memory_order_relaxed);
    logger::info("compare sequence finished after {} state(s)", sequence);
    g_cancel.store(false);
}

void shutdown() {
    g_cancel.store(true, std::memory_order_relaxed);
    state::set_compare_mode(state::CompareMode::Normal);
    g_time_scale.store(1.0f, std::memory_order_relaxed);
    if (!g_handshake.empty()) {
        DeleteFileW(g_handshake.c_str());
    }
    if (g_registered.exchange(false, std::memory_order_relaxed) && g_unregister != nullptr) {
        g_unregister(g_module);
    }
}

}  // namespace compare

