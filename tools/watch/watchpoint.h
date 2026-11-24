// Copyright (c) 2026 Viktor Sharha
// Portions adapted from rdr2-ultrawide-proper-black-bar-fix, Copyright (c) 2026 rschi (MIT).

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace watchpoint {

// One debug register each; the x64 debug registers hold four addresses.
constexpr std::size_t kMaxTargets = 4;

struct Target {
    std::uintptr_t address;  // 4-byte aligned float
    int slot;                // reported with every hit
};

enum class StackMode {
    None,    // no call stack
    Scan,    // first values on the raw stack that point into executable sections
    Unwind,  // RtlVirtualUnwind walk; can fault on unusual unwind data
};

struct Range {
    std::uintptr_t begin;
    std::uintptr_t end;
};

struct Config {
    StackMode stack = StackMode::Scan;
    std::size_t scan_qwords = 256;
    std::span<const Range> executable;  // at most 16 ranges are used
    // false: a single step whose DR6 shows none of our bits, with the trap flag clear,
    // while armed, is still taken as ours (slot 255 when several slots are watched).
    bool trust_dr6 = false;
    bool main_thread_only = false;
    unsigned long main_thread = 0;
    std::size_t max_threads = 0;  // 0: no cap
    unsigned long arm_delay_ms = 0;  // pause between threads while arming
};

struct Applied {
    int updated = 0;
    int failed = 0;
};

// Must be called before arm().
void configure(const Config& config);

// Threads in the process other than the caller.
[[nodiscard]] std::size_t other_thread_count();

// Installs the exception handler and sets write watchpoints on every thread of
// the process except the caller. Clears the hit table. Extra targets beyond
// kMaxTargets are ignored. Returns zeros if the handler cannot be installed.
Applied arm(std::span<const Target> targets);

// Brings threads that appeared since the last call under watch. With `all`, every
// thread is rewritten instead, which also restores registers something else cleared.
Applied rearm(bool all);

// Clears the watchpoints on every thread and removes the exception handler.
Applied disarm();

[[nodiscard]] bool armed();

// Logs the aggregated hit table, most frequent first, at most `max_rows` rows.
void flush(std::uintptr_t module_base, std::size_t module_size, std::size_t max_rows);

// Hits recorded since arm(); lets the caller skip flushes when nothing changed.
[[nodiscard]] unsigned long long total_hits();

}  // namespace watchpoint
