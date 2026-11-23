// Copyright (c) 2026 Viktor Sharha
// Portions adapted from rdr2-ultrawide-proper-black-bar-fix, Copyright (c) 2026 rschi (MIT).

#include "common/mem.h"

#include <algorithm>
#include <format>
#include <string_view>
#include <windows.h>

#include <winver.h>

namespace mem {
namespace {

const IMAGE_NT_HEADERS64* nt_headers(const Region& module) {
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(module.base);
    return reinterpret_cast<const IMAGE_NT_HEADERS64*>(module.base + dos->e_lfanew);
}

bool is_readable(const MEMORY_BASIC_INFORMATION& mbi) {
    return mbi.State == MEM_COMMIT && (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) == 0;
}

}  // namespace

Region main_module() {
    const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    if (base == 0) {
        return {};
    }
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        return {};
    }
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) {
        return {};
    }
    return {base, nt->OptionalHeader.SizeOfImage};
}

std::vector<Section> sections(const Region& module) {
    std::vector<Section> result;
    if (!module) {
        return result;
    }

    const IMAGE_NT_HEADERS64* nt = nt_headers(module);
    const IMAGE_SECTION_HEADER* first = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        const IMAGE_SECTION_HEADER& header = first[i];

        // The name field is 8 bytes and not necessarily NUL-terminated.
        std::string_view name(reinterpret_cast<const char*>(header.Name), IMAGE_SIZEOF_SHORT_NAME);
        name = name.substr(0, name.find('\0'));

        Section section;
        section.name = name.empty() ? std::format("<unnamed #{}>", i + 1) : std::string(name);
        section.region = {module.base + header.VirtualAddress, header.Misc.VirtualSize};
        section.characteristics = header.Characteristics;
        section.executable = (header.Characteristics & IMAGE_SCN_MEM_EXECUTE) != 0;
        result.push_back(std::move(section));
    }
    return result;
}

std::vector<Section> executable_sections(const Region& module) {
    std::vector<Section> result = sections(module);
    std::erase_if(result, [](const Section& s) { return !s.executable || s.region.size == 0; });
    return result;
}

std::vector<Region> readable_subranges(const Region& region) {
    std::vector<Region> result;

    std::uintptr_t addr = region.base;
    while (addr < region.end()) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(reinterpret_cast<LPCVOID>(addr), &mbi, sizeof(mbi)) == 0) {
            break;
        }
        const auto block_end = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
        if (block_end <= addr) {
            break;
        }

        if (is_readable(mbi)) {
            const std::uintptr_t stop = (std::min)(region.end(), block_end);
            if (!result.empty() && result.back().end() == addr) {
                result.back().size += stop - addr;
            } else {
                result.push_back({addr, stop - addr});
            }
        }
        addr = block_end;
    }
    return result;
}

std::size_t copy_readable(std::uintptr_t addr, std::uint8_t* dest, std::size_t len) {
    std::fill_n(dest, len, std::uint8_t{0});

    std::size_t copied = 0;
    for (const Region& part : readable_subranges({addr, len})) {
        // ReadProcessMemory on our own process fails instead of faulting if a page's
        // protection changes after the query above.
        SIZE_T read = 0;
        ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(part.base),
                          dest + (part.base - addr), part.size, &read);
        copied += read;
    }
    return len - copied;
}

std::string main_module_version() {
    wchar_t path[MAX_PATH]{};
    if (GetModuleFileNameW(nullptr, path, MAX_PATH) == 0) {
        return {};
    }

    DWORD ignored = 0;
    const DWORD size = GetFileVersionInfoSizeW(path, &ignored);
    if (size == 0) {
        return {};
    }

    std::vector<std::byte> buffer(size);
    if (!GetFileVersionInfoW(path, 0, size, buffer.data())) {
        return {};
    }

    VS_FIXEDFILEINFO* info = nullptr;
    UINT info_size = 0;
    if (!VerQueryValueW(buffer.data(), L"\\", reinterpret_cast<LPVOID*>(&info), &info_size) ||
        info == nullptr) {
        return {};
    }
    return std::format("{}.{}.{}.{}", HIWORD(info->dwFileVersionMS), LOWORD(info->dwFileVersionMS),
                       HIWORD(info->dwFileVersionLS), LOWORD(info->dwFileVersionLS));
}

}  // namespace mem
