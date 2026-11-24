// Copyright (c) 2026 Viktor Sharha

#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace zoom::framelog {

constexpr const char* kFileName = "zoom-frames.log";

struct Result {
    bool written = false;
    std::size_t records = 0;
    std::size_t full_records = 0;
    std::size_t skipped = 0;
    std::uintmax_t bytes = 0;
};

// Appends a dump of the newest `frames` ring records to `file`: a header block that
// lists the fields, then one entry per record. Runs on the calling thread and
// allocates; never call it from the hook.
Result write(const std::filesystem::path& file, int frames, double qpc_frequency,
             const std::string& game_version);

}  // namespace zoom::framelog
