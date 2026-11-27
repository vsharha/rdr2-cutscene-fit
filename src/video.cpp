// Copyright (c) 2026 Viktor Sharha

#include "video.h"

#include <atomic>
#include <windows.h>

#include <MinHook.h>

#include "common/log.h"
#include "inflight.h"

namespace video {
namespace {

// Bink 2: BinkOpenWithOptions(char const* name, BINKOPENOPTIONS const* options,
// U32 flags) -> HBINK; BinkNextFrame(HBINK); BinkClose(HBINK).
using OpenFn = void* (*)(const char*, const void*, std::uint32_t);
using HandleFn = void (*)(void*);

OpenFn g_open_original = nullptr;
HandleFn g_next_original = nullptr;
HandleFn g_close_original = nullptr;

// GetTickCount64 at the latest BinkNextFrame; 0 before the first.
std::atomic<std::uint64_t> g_last_frame_ms{0};
// The latest video opened that has neither advanced a frame nor closed, and when it
// was opened.
std::atomic<void*> g_pending{nullptr};
std::atomic<std::uint64_t> g_pending_since_ms{0};

void clear_pending(void* handle) {
    void* expected = handle;
    g_pending.compare_exchange_strong(expected, nullptr, std::memory_order_relaxed);
}

void* open_detour(const char* name, const void* options, std::uint32_t flags) {
    [[maybe_unused]] const inflight::Guard guard;
    void* const handle = g_open_original(name, options, flags);
    if (handle != nullptr) {
        g_pending_since_ms.store(GetTickCount64(), std::memory_order_relaxed);
        g_pending.store(handle, std::memory_order_relaxed);
    }
    return handle;
}

void next_detour(void* handle) {
    [[maybe_unused]] const inflight::Guard guard;
    g_last_frame_ms.store(GetTickCount64(), std::memory_order_relaxed);
    clear_pending(handle);
    g_next_original(handle);
}

void close_detour(void* handle) {
    [[maybe_unused]] const inflight::Guard guard;
    clear_pending(handle);
    g_close_original(handle);
}

bool hook(HMODULE module, const char* name, void* detour, void** original) {
    const FARPROC target = GetProcAddress(module, name);
    if (target == nullptr) {
        logger::info("video hooks: bink2w64.dll has no export {}", name);
        return false;
    }
    const MH_STATUS status = MH_CreateHook(reinterpret_cast<LPVOID>(target), detour, original);
    if (status != MH_OK) {
        logger::info("video hooks: MH_CreateHook on {} failed: {}", name, MH_StatusToString(status));
        return false;
    }
    return true;
}

}  // namespace

bool create() {
    const HMODULE module = GetModuleHandleW(L"bink2w64.dll");
    if (module == nullptr) {
        logger::info("video hooks: bink2w64.dll is not loaded");
        return false;
    }
    // Without the frame and close hooks an opened video would count as playing for
    // kPendingLimitMs, so the hooks are installed all together or not at all.
    const bool created = hook(module, "BinkOpenWithOptions", reinterpret_cast<void*>(&open_detour),
                              reinterpret_cast<void**>(&g_open_original)) &&
                         hook(module, "BinkNextFrame", reinterpret_cast<void*>(&next_detour),
                              reinterpret_cast<void**>(&g_next_original)) &&
                         hook(module, "BinkClose", reinterpret_cast<void*>(&close_detour),
                              reinterpret_cast<void**>(&g_close_original));
    if (!created) {
        for (const char* name : {"BinkOpenWithOptions", "BinkNextFrame", "BinkClose"}) {
            MH_RemoveHook(reinterpret_cast<LPVOID>(GetProcAddress(module, name)));
        }
    }
    return created;
}

bool playing() {
    const std::uint64_t now = GetTickCount64();
    if (g_pending.load(std::memory_order_relaxed) != nullptr &&
        now - g_pending_since_ms.load(std::memory_order_relaxed) < kPendingLimitMs) {
        return true;
    }
    const std::uint64_t last = g_last_frame_ms.load(std::memory_order_relaxed);
    return last != 0 && now - last < kPlayingWindowMs;
}

}  // namespace video
