// Copyright (c) 2026 Viktor Sharha

#include "common/paths.h"

#include <system_error>
#include <string>

namespace paths {

std::filesystem::path module_path(HMODULE module) {
    std::wstring buffer(MAX_PATH, L'\0');
    for (;;) {
        const DWORD length = GetModuleFileNameW(module, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0) {
            return {};
        }
        if (length < buffer.size()) {
            buffer.resize(length);
            return buffer;
        }
        buffer.resize(buffer.size() * 2);
    }
}

std::filesystem::path fallback_directory() {
    const DWORD needed = GetEnvironmentVariableW(L"LOCALAPPDATA", nullptr, 0);
    if (needed == 0) {
        return {};
    }
    std::wstring value(needed, L'\0');
    const DWORD written = GetEnvironmentVariableW(L"LOCALAPPDATA", value.data(), needed);
    if (written == 0 || written >= needed) {
        return {};
    }
    value.resize(written);

    std::filesystem::path directory = std::filesystem::path(value) / L"CutsceneFit";
    std::error_code ec;
    std::filesystem::create_directories(directory, ec);
    return ec ? std::filesystem::path{} : directory;
}

}  // namespace paths
