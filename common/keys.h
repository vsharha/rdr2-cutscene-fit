// Copyright (c) 2026 Viktor Sharha

#pragma once

#include <string>

namespace keys {

// Parses a key name (F1..F24, A..Z, 0..9, End, Home, Insert, Delete, PageUp, PageDown,
// Up, Down, Left, Right, Pause, ScrollLock) or a virtual-key code such as 0x78 into a
// virtual-key code. Whitespace and case are ignored. Returns `fallback` for anything else.
int parse(const std::wstring& text, int fallback);

}  // namespace keys
