// Copyright (c) 2026 Viktor Sharha
// Portions adapted from rdr2-ultrawide-proper-black-bar-fix, Copyright (c) 2026 rschi (MIT).

#pragma once

#include <cstddef>
#include <filesystem>

#include "common/mem.h"

namespace dump {

struct Result {
    bool ok = false;
    std::size_t bytes_written = 0;
    std::size_t zero_filled = 0;
    double seconds = 0.0;
};

// Writes the in-memory image of `module` to `out` as a PE file whose file offsets
// equal its RVAs, so addresses in the process map straight onto offsets in the file.
// The import table is left as the loader resolved it.
Result write_image(const mem::Region& module, const std::filesystem::path& out);

}  // namespace dump
