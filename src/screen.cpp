// Copyright (c) 2026 Viktor Sharha

#include "screen.h"

#include <windows.h>

namespace screen {
namespace {

struct Search {
    DWORD pid = 0;
    long best_area = 0;
    int width = 0;
    int height = 0;
};

BOOL CALLBACK consider(HWND window, LPARAM parameter) {
    auto& search = *reinterpret_cast<Search*>(parameter);
    DWORD pid = 0;
    GetWindowThreadProcessId(window, &pid);
    RECT client{};
    if (pid != search.pid || !IsWindowVisible(window) || !GetClientRect(window, &client)) {
        return TRUE;
    }
    const long width = client.right - client.left;
    const long height = client.bottom - client.top;
    if (width > 0 && height > 0 && width * height > search.best_area) {
        search.best_area = width * height;
        search.width = static_cast<int>(width);
        search.height = static_cast<int>(height);
    }
    return TRUE;
}

Measurement make(int width, int height, const char* source) {
    return {static_cast<double>(width) / static_cast<double>(height), width, height, source};
}

}  // namespace

std::optional<Measurement> game_window() {
    Search search;
    search.pid = GetCurrentProcessId();
    EnumWindows(consider, reinterpret_cast<LPARAM>(&search));
    if (search.best_area == 0) {
        return std::nullopt;
    }
    return make(search.width, search.height, "game window client area");
}

std::optional<Measurement> primary_monitor() {
    const int width = GetSystemMetrics(SM_CXSCREEN);
    const int height = GetSystemMetrics(SM_CYSCREEN);
    if (width <= 0 || height <= 0) {
        return std::nullopt;
    }
    return make(width, height, "primary monitor");
}

}  // namespace screen
