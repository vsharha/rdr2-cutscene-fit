// Copyright (c) 2026 Viktor Sharha

#pragma once

#ifdef CUTSCENEFIT_RELOAD
#include <atomic>
#include <chrono>
#include <thread>
#endif

// Counts the game threads inside a hook, so that a reload build can wait for them to
// leave before the DLL is unloaded. In other builds a Guard does nothing.
namespace inflight {

#ifdef CUTSCENEFIT_RELOAD

inline std::atomic<int> g_count{0};

class Guard {
public:
    Guard() { g_count.fetch_add(1, std::memory_order_acq_rel); }
    ~Guard() { g_count.fetch_sub(1, std::memory_order_acq_rel); }
    Guard(const Guard&) = delete;
    Guard& operator=(const Guard&) = delete;
};

// Waits until no game thread is inside a hook. Call it after the hooks are disabled.
// A thread can have taken the jump in a patched prologue without having reached the
// Guard yet, so a fixed wait follows the first time the count is zero.
inline void drain() {
    constexpr auto kPoll = std::chrono::milliseconds(1);
    constexpr auto kSettle = std::chrono::milliseconds(100);
    for (int pass = 0; pass < 2; ++pass) {
        while (g_count.load(std::memory_order_acquire) != 0) {
            std::this_thread::sleep_for(kPoll);
        }
        if (pass == 0) {
            std::this_thread::sleep_for(kSettle);
        }
    }
}

#else

class Guard {
public:
    Guard() {}
};

#endif

}  // namespace inflight
