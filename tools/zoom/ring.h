// Copyright (c) 2026 Viktor Sharha

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace zoom {

constexpr std::size_t kFrameBytes = 0x100;
constexpr std::size_t kAuxBytes = 0x40;

// Bits of Record::valid: the byte arrays that were copied completely.
constexpr std::uint8_t kValidFrame = 1U << 0;
constexpr std::uint8_t kValidP1 = 1U << 1;
constexpr std::uint8_t kValidP3 = 1U << 2;
constexpr std::uint8_t kValidOwner = 1U << 3;

// One call of the viewport builder. `frame` is the call counter assigned by ring::push.
// The size must be a multiple of 8: the ring stores records as 64-bit words.
struct Record {
    std::uint64_t frame = 0;
    std::uint64_t qpc = 0;
    // The pointer arguments of the call.
    std::uint64_t p1 = 0;
    std::uint64_t frame_pointer = 0;
    std::uint64_t p3 = 0;
    std::uint64_t owner = 0;
    float input = 0.0f;
    float corrected = 0.0f;
    float output = 0.0f;
    // The letterbox weight at anchor - 8.
    float weight = 0.0f;
    // The frame's flags after the call.
    std::uint16_t flags = 0;
    // The drawn enable byte at anchor + 0x21.
    std::uint8_t letterbox = 0;
    std::uint8_t mode = 0;
    std::uint8_t valid = 0;
    std::uint8_t reserved[3]{};
    // Copied before the call, so before this tool's and the plugin's field of view change.
    std::array<std::uint8_t, kFrameBytes> frame_bytes{};
    std::array<std::uint8_t, kAuxBytes> p1_bytes{};
    std::array<std::uint8_t, kAuxBytes> p3_bytes{};
    std::array<std::uint8_t, kAuxBytes> owner_bytes{};
};
static_assert(sizeof(Record) % sizeof(std::uint64_t) == 0);
static_assert(std::is_trivially_copyable_v<Record>);

// A fixed ring of per-frame records with one writer (the game's update thread) and any
// number of readers. Entries are guarded by a per-entry sequence counter, so a reader
// skips an entry that is being overwritten instead of returning a torn one.
namespace ring {

// 2400 records of 520 bytes: about 1.3 MB, 20 s at 120 frames per second.
constexpr std::size_t kCapacity = 2400;

// Appends a record and assigns it the next frame number. Does not allocate or lock.
void push(const Record& record);

// Number of records pushed so far; the newest has frame number count() - 1.
std::uint64_t count();

// Copies the record for `frame`. Returns false if it has been overwritten, was never
// written, or is being written right now.
bool read(std::uint64_t frame, Record& out);

}  // namespace ring

}  // namespace zoom
