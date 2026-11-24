// Copyright (c) 2026 Viktor Sharha

#include "ring.h"

#include <array>
#include <atomic>
#include <cstring>

namespace zoom::ring {

namespace {

constexpr std::size_t kWords = sizeof(Record) / sizeof(std::uint64_t);

struct Entry {
    std::atomic<std::uint32_t> sequence{0};
    std::array<std::atomic<std::uint64_t>, kWords> words{};
};

std::array<Entry, kCapacity> g_entries;
std::atomic<std::uint64_t> g_count{0};

}  // namespace

void push(const Record& record) {
    const std::uint64_t frame = g_count.load(std::memory_order_relaxed);
    Entry& entry = g_entries[frame % kCapacity];

    Record numbered = record;
    numbered.frame = frame;
    std::array<std::uint64_t, kWords> source{};
    std::memcpy(source.data(), &numbered, sizeof(numbered));

    const std::uint32_t sequence = entry.sequence.load(std::memory_order_relaxed);
    entry.sequence.store(sequence + 1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);

    for (std::size_t i = 0; i < kWords; ++i) {
        entry.words[i].store(source[i], std::memory_order_relaxed);
    }

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
    std::array<std::uint64_t, kWords> w{};
    for (std::size_t i = 0; i < kWords; ++i) {
        w[i] = entry.words[i].load(std::memory_order_relaxed);
    }
    std::atomic_thread_fence(std::memory_order_acquire);
    if (entry.sequence.load(std::memory_order_relaxed) != before || w[0] != frame) {
        return false;
    }

    std::memcpy(static_cast<void*>(&out), w.data(), sizeof(out));
    return true;
}

}  // namespace zoom::ring
