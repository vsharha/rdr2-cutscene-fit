// Copyright (c) 2026 Viktor Sharha
// Portions adapted from rdr2-ultrawide-proper-black-bar-fix, Copyright (c) 2026 rschi (MIT).

#include "image.h"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <vector>
#include <windows.h>

namespace dump {
namespace {

constexpr std::size_t kChunk = 0x100000;

// Rewrites the section table in the copied headers so that raw file offsets equal
// RVAs. `image_size` is the number of bytes the dump will contain.
bool realign_headers(std::uint8_t* headers, std::size_t size, std::size_t image_size) {
    if (size < sizeof(IMAGE_DOS_HEADER)) {
        return false;
    }
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(headers);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < 0 ||
        static_cast<std::size_t>(dos->e_lfanew) + sizeof(IMAGE_NT_HEADERS64) > size) {
        return false;
    }
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(headers + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) {
        return false;
    }

    const DWORD alignment = nt->OptionalHeader.SectionAlignment;
    if (alignment == 0) {
        return false;
    }
    nt->OptionalHeader.FileAlignment = alignment;

    IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(nt);
    if (reinterpret_cast<std::uint8_t*>(section + nt->FileHeader.NumberOfSections) >
        headers + size) {
        return false;
    }

    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        const DWORD rva = section[i].VirtualAddress;
        DWORD raw_size = (section[i].Misc.VirtualSize + alignment - 1) / alignment * alignment;

        // SizeOfImage need not be a multiple of SectionAlignment, so the last section
        // must be clamped or the header claims bytes past the end of the file and
        // Ghidra reports a truncated section.
        if (rva >= image_size) {
            raw_size = 0;
        } else if (static_cast<std::size_t>(rva) + raw_size > image_size) {
            raw_size = static_cast<DWORD>(image_size - rva);
        }

        section[i].PointerToRawData = rva;
        section[i].SizeOfRawData = raw_size;
    }
    return true;
}

}  // namespace

Result write_image(const mem::Region& module, const std::filesystem::path& out) {
    Result result;
    if (!module) {
        return result;
    }
    const auto start = std::chrono::steady_clock::now();

    std::error_code ec;
    std::filesystem::create_directories(out.parent_path(), ec);

    std::ofstream file(out, std::ios::binary | std::ios::trunc);
    if (!file) {
        return result;
    }

    std::vector<std::uint8_t> buffer(kChunk);
    for (std::size_t offset = 0; offset < module.size; offset += kChunk) {
        const std::size_t length = (std::min)(kChunk, module.size - offset);
        result.zero_filled += mem::copy_readable(module.base + offset, buffer.data(), length);

        if (offset == 0 && !realign_headers(buffer.data(), length, module.size)) {
            return result;
        }
        file.write(reinterpret_cast<const char*>(buffer.data()),
                   static_cast<std::streamsize>(length));
        if (!file) {
            return result;
        }
        result.bytes_written += length;
    }

    file.close();
    result.ok = !file.fail();
    result.seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    return result;
}

}  // namespace dump
