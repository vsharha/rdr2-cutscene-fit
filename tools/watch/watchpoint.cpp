// Copyright (c) 2026 Viktor Sharha
// Portions adapted from rdr2-ultrawide-proper-black-bar-fix, Copyright (c) 2026 rschi (MIT).

#include "watchpoint.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>
#include <windows.h>

#include <tlhelp32.h>

#include "common/log.h"
#include "trace.h"

namespace watchpoint {
namespace {

constexpr std::size_t kStackDepth = 8;
constexpr std::size_t kTableSize = 512;  // power of two

// DR7 bits for register i: local enable at 2i, access type at 16 + 4i (01 = write),
// length at 18 + 4i (11 = 4 bytes).
constexpr DWORD64 dr7_enable(std::size_t i) { return 1ull << (2 * i); }
constexpr DWORD64 dr7_control(std::size_t i) {
    return (0x1ull << (16 + 4 * i)) | (0x3ull << (18 + 4 * i));
}
constexpr DWORD64 dr7_mask(std::size_t i) { return dr7_enable(i) | (0xFull << (16 + 4 * i)); }

constexpr DWORD64 kDr6Hits = 0xF;
constexpr DWORD64 kDr6SingleStep = 1ull << 14;

struct Entry {
    std::atomic<std::uint32_t> state{0};  // 0 empty, 1 being filled, 2 ready
    std::uint32_t slot = 0;
    std::uint64_t rip = 0;
    std::uint64_t stack[kStackDepth]{};
    std::atomic<unsigned long long> count{0};
    std::atomic<std::uint32_t> last_value{0};  // float bits
    std::atomic<std::uint32_t> last_thread{0};
};

// Written by arm() before any thread receives a debug register, so the handler
// reads them without synchronisation.
std::array<std::uintptr_t, kMaxTargets> g_address{};
std::array<int, kMaxTargets> g_slot{};
std::atomic<std::size_t> g_target_count{0};

constexpr std::size_t kMaxRanges = 16;
constexpr std::size_t kDiagnosticEvents = 8;

// Raw registers of the first single-step exceptions, to show what the platform
// reports. Filled by the handler, logged by flush().
struct Diagnostic {
    std::atomic<std::uint64_t> dr6{0};
    std::atomic<std::uint64_t> dr7{0};
    std::atomic<std::uint64_t> dr0{0};
    std::atomic<std::uint64_t> rip{0};
    std::atomic<std::uint32_t> ours{0};  // 0 foreign, 1 by DR6, 2 by fallback
    std::atomic<std::uint32_t> ready{0};
};

StackMode g_stack_mode = StackMode::Scan;
std::size_t g_scan_qwords = 256;
std::array<Range, kMaxRanges> g_executable{};
std::size_t g_executable_count = 0;

bool g_trust_dr6 = false;
bool g_main_only = false;
DWORD g_main_thread = 0;
std::size_t g_max_threads = 0;
DWORD g_arm_delay_ms = 0;
std::atomic<bool> g_active{false};
std::atomic<unsigned> g_trace_events{0};
constexpr unsigned kTraceEvents = 32;
constexpr std::uint32_t kUnknownSlot = 255;

Diagnostic g_diagnostics[kDiagnosticEvents];
std::atomic<std::size_t> g_events{0};
std::atomic<unsigned long long> g_foreign{0};

Entry g_table[kTableSize];
std::atomic<unsigned long long> g_total{0};
std::atomic<unsigned long long> g_dropped{0};

PVOID g_handler = nullptr;
std::vector<DWORD> g_armed_threads;  // worker thread only

DWORD64& debug_register(CONTEXT& context, std::size_t i) {
    switch (i) {
        case 0: return context.Dr0;
        case 1: return context.Dr1;
        case 2: return context.Dr2;
        default: return context.Dr3;
    }
}

std::uint64_t hash_key(std::uint32_t slot, std::uint64_t rip, const std::uint64_t* stack) {
    std::uint64_t h = 1469598103934665603ull;
    const auto mix = [&h](std::uint64_t v) {
        h = (h ^ v) * 1099511628211ull;
        h ^= h >> 29;
    };
    mix(slot);
    mix(rip);
    for (std::size_t i = 0; i < kStackDepth; ++i) {
        mix(stack[i]);
    }
    return h;
}

// Entries are never removed while armed, so a probe that reaches an empty entry
// proves the key is absent. An entry another thread is still filling is skipped,
// which can duplicate a key in the rare case of two threads recording it first at
// the same instant.
void record(std::uint32_t slot, std::uint64_t rip, const std::uint64_t* stack, float value,
            DWORD thread) {
    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));

