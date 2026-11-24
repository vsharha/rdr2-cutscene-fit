// Copyright (c) 2026 Viktor Sharha

#include "pattern.h"

#include <charconv>
#include <cstring>

namespace pattern {

std::optional<Pattern> parse(std::string_view text) {
    Pattern result;
    while (!text.empty()) {
        if (text.front() == ' ') {
            text.remove_prefix(1);
            continue;
        }
        const std::size_t end = text.find(' ');
        const std::string_view token = text.substr(0, end);
        text.remove_prefix(end == std::string_view::npos ? text.size() : end);

        if (token == "?") {
            result.bytes.push_back(0);
            result.fixed.push_back(false);
            continue;
        }
        unsigned value = 0;
        const auto [ptr, ec] = std::from_chars(token.data(), token.data() + token.size(), value, 16);
        if (token.size() != 2 || ec != std::errc{} || ptr != token.data() + token.size()) {
            return std::nullopt;
        }
        result.bytes.push_back(static_cast<std::uint8_t>(value));
        result.fixed.push_back(true);
    }
    if (result.bytes.empty() || !result.fixed.front()) {
        return std::nullopt;
    }
    return result;
}

std::vector<std::uintptr_t> find_all(std::span<const mem::Region> regions, const Pattern& pattern) {
    std::vector<std::uintptr_t> hits;
    const std::size_t length = pattern.bytes.size();

    for (const mem::Region& region : regions) {
        for (const mem::Region& part : mem::readable_subranges(region)) {
            if (part.size < length) {
                continue;
            }
            const auto* data = reinterpret_cast<const std::uint8_t*>(part.base);
            const std::size_t last = part.size - length;
            std::size_t i = 0;
            while (i <= last) {
                const void* found = std::memchr(data + i, pattern.bytes.front(), last - i + 1);
                if (found == nullptr) {
                    break;
                }
                i = static_cast<std::size_t>(static_cast<const std::uint8_t*>(found) - data);

                bool match = true;
                for (std::size_t j = 1; j < length; ++j) {
                    if (pattern.fixed[j] && data[i + j] != pattern.bytes[j]) {
                        match = false;
                        break;
                    }
                }
                if (match) {
                    hits.push_back(part.base + i);
                }
                ++i;
            }
        }
    }
    return hits;
}

}  // namespace pattern
