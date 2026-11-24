// Copyright (c) 2026 Viktor Sharha

#include "ring.h"

#include <array>
#include <atomic>
#include <bit>

namespace zoom::ring {

namespace {

struct Entry {
    std::atomic<std::uint32_t> sequence{0};
    std::array<std::atomic<std::uint64_t>, 4> words{};
};

std::array<Entry, kCapacity> g_entries;
std::atomic<std::uint64_t> g_count{0};

std::uint64_t pack(std::uint32_t low, std::uint32_t high) {
    return static_cast<std::uint64_t>(low) | (static_cast<std::uint64_t>(high) << 32);
}

}  // namespace

void push(const Record& record) {
    const std::uint64_t frame = g_count.load(std::memory_order_relaxed);
    Entry& entry = g_entries[frame % kCapacity];

    const std::uint32_t sequence = entry.sequence.load(std::memory_order_relaxed);
    entry.sequence.store(sequence + 1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);

    const std::uint32_t tail = static_cast<std::uint32_t>(record.flags) |
                               (static_cast<std::uint32_t>(record.letterbox) << 16) |
                               (static_cast<std::uint32_t>(record.mode) << 24);
    entry.words[0].store(frame, std::memory_order_relaxed);
    entry.words[1].store(record.qpc, std::memory_order_relaxed);
    entry.words[2].store(pack(std::bit_cast<std::uint32_t>(record.input),
                              std::bit_cast<std::uint32_t>(record.corrected)),
                         std::memory_order_relaxed);
    entry.words[3].store(pack(std::bit_cast<std::uint32_t>(record.output), tail),
                         std::memory_order_relaxed);

    entry.sequence.store(sequence + 2, std::memory_order_release);
    g_count.store(frame + 1, std::memory_order_release);
}

std::uint64_t count() {
    return g_count.load(std::memory_order_acquire);
}

bool read(std::uint64_t frame, Record& out) {
    if (frame >= g_count.load(std::memory_order_acquire)) {
        return false;
    }
    const Entry& entry = g_entries[frame % kCapacity];

    const std::uint32_t before = entry.sequence.load(std::memory_order_acquire);
    if ((before & 1U) != 0) {
        return false;
    }
    std::array<std::uint64_t, 4> w{};
    for (std::size_t i = 0; i < w.size(); ++i) {
        w[i] = entry.words[i].load(std::memory_order_relaxed);
    }
    std::atomic_thread_fence(std::memory_order_acquire);
    if (entry.sequence.load(std::memory_order_relaxed) != before || w[0] != frame) {
        return false;
    }

    const auto tail = static_cast<std::uint32_t>(w[3] >> 32);
    out.frame = w[0];
    out.qpc = w[1];
    out.input = std::bit_cast<float>(static_cast<std::uint32_t>(w[2]));
    out.corrected = std::bit_cast<float>(static_cast<std::uint32_t>(w[2] >> 32));
    out.output = std::bit_cast<float>(static_cast<std::uint32_t>(w[3]));
    out.flags = static_cast<std::uint16_t>(tail & 0xFFFFU);
    out.letterbox = static_cast<std::uint8_t>((tail >> 16) & 0xFFU);
    out.mode = static_cast<std::uint8_t>(tail >> 24);
    return true;
}

}  // namespace zoom::ring