    const std::size_t start = static_cast<std::size_t>(hash_key(slot, rip, stack)) & (kTableSize - 1);
    for (std::size_t n = 0; n < kTableSize; ++n) {
        Entry& e = g_table[(start + n) & (kTableSize - 1)];
        std::uint32_t state = e.state.load(std::memory_order_acquire);
        if (state == 0 && e.state.compare_exchange_strong(state, 1, std::memory_order_acquire)) {
            e.slot = slot;
            e.rip = rip;
            std::memcpy(e.stack, stack, sizeof(e.stack));
            e.count.store(1, std::memory_order_relaxed);
            e.last_value.store(bits, std::memory_order_relaxed);
            e.last_thread.store(thread, std::memory_order_relaxed);
            e.state.store(2, std::memory_order_release);
            return;
        }
        if (state == 2 && e.slot == slot && e.rip == rip &&
            std::memcmp(e.stack, stack, sizeof(e.stack)) == 0) {
            e.count.fetch_add(1, std::memory_order_relaxed);
            e.last_value.store(bits, std::memory_order_relaxed);
            e.last_thread.store(thread, std::memory_order_relaxed);
            return;
        }
    }
    g_dropped.fetch_add(1, std::memory_order_relaxed);
}

bool in_executable(std::uint64_t address) {
    for (std::size_t i = 0; i < g_executable_count; ++i) {
        if (address >= g_executable[i].begin && address < g_executable[i].end) {
            return true;
        }
    }
    return false;
}

struct StackBounds {
    DWORD64 low;
    DWORD64 high;
};

StackBounds stack_bounds() {
    const auto* tib = reinterpret_cast<const NT_TIB*>(NtCurrentTeb());
    return {reinterpret_cast<DWORD64>(tib->StackLimit), reinterpret_cast<DWORD64>(tib->StackBase)};
}

// Candidate return addresses: the first values found on the stack, from Rsp upwards,
// that point into executable sections of the game. Only reads inside the thread's
// stack bounds, so it cannot fault. Includes stale values; innermost first.
void scan_stack(const CONTEXT& exception, std::uint64_t* out) {
    const StackBounds bounds = stack_bounds();
    DWORD64 sp = exception.Rsp & ~static_cast<DWORD64>(7);
    if (sp < bounds.low || sp >= bounds.high) {
        return;
    }
    std::size_t found = 0;
    for (std::size_t n = 0; n < g_scan_qwords && found < kStackDepth &&
                            sp + sizeof(DWORD64) <= bounds.high;
         ++n, sp += sizeof(DWORD64)) {
        const std::uint64_t value = *reinterpret_cast<const std::uint64_t*>(sp);
        if (in_executable(value)) {
            out[found++] = value;
        }
    }
}

// Return addresses of the frames above the exception, innermost first; unused
// entries stay zero. Functions without unwind data are treated as leaf functions.
void unwind_stack(const CONTEXT& exception, std::uint64_t* out) {
    const StackBounds bounds = stack_bounds();
    const auto on_stack = [&](DWORD64 sp) {
        return sp >= bounds.low && sp + sizeof(DWORD64) <= bounds.high && (sp & 7) == 0;
    };

    CONTEXT context = exception;
    for (std::size_t i = 0; i < kStackDepth; ++i) {
        if (!on_stack(context.Rsp)) {
            return;
        }
        DWORD64 image_base = 0;
        const RUNTIME_FUNCTION* function = RtlLookupFunctionEntry(context.Rip, &image_base, nullptr);
        if (function != nullptr) {
            PVOID handler_data = nullptr;
            DWORD64 establisher = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, context.Rip,
                             const_cast<RUNTIME_FUNCTION*>(function), &context, &handler_data,
                             &establisher, nullptr);
        } else {
            context.Rip = *reinterpret_cast<const DWORD64*>(context.Rsp);
            context.Rsp += sizeof(DWORD64);
        }
        if (context.Rip == 0) {
            return;
        }
        out[i] = context.Rip;
    }
}

void note_event(const CONTEXT& context, std::uint32_t ours) {
    const std::size_t index = g_events.fetch_add(1, std::memory_order_relaxed);
    if (index >= kDiagnosticEvents) {
        return;
    }
    Diagnostic& d = g_diagnostics[index];
    d.dr6.store(context.Dr6, std::memory_order_relaxed);
    d.dr7.store(context.Dr7, std::memory_order_relaxed);
    d.dr0.store(context.Dr0, std::memory_order_relaxed);
    d.rip.store(context.Rip, std::memory_order_relaxed);
    d.ours.store(ours, std::memory_order_relaxed);
    d.ready.store(1, std::memory_order_release);
}

