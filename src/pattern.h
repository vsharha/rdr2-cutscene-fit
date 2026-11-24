// Copyright (c) 2026 Viktor Sharha

#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "common/mem.h"

namespace pattern {

// Hex bytes separated by spaces, "?" for any byte: "48 8B ? ? C3". The first byte
// must not be a wildcard. Returns nullopt for anything else.
struct Pattern {
    std::vector<std::uint8_t> bytes;
    std::vector<bool> fixed;
};
std::optional<Pattern> parse(std::string_view text);

// The address of every match inside the readable parts of `regions`, in order.
std::vector<std::uintptr_t> find_all(std::span<const mem::Region> regions, const Pattern& pattern);

}  // namespace pattern
