// Copyright (c) 2026 Viktor Sharha

#include "hook.h"

#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <windows.h>

#include "common/log.h"
#include "ring.h"

namespace zoom {

const char* mode_name(Mode mode) {
    switch (mode) {
        case Mode::Off:
            return "off";
        case Mode::Always:
            return "always";
        case Mode::Letterbox:
            return "letterbox";
    }
    return "?";
}

namespace hook {

namespace {

constexpr std::uintptr_t kCallSiteRva = 0x3212DE;
constexpr std::uintptr_t kCalleeRva = 0x3C6168;
constexpr std::uintptr_t kFovOffset = 0x60;
constexpr std::uintptr_t kFlagsOffset = 0xCC;
constexpr std::uint8_t kCallOpcode = 0xE8;
constexpr std::uint8_t kJumpOpcode = 0xE9;
// The instruction `mov byte ptr [rip + disp32], 0FFh` that stores the letterbox alpha;
// the byte it stores to is the anchor of the letterbox state.
constexpr std::uintptr_t kAnchorStoreRva = 0x320545;
constexpr std::array<std::uint8_t, 12> kAnchorStore = {0xC6, 0x05, 0x00, 0x00, 0x00, 0x00,
                                                       0xFF, 0x0F, 0x28, 0x74, 0x24, 0x60};
constexpr std::uintptr_t kAnchorStoreLength = 7;
constexpr std::uintptr_t kWeightBelowAnchor = 8;
constexpr std::uintptr_t kEnableAboveAnchor = 0x21;
constexpr double kPi = 3.14159265358979323846;

constexpr std::array<std::uint8_t, 24> kCalleePrologue = {
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10, 0x56, 0x57,
    0x41, 0x56, 0x48, 0x83, 0xEC, 0x30, 0x0F, 0x29, 0x74, 0x24, 0x20, 0x4D};

using OriginalFn = char (*)(std::int64_t*, std::uint8_t*, std::int64_t*, std::uint8_t*);

OriginalFn g_original = nullptr;
std::uintptr_t g_call_site = 0;
std::uint8_t* g_stub = nullptr;
std::int32_t g_original_rel32 = 0;
std::int32_t g_stub_rel32 = 0;
const volatile std::uint8_t* g_letterbox = nullptr;
const volatile float* g_weight = nullptr;
std::atomic<bool> g_installed{false};
std::atomic<std::uint8_t> g_mode{static_cast<std::uint8_t>(Mode::Off)};
std::atomic<float> g_scale{1.0f};

float correct(float fov, float scale) {
    const double half = static_cast<double>(fov) * (kPi / 360.0);
    return static_cast<float>(2.0 * std::atan(static_cast<double>(scale) * std::tan(half)) *
                              (180.0 / kPi));
}

// Copies `size` bytes from a pointer the game passed us. ReadProcessMemory is one system
// call that fails instead of faulting when the memory is gone, and it neither allocates
// nor takes a lock in user mode. Returns whether all bytes were copied.
bool copy_bytes(const void* source, std::uint8_t* dest, std::size_t size) {
    if (source == nullptr) {
        return false;
    }
    SIZE_T read = 0;
    return ReadProcessMemory(GetCurrentProcess(), source, dest, size, &read) != FALSE && read == size;
}

// Runs on the game's update thread once per frame: no allocation, no logging, no locks.
char hooked(std::int64_t* p1, std::uint8_t* frame, std::int64_t* p3, std::uint8_t* owner) {
    if (frame == nullptr) {
        return g_original(p1, frame, p3, owner);
    }
    const auto mode = static_cast<Mode>(g_mode.load(std::memory_order_relaxed));
    const std::uint8_t letterbox = *g_letterbox;

    Record record;
    record.p1 = reinterpret_cast<std::uintptr_t>(p1);
    record.frame_pointer = reinterpret_cast<std::uintptr_t>(frame);
    record.p3 = reinterpret_cast<std::uintptr_t>(p3);
    record.owner = reinterpret_cast<std::uintptr_t>(owner);
    record.weight = *g_weight;
    if (copy_bytes(frame, record.frame_bytes.data(), record.frame_bytes.size())) {
        record.valid |= kValidFrame;
    }
    if (copy_bytes(p1, record.p1_bytes.data(), record.p1_bytes.size())) {
        record.valid |= kValidP1;
    }
    if (copy_bytes(p3, record.p3_bytes.data(), record.p3_bytes.size())) {
        record.valid |= kValidP3;
    }
    if (copy_bytes(owner, record.owner_bytes.data(), record.owner_bytes.size())) {
        record.valid |= kValidOwner;
    }

    float input = 0.0f;
    std::memcpy(&input, frame + kFovOffset, sizeof(input));
    float corrected = input;
    const bool active = mode == Mode::Always || (mode == Mode::Letterbox && letterbox != 0);
    if (active && input > 0.0f && input < 180.0f) {
        const float scale = g_scale.load(std::memory_order_relaxed);
        if (scale > 0.0f && std::isfinite(scale)) {
            corrected = correct(input, scale);
            std::memcpy(frame + kFovOffset, &corrected, sizeof(corrected));
        }
    }

    const char result = g_original(p1, frame, p3, owner);

    std::memcpy(&record.output, frame + kFovOffset, sizeof(record.output));
    std::memcpy(&record.flags, frame + kFlagsOffset, sizeof(record.flags));
    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);
    record.qpc = static_cast<std::uint64_t>(counter.QuadPart);
    record.input = input;
    record.corrected = corrected;
    record.letterbox = letterbox;
    record.mode = static_cast<std::uint8_t>(mode);
    ring::push(record);
    return result;
}

std::uint8_t* allocate_stub(std::uintptr_t anchor, const mem::Region& module) {
    constexpr std::uintptr_t kGranularity = 0x10000;
    constexpr std::uintptr_t kReach = 0x7FF00000;
    constexpr std::uintptr_t kLowest = 0x10000;
    const std::uintptr_t below = module.base & ~(kGranularity - 1);
    const std::uintptr_t above = (module.end() + kGranularity - 1) & ~(kGranularity - 1);

    const auto within = [anchor](std::uintptr_t address) {
        const std::uintptr_t distance = address > anchor ? address - anchor : anchor - address;
        return distance < kReach;
    };
    const auto attempt = [&](std::uintptr_t address) -> std::uint8_t* {
        if (!within(address)) {
            return nullptr;
        }
        return static_cast<std::uint8_t*>(VirtualAlloc(reinterpret_cast<void*>(address), kGranularity,
                                                       MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    };

    for (std::uintptr_t step = 0; step < kReach; step += kGranularity) {
        if (below > step + kLowest) {
            if (std::uint8_t* p = attempt(below - step - kGranularity)) {
                return p;
            }
        }
        if (std::uint8_t* p = attempt(above + step)) {
            return p;
        }
    }
    return nullptr;
}

bool store_rel32(std::int32_t value) {
    void* const target = reinterpret_cast<void*>(g_call_site + 1);
    DWORD previous = 0;
    if (!VirtualProtect(target, sizeof(value), PAGE_EXECUTE_READWRITE, &previous)) {
        logger::info("VirtualProtect on the call site failed, error {}", GetLastError());
        return false;
    }
    // The four bytes lie inside one cache line, so a locked store is seen whole by a
    // thread fetching the instruction.
    InterlockedExchange(static_cast<volatile LONG*>(target), static_cast<LONG>(value));
    DWORD ignored = 0;
    VirtualProtect(target, sizeof(value), previous, &ignored);
    FlushInstructionCache(GetCurrentProcess(), target, sizeof(value));
    return true;
}

std::int32_t current_rel32() {
    std::int32_t value = 0;
    std::memcpy(&value, reinterpret_cast<const void*>(g_call_site + 1), sizeof(value));
    return value;
}

// Finds the letterbox anchor from the alpha store at +0x320545. If the instruction is not
// there, falls back to the fixed enable address (anchor + 0x21 = kLetterboxEnableRva).
void resolve_letterbox(const mem::Region& module) {
    std::uintptr_t anchor = module.base + kLetterboxEnableRva - kEnableAboveAnchor;

    std::array<std::uint8_t, kAnchorStore.size()> store{};
    const std::uintptr_t address = module.base + kAnchorStoreRva;
    bool matches = mem::copy_readable(address, store.data(), store.size()) == 0;
    for (std::size_t i = 0; matches && i < store.size(); ++i) {
        const bool wildcard = i >= 2 && i < 6;
        matches = wildcard || store[i] == kAnchorStore[i];
    }
    if (matches) {
        std::int32_t displacement = 0;
        std::memcpy(&displacement, store.data() + 2, sizeof(displacement));
        const std::uintptr_t found =
            address + kAnchorStoreLength + static_cast<std::uintptr_t>(static_cast<std::int64_t>(displacement));
        logger::info("verify: letterbox anchor +0x{:X} from the alpha store at +0x{:X}{}",
                     found - module.base, kAnchorStoreRva,
                     found == anchor ? "" : " (differs from the fixed address; using it)");
        anchor = found;
    } else {
        logger::info("verify: the alpha store at +0x{:X} does not match; letterbox anchor +0x{:X} from the "
                     "fixed address", kAnchorStoreRva, anchor - module.base);
    }
    g_letterbox = reinterpret_cast<const volatile std::uint8_t*>(anchor + kEnableAboveAnchor);
    g_weight = reinterpret_cast<const volatile float*>(anchor - kWeightBelowAnchor);
}

}  // namespace

bool verify(const mem::Region& module) {
    g_call_site = module.base + kCallSiteRva;
    const std::uintptr_t callee = module.base + kCalleeRva;

    std::array<std::uint8_t, 5> site{};
    if (mem::copy_readable(g_call_site, site.data(), site.size()) != 0) {
        logger::info("verify: call site +0x{:X} is not readable", kCallSiteRva);
        return false;
    }
    std::int32_t rel = 0;
    std::memcpy(&rel, site.data() + 1, sizeof(rel));
    const std::uintptr_t target = g_call_site + 5 + static_cast<std::uintptr_t>(static_cast<std::int64_t>(rel));
    logger::info("verify: call site +0x{:X} bytes {:02X} {:02X} {:02X} {:02X} {:02X}, target +0x{:X}",
                 kCallSiteRva, site[0], site[1], site[2], site[3], site[4], target - module.base);
    if (site[0] != kCallOpcode || target != callee) {
        logger::info("verify: FAILED, expected E8 with target +0x{:X}", kCalleeRva);
        return false;
    }

    std::array<std::uint8_t, kCalleePrologue.size()> bytes{};
    if (mem::copy_readable(callee, bytes.data(), bytes.size()) != 0) {
        logger::info("verify: FAILED, +0x{:X} is not readable", kCalleeRva);
        return false;
    }
    if (bytes == kCalleePrologue) {
        logger::info("verify: ok, call site and callee prologue match");
    } else if (bytes[0] == kJumpOpcode) {
        // Another plugin (CutsceneFit, through MinHook) has replaced the start of the
        // callee with a jump. Calling the callee's address still runs the original through
        // that plugin's detour.
        std::int32_t jump = 0;
        std::memcpy(&jump, bytes.data() + 1, sizeof(jump));
        const std::uintptr_t jump_target =
            callee + 5 + static_cast<std::uintptr_t>(static_cast<std::int64_t>(jump));
        logger::info("verify: ok, the callee +0x{:X} starts with a jump (E9) to 0x{:X}; another plugin "
                     "has hooked it, the prologue was not compared", kCalleeRva, jump_target);
    } else {
        logger::info("verify: FAILED, the first {} bytes of +0x{:X} differ from the expected prologue",
                     bytes.size(), kCalleeRva);
        return false;
    }

    g_original = reinterpret_cast<OriginalFn>(callee);
    g_original_rel32 = rel;
    resolve_letterbox(module);
    if (g_stub == nullptr) {
        g_stub = allocate_stub(g_call_site + 5, module);
        if (g_stub == nullptr) {
            logger::info("verify: no executable page within 2 GB of the call site");
            g_original = nullptr;
            return false;
        }
        const std::int64_t offset = reinterpret_cast<std::intptr_t>(g_stub) -
                                    static_cast<std::int64_t>(g_call_site + 5);
        g_stub_rel32 = static_cast<std::int32_t>(offset);

        // jmp qword ptr [rip + 0] followed by the 8-byte destination.
        const std::array<std::uint8_t, 6> jump = {0xFF, 0x25, 0x00, 0x00, 0x00, 0x00};
        std::memcpy(g_stub, jump.data(), jump.size());
        const auto destination = reinterpret_cast<std::uintptr_t>(&hooked);
        std::memcpy(g_stub + jump.size(), &destination, sizeof(destination));
        DWORD previous = 0;
        VirtualProtect(g_stub, 0x1000, PAGE_EXECUTE_READ, &previous);
        FlushInstructionCache(GetCurrentProcess(), g_stub, jump.size() + sizeof(destination));
        logger::info("stub at 0x{:X} (rel32 0x{:08X}), forwards to 0x{:X}",
                     reinterpret_cast<std::uintptr_t>(g_stub), static_cast<std::uint32_t>(g_stub_rel32),
                     destination);
    }
    return true;
}

bool install() {
    if (g_stub == nullptr || g_original == nullptr) {
        logger::info("install: not verified");
        return false;
    }
    if (g_installed.load()) {
        logger::info("install: already installed");
        return false;
    }
    if (current_rel32() != g_original_rel32) {
        logger::info("install: the call site no longer holds the original rel32; not patching");
        return false;
    }
    if (!store_rel32(g_stub_rel32)) {
        return false;
    }
    g_installed.store(true);
    logger::info("hook installed: call rel32 0x{:08X} -> 0x{:08X} (stub 0x{:X})",
                 static_cast<std::uint32_t>(g_original_rel32), static_cast<std::uint32_t>(g_stub_rel32),
                 reinterpret_cast<std::uintptr_t>(g_stub));
    return true;
}

bool remove() {
    if (!g_installed.load()) {
        logger::info("remove: not installed");
        return false;
    }
    if (current_rel32() != g_stub_rel32) {
        logger::info("remove: the call site does not hold our rel32; not patching");
        return false;
    }
    if (!store_rel32(g_original_rel32)) {
        return false;
    }
    g_installed.store(false);
    logger::info("hook removed: call rel32 restored to 0x{:08X}", static_cast<std::uint32_t>(g_original_rel32));
    return true;
}

bool installed() {
    return g_installed.load();
}

void shutdown() {
    if (g_installed.load() && current_rel32() == g_stub_rel32 && store_rel32(g_original_rel32)) {
        g_installed.store(false);
    }
}

std::uintptr_t stub_address() {
    return reinterpret_cast<std::uintptr_t>(g_stub);
}

void set_mode(Mode mode) {
    g_mode.store(static_cast<std::uint8_t>(mode), std::memory_order_relaxed);
}

Mode mode() {
    return static_cast<Mode>(g_mode.load(std::memory_order_relaxed));
}

void set_scale(float scale) {
    g_scale.store(scale, std::memory_order_relaxed);
}

float scale() {
    return g_scale.load(std::memory_order_relaxed);
}

}  // namespace hook

}  // namespace zoom
