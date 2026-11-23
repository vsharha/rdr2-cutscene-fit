// Copyright (c) 2026 Viktor Sharha

#pragma once

#include <filesystem>
#include <windows.h>

namespace paths {

// Full path of the DLL (the .asi) identified by `module`.
std::filesystem::path module_path(HMODULE module);

// %LOCALAPPDATA%\CutsceneFit, created if missing. Empty if it cannot be created.
std::filesystem::path fallback_directory();

}  // namespace paths
