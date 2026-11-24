// Copyright (c) 2026 Viktor Sharha

#pragma once

#include <cstdint>

#include "common/mem.h"

namespace zoom {

enum class Mode : std::uint8_t { Off = 0, Always = 1, Letterbox = 2 };

const char* mode_name(Mode mode);

namespace hook {

constexpr std::uintptr_t kLetterboxEnableRva = 0x39751D5;

// Checks the call site and the callee against the expected bytes and logs the result.
// Returns false if either differs; nothing else in this namespace works until it
// returns true.
bool verify(const mem::Region& module);

// Redirects the call to our function through a stub near the call site, allocating the
// stub on first use. Both return false if the state already matches or on failure,
// and log what happened.
bool install();
bool remove();
bool installed();

// Restores the original call if installed. Safe to call from DLL_PROCESS_DETACH.
void shutdown();

std::uintptr_t stub_address();

void set_mode(Mode mode);
Mode mode();

// Factor applied to tan(fov / 2).
void set_scale(float scale);
float scale();

}  // namespace hook

}  // namespace zoom
