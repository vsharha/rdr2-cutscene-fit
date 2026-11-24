// Copyright (c) 2026 Viktor Sharha

#pragma once

#include <cstddef>
#include <cstdint>

namespace zoom {

struct Record {
    std::uint64_t frame = 0;
    std::uint64_t qpc = 0;
    float input = 0.0f;
    float corrected = 0.0f;
    float output = 0.0f;
    std::uint16_t flags = 0;
    std::uint8_t letterbox = 0;
    std::uint8_t mode = 0;
};

// A fixed ring of per-frame records with one writer (the game's update thread) and any
// number of readers. Entries are guarded by a per-entry sequence counter, so a reader
// skips an entry that is being overwritten instead of returning a torn one.
namespace ring {

constexpr std::size_t kCapacity = 600;

// Appends a record and assigns it the next frame number. Does not allocate or lock.
void push(const Record& record);

// Number of records pushed so far; the newest has frame number count() - 1.
std::uint64_t count();

// Copies the record for `frame`. Returns false if it has been overwritten, was never
// written, or is being written right now.
bool read(std::uint64_t frame, Record& out);

}  // namespace ring

}  // namespace zoom
