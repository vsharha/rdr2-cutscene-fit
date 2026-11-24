// Copyright (c) 2026 Viktor Sharha

#pragma once

#include <filesystem>
#include <windows.h>

namespace compare {

void initialize(HMODULE module, const std::filesystem::path& asi, bool freeze);
void run(int key, int hold_ms, bool freeze);
void shutdown();

}  // namespace compare

