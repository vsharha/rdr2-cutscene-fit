// Copyright (c) 2026 Viktor Sharha
// Portions adapted from rdr2-ultrawide-proper-black-bar-fix, Copyright (c) 2026 rschi (MIT).

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace mem {

struct Region {
    std::uintptr_t base = 0;
    std::size_t size = 0;

    [[nodiscard]] std::uintptr_t end() const { return base + size; }
    [[nodiscard]] explicit operator bool() const { return base != 0 && size != 0; }
};

struct Section {
    std::string name;
    Region region;
    std::uint32_t characteristics = 0;
    bool executable = false;
};

// The process's main executable, sized by SizeOfImage. Empty if the headers are invalid.
Region main_module();

// Every section in header order. RDR2.exe has two sections named ".text" and two
// with no name, which are reported as "<unnamed #N>" with N the 1-based index.
std::vector<Section> sections(const Region& module);

std::vector<Section> executable_sections(const Region& module);

// The committed, accessible parts of a region, with adjacent parts merged. A packed
// image has PAGE_NOACCESS and guard pages inside its sections.
std::vector<Region> readable_subranges(const Region& region);

// Copies [addr, addr + len) into dest and zero-fills everything that cannot be
// read. Returns the number of zero-filled bytes.
std::size_t copy_readable(std::uintptr_t addr, std::uint8_t* dest, std::size_t len);

// File version of the main module such as "1.0.1491.50", or empty if unavailable.
std::string main_module_version();

}  // namespace mem