LONG handle_exception(EXCEPTION_POINTERS* info, const char*& decision) {
    if (info->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    CONTEXT& context = *info->ContextRecord;

    const std::size_t count = g_target_count.load(std::memory_order_acquire);
    DWORD64 ours = 0;
    for (std::size_t i = 0; i < count; ++i) {
        if ((context.Dr6 & (1ull << i)) != 0) {
            ours |= 1ull << i;
        }
    }
    std::uint32_t kind = ours != 0 ? 1 : 0;

    // Wine may deliver a context whose DR6 lacks the hit bits. A single step that no
    // trap flag explains, while armed, is then taken to be one of our watchpoints.
    bool unknown_slot = false;
    if (ours == 0 && !g_trust_dr6 && count != 0 && g_active.load(std::memory_order_acquire) &&
        (context.EFlags & 0x100) == 0) {
        kind = 2;
        if (count == 1) {
            ours = 1;
        } else {
            unknown_slot = true;
        }
    }
    note_event(context, kind);
    if (kind == 0) {
        g_foreign.fetch_add(1, std::memory_order_relaxed);
        return EXCEPTION_CONTINUE_SEARCH;
    }
    decision = kind == 1 ? "ours (dr6)" : "ours (fallback)";

    std::uint64_t stack[kStackDepth]{};
    if (g_stack_mode == StackMode::Scan) {
        scan_stack(context, stack);
    } else if (g_stack_mode == StackMode::Unwind) {
        unwind_stack(context, stack);
    }
    const DWORD thread = GetCurrentThreadId();
    if (unknown_slot) {
        record(kUnknownSlot, context.Rip, stack, 0.0f, thread);
        g_total.fetch_add(1, std::memory_order_relaxed);
    }
    for (std::size_t i = 0; i < count && !unknown_slot; ++i) {
        if ((ours & (1ull << i)) != 0) {
            float value = 0.0f;
            std::memcpy(&value, reinterpret_cast<const void*>(g_address[i]), sizeof(value));
            record(static_cast<std::uint32_t>(g_slot[i]), context.Rip, stack, value, thread);
            g_total.fetch_add(1, std::memory_order_relaxed);
        }
    }

    context.Dr6 = 0;
    context.EFlags |= 0x10000;  // resume flag
    return EXCEPTION_CONTINUE_EXECUTION;
}

LONG CALLBACK on_exception(EXCEPTION_POINTERS* info) {
    // Written before anything else so a fault inside the decision still leaves a line.
    const bool traced = g_active.load(std::memory_order_acquire) &&
                        g_trace_events.fetch_add(1, std::memory_order_relaxed) < kTraceEvents;
    if (traced) {
        const EXCEPTION_RECORD& record = *info->ExceptionRecord;
        const CONTEXT& context = *info->ContextRecord;
        trace::Line()
            .text("E exc=").hex(record.ExceptionCode)
            .text(" rip=").address(context.Rip)
            .text(" dr0=").hex(context.Dr0)
            .text(" dr6=").hex(context.Dr6)
            .text(" dr7=").hex(context.Dr7)
            .text(" eflags=").hex(context.EFlags)
            .text(" ctxflags=").hex(context.ContextFlags)
            .text(" tid=").dec(GetCurrentThreadId())
            .end();
    }
    const char* decision = "pass";
    const LONG result = handle_exception(info, decision);
    if (traced) {
        trace::Line().text("  -> ").text(decision).end();
    }
    return result;
}

std::vector<DWORD> other_thread_ids() {
    std::vector<DWORD> ids;
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return ids;
    }
    THREADENTRY32 entry{};
    entry.dwSize = sizeof(entry);
    const DWORD process = GetCurrentProcessId();
    const DWORD self = GetCurrentThreadId();
    if (Thread32First(snapshot, &entry)) {
        do {
            if (entry.th32OwnerProcessID == process && entry.th32ThreadID != self) {
                ids.push_back(entry.th32ThreadID);
            }
        } while (Thread32Next(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return ids;
}

bool set_thread(DWORD id, bool enable, std::size_t index) {
    trace::Line().text(enable ? "A> idx=" : "D> idx=").dec(index).text(" tid=").dec(id).end();

    bool got = false;
    bool set = false;
    DWORD suspended = static_cast<DWORD>(-1);
    DWORD resumed = static_cast<DWORD>(-1);
    DWORD error = 0;
    const HANDLE thread =
        OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE, id);
    if (thread == nullptr) {
        error = GetLastError();
    } else {
        // Nothing between suspend and resume may allocate, log or trace: the
        // suspended thread could hold the heap or logger lock.
        suspended = SuspendThread(thread);
        if (suspended != static_cast<DWORD>(-1)) {
            CONTEXT context{};
            context.ContextFlags = CONTEXT_DEBUG_REGISTERS;
            got = GetThreadContext(thread, &context) != FALSE;
            if (got) {
                const std::size_t count = g_target_count.load(std::memory_order_relaxed);
                for (std::size_t i = 0; i < count; ++i) {
                    DWORD64& reg = debug_register(context, i);
                    if (enable) {
                        reg = g_address[i];
                        context.Dr7 = (context.Dr7 & ~dr7_mask(i)) | dr7_enable(i) | dr7_control(i);
                    } else if (reg == g_address[i]) {
                        reg = 0;
                        context.Dr7 &= ~dr7_mask(i);
                    }
                }
                context.Dr6 = 0;
                set = SetThreadContext(thread, &context) != FALSE;
            }
            if (!got || !set) {
                error = GetLastError();
            }
            resumed = ResumeThread(thread);
        } else {
            error = GetLastError();
        }
        CloseHandle(thread);
    }

    trace::Line()
        .text(enable ? "A< idx=" : "D< idx=").dec(index)
        .text(" tid=").dec(id)
        .text(" open=").dec(thread != nullptr ? 1 : 0)
        .text(" suspend=").hex(suspended)
        .text(" get=").dec(got ? 1 : 0)
        .text(" set=").dec(set ? 1 : 0)
        .text(" resume=").hex(resumed)
        .text(" err=").dec(error)
        .end();
    return set;
}

// Applies the thread selection settings to a list of candidates; `already` threads
// are armed and count against the cap.
void select_threads(std::vector<DWORD>& ids, std::size_t already) {
    if (g_main_only) {
        std::erase_if(ids, [](DWORD id) { return id != g_main_thread; });
    }
    if (g_max_threads != 0) {
        ids.resize(std::min(ids.size(), g_max_threads > already ? g_max_threads - already : 0));
    }
}

Applied apply_to_threads(const std::vector<DWORD>& ids, bool enable) {
    Applied result;
    std::size_t index = 0;
    for (const DWORD id : ids) {
        if (enable && g_arm_delay_ms != 0 && index != 0) {
            Sleep(g_arm_delay_ms);
        }
        if (set_thread(id, enable, index++)) {
            ++result.updated;
            if (enable) {
                g_armed_threads.push_back(id);
            }
        } else {
            ++result.failed;
        }
    }
    return result;
}

std::string describe(std::uint64_t address, std::uintptr_t module_base, std::size_t module_size) {
    if (address == 0) {
        return "-";
    }
    if (address >= module_base && address < module_base + module_size) {
        return std::format("+0x{:X}", address - module_base);
    }
    HMODULE owner = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(address), &owner)) {
        std::wstring path(MAX_PATH, L'\0');
        const DWORD length = GetModuleFileNameW(owner, path.data(), static_cast<DWORD>(path.size()));
        if (length != 0 && length < path.size()) {
            path.resize(length);
            return std::format("{}+0x{:X}", logger::utf8(std::filesystem::path(path).filename()),
                               address - reinterpret_cast<std::uintptr_t>(owner));
        }
    }
    return std::format("0x{:X}", address);
}

}  // namespace

