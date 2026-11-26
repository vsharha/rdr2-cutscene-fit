// Copyright (c) 2026 Viktor Sharha

// Runs game::resolve on an image written by dump.asi, whose file offsets equal the
// game's RVAs, so the file can stand in for the loaded module. Passes if every pattern
// matches exactly once, at the offset recorded for it, and the matches agree with
// layout.h.
//
// Usage: signatures <RDR2.dump.exe>. Exits with 77 if the file does not exist.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

#include "common/log.h"
#include "common/mem.h"
#include "game.h"

namespace {

constexpr int kSkipped = 77;
constexpr std::uint32_t kScnMemExecute = 0x20000000;

std::vector<std::uint8_t> g_image;
std::vector<std::string> g_log;

template <typename T>
T read(std::size_t offset) {
    T value{};
    if (offset + sizeof(T) <= g_image.size()) {
        std::memcpy(&value, g_image.data() + offset, sizeof(T));
    }
    return value;
}

std::uintptr_t image_base() {
    return reinterpret_cast<std::uintptr_t>(g_image.data());
}

}  // namespace

// The parts of the common library that game.cpp and pattern.cpp use, over g_image
// instead of the process's memory.
namespace logger {

void write(std::string_view line) {
    std::printf("%.*s\n", static_cast<int>(line.size()), line.data());
    g_log.emplace_back(line);
}

}  // namespace logger

namespace mem {

std::vector<Section> executable_sections(const Region& module) {
    std::vector<Section> result;
    const auto nt = read<std::uint32_t>(0x3C);
    const auto count = read<std::uint16_t>(nt + 6);
    const auto optional_size = read<std::uint16_t>(nt + 20);
    const std::size_t table = nt + 24 + optional_size;
    for (std::size_t i = 0; i < count; ++i) {
        const std::size_t header = table + i * 40;
        const auto virtual_size = read<std::uint32_t>(header + 8);
        const auto rva = read<std::uint32_t>(header + 12);
        const auto characteristics = read<std::uint32_t>(header + 36);
        if ((characteristics & kScnMemExecute) == 0 || virtual_size == 0 || rva >= g_image.size()) {
            continue;
        }
        Section section;
        // The name field is 8 bytes and not necessarily NUL-terminated.
        std::string_view name(reinterpret_cast<const char*>(g_image.data() + header), 8);
        section.name = name.substr(0, name.find('\0'));
        const std::size_t size = std::min<std::size_t>(virtual_size, g_image.size() - rva);
        section.region = {module.base + rva, size};
        section.characteristics = characteristics;
        section.executable = true;
        result.push_back(std::move(section));
    }
    return result;
}

// Every byte of the file is readable.
std::vector<Region> readable_subranges(const Region& region) {
    return {region};
}

}  // namespace mem

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: signatures <RDR2.dump.exe>\n");
        return 2;
    }
    const std::filesystem::path path = argv[1];
    if (!std::filesystem::exists(path)) {
        std::printf("%s does not exist; skipped\n", argv[1]);
        return kSkipped;
    }
    std::ifstream in(path, std::ios::binary);
    g_image.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    if (read<std::uint16_t>(0) != 0x5A4D || read<std::uint32_t>(read<std::uint32_t>(0x3C)) != 0x4550) {
        std::fprintf(stderr, "%s is not a PE image\n", argv[1]);
        return 1;
    }

    const mem::Region module{image_base(), g_image.size()};
    game::Addresses addresses;
    if (game::resolve(module, addresses, true) != game::Resolve::Found) {
        std::fprintf(stderr, "FAIL: the patterns do not resolve\n");
        return 1;
    }
    for (const std::string& line : g_log) {
        if (line.find("differs from the expected offset") != std::string::npos) {
            std::fprintf(stderr, "FAIL: a pattern matched away from its recorded offset\n");
            return 1;
        }
    }
    std::printf("PASS\n");
    return 0;
}