void configure(const Config& config) {
    g_stack_mode = config.stack;
    g_scan_qwords = config.scan_qwords;
    g_trust_dr6 = config.trust_dr6;
    g_main_only = config.main_thread_only;
    g_main_thread = config.main_thread;
    g_max_threads = config.max_threads;
    g_arm_delay_ms = config.arm_delay_ms;
    g_executable_count = std::min(config.executable.size(), kMaxRanges);
    std::copy_n(config.executable.begin(), g_executable_count, g_executable.begin());
}

std::size_t other_thread_count() { return other_thread_ids().size(); }

Applied arm(std::span<const Target> targets) {
    if (g_handler != nullptr) {
        disarm();
    }
    const std::size_t count = std::min(targets.size(), kMaxTargets);
    if (count == 0) {
        return {};
    }

    for (Entry& e : g_table) {
        e.state.store(0, std::memory_order_relaxed);
    }
    g_total.store(0);
    g_dropped.store(0);
    g_foreign.store(0);
    g_events.store(0);
    g_trace_events.store(0);
    for (Diagnostic& d : g_diagnostics) {
        d.ready.store(0);
    }
    for (std::size_t i = 0; i < count; ++i) {
        g_address[i] = targets[i].address;
        g_slot[i] = targets[i].slot;
    }
    g_target_count.store(count, std::memory_order_release);
    g_armed_threads.clear();

    g_active.store(true, std::memory_order_release);
    g_handler = AddVectoredExceptionHandler(1, on_exception);
    if (g_handler == nullptr) {
        g_target_count.store(0);
        g_active.store(false);
        return {};
    }
    std::vector<DWORD> ids = other_thread_ids();
    select_threads(ids, 0);
    return apply_to_threads(ids, true);
}

Applied rearm(bool all) {
    if (g_handler == nullptr) {
        return {};
    }
    std::vector<DWORD> ids = other_thread_ids();
    std::vector<DWORD> alive;
    alive.reserve(g_armed_threads.size());
    for (const DWORD id : g_armed_threads) {
        if (std::find(ids.begin(), ids.end(), id) != ids.end()) {
            alive.push_back(id);
        }
    }
    if (all) {
        g_armed_threads.clear();
        select_threads(ids, 0);
        return apply_to_threads(ids, true);
    }
    g_armed_threads = std::move(alive);
    std::erase_if(ids, [](DWORD id) {
        return std::find(g_armed_threads.begin(), g_armed_threads.end(), id) !=
               g_armed_threads.end();
    });
    select_threads(ids, g_armed_threads.size());
    return apply_to_threads(ids, true);
}

Applied disarm() {
    if (g_handler == nullptr) {
        return {};
    }
    const Applied result = apply_to_threads(other_thread_ids(), false);
    RemoveVectoredExceptionHandler(g_handler);
    g_handler = nullptr;
    g_active.store(false, std::memory_order_release);
    g_armed_threads.clear();
    return result;
}

bool armed() { return g_handler != nullptr; }

unsigned long long total_hits() { return g_total.load(std::memory_order_relaxed); }

void flush(std::uintptr_t module_base, std::size_t module_size, std::size_t max_rows) {
    struct Row {
        const Entry* entry;
        unsigned long long count;
    };
    std::vector<Row> rows;
    for (const Entry& e : g_table) {
        if (e.state.load(std::memory_order_acquire) == 2) {
            rows.push_back({&e, e.count.load(std::memory_order_relaxed)});
        }
    }
    std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.count > b.count; });

    logger::info("hits: {} total, {} distinct (slot, stack) records, {} dropped (table full)",
                 g_total.load(), rows.size(), g_dropped.load());
    logger::info("single-step exceptions seen: {} ({} not ours)", g_events.load(), g_foreign.load());
    for (std::size_t i = 0; i < kDiagnosticEvents; ++i) {
        const Diagnostic& d = g_diagnostics[i];
        if (d.ready.load(std::memory_order_acquire) != 0) {
            logger::info("  event {}: dr6 0x{:X} dr7 0x{:X} dr0 0x{:X} rip {} {}", i,
                         d.dr6.load(), d.dr7.load(), d.dr0.load(),
                         describe(d.rip.load(), module_base, module_size),
                         d.ours.load() == 1 ? "ours (dr6)" : d.ours.load() == 2 ? "ours (fallback)" : "not ours");
        }
    }
    const std::size_t shown = std::min(rows.size(), max_rows);
    for (std::size_t r = 0; r < shown; ++r) {
        const Entry& e = *rows[r].entry;
        float value = 0.0f;
        const std::uint32_t bits = e.last_value.load(std::memory_order_relaxed);
        std::memcpy(&value, &bits, sizeof(value));

        std::string stack;
        for (const std::uint64_t frame : e.stack) {
            if (frame == 0) {
                break;
            }
            if (!stack.empty()) {
                stack += ' ';
            }
            stack += describe(frame, module_base, module_size);
        }
        logger::info("  slot {:<3} count {:<8} last {:<10.4f} tid {:<6} rip {}  stack {}", e.slot,
                     rows[r].count, value, e.last_thread.load(std::memory_order_relaxed),
                     describe(e.rip, module_base, module_size), stack);
    }
    if (shown < rows.size()) {
        logger::info("  ... {} more record(s) not shown", rows.size() - shown);
    }
}

}  // namespace watchpoint
