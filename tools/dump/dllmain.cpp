// Copyright (c) 2026 Viktor Sharha

// Writes the unpacked RDR2.exe image to disk so it can be loaded in Ghidra.
//
// Settings are read from "dump.ini" next to dump.asi, section [dump]:
//   output               path of the dump; relative paths are taken from the .asi
//                        folder. Default: "RDR2.dump.exe" in the log's folder.
//   max_wait_seconds     how long to wait for the code to unpack. Default: 300.
//   settle_seconds       extra wait after unpacking is detected. Default: 10.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <exception>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>
#include <windows.h>

#include "common/log.h"
#include "common/mem.h"
#include "common/paths.h"
#include "image.h"

namespace {

// 'mov [rsp+8], rbx' is one of the most common x64 prologues. The packed file
// holds about 15 of them; the unpacked game holds tens of thousands.
constexpr std::uint8_t kPrologue[] = {0x48, 0x89, 0x5C, 0x24, 0x08};
constexpr std::size_t kUnpackedPrologueCount = 5000;
constexpr auto kPollInterval = std::chrono::seconds(2);

bool has_unpacked_code(const std::vector<mem::Section>& executable) {
    std::size_t count = 0;
    for (const mem::Section& section : executable) {
        for (const mem::Region& part : mem::readable_subranges(section.region)) {
            const auto* data = reinterpret_cast<const std::uint8_t*>(part.base);
            const std::uint8_t* const end = data + part.size;
            while (static_cast<std::size_t>(end - data) >= sizeof(kPrologue)) {
                data = static_cast<const std::uint8_t*>(
                    std::memchr(data, kPrologue[0], static_cast<std::size_t>(end - data)));
                if (data == nullptr) {
                    break;
                }
                if (static_cast<std::size_t>(end - data) >= sizeof(kPrologue) &&
                    std::memcmp(data, kPrologue, sizeof(kPrologue)) == 0 &&
                    ++count >= kUnpackedPrologueCount) {
                    return true;
                }
                ++data;
            }
        }
    }
    return false;
}

void log_module(const mem::Region& module) {
    logger::info("module base 0x{:X}, size 0x{:X} ({} bytes), game version {}", module.base,
                 module.size, module.size, mem::main_module_version());
    for (const mem::Section& s : mem::sections(module)) {
        logger::info("  section {:<14} rva 0x{:08X} size 0x{:08X} flags 0x{:08X}{}", s.name,
                     s.region.base - module.base, s.region.size, s.characteristics,
                     s.executable ? " executable" : "");
    }
}

DWORD WINAPI run(LPVOID parameter) {
    const auto self = static_cast<HMODULE>(parameter);
    try {
        logger::open(self);

        const std::filesystem::path asi = paths::module_path(self);
        const std::wstring ini = std::filesystem::path(asi).replace_extension(L".ini").wstring();
        const int max_wait = GetPrivateProfileIntW(L"dump", L"max_wait_seconds", 300, ini.c_str());
        const int settle = GetPrivateProfileIntW(L"dump", L"settle_seconds", 10, ini.c_str());
        std::vector<wchar_t> configured(1024);
        GetPrivateProfileStringW(L"dump", L"output", L"", configured.data(),
                                 static_cast<DWORD>(configured.size()), ini.c_str());

        std::filesystem::path output = configured.data();
        if (output.empty()) {
            const std::filesystem::path directory = logger::directory();
            output = (directory.empty() ? asi.parent_path() : directory) / L"RDR2.dump.exe";
        } else if (output.is_relative()) {
            output = asi.parent_path() / output;
        }

        const mem::Region module = mem::main_module();
        if (!module) {
            logger::info("the main module has no valid PE header");
            return 1;
        }
        log_module(module);

        const std::vector<mem::Section> executable = mem::executable_sections(module);
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(std::max(max_wait, 0));
        int attempts = 1;
        while (!has_unpacked_code(executable)) {
            if (std::chrono::steady_clock::now() >= deadline) {
                logger::info("code still packed after {} attempts; nothing written", attempts);
                return 1;
            }
            std::this_thread::sleep_for(kPollInterval);
            ++attempts;
        }
        logger::info("unpacked code found on attempt {}; waiting {} s", attempts, settle);
        std::this_thread::sleep_for(std::chrono::seconds(std::max(settle, 0)));

        logger::info("writing {}", logger::utf8(output));
        const dump::Result result = dump::write_image(module, output);
        if (!result.ok) {
            logger::info("write failed after {} bytes", result.bytes_written);
            return 1;
        }
        logger::info("wrote {} bytes to {} in {:.1f} s; {} bytes zero-filled as unreadable",
                     result.bytes_written, logger::utf8(output), result.seconds,
                     result.zero_filled);
    } catch (const std::exception& e) {
        logger::info("failed: {}", e.what());
        return 1;
    }
    return 0;
}

}  // namespace

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(instance);
        if (HANDLE thread = CreateThread(nullptr, 0, run, instance, 0, nullptr)) {
            CloseHandle(thread);
        }
    }
    return TRUE;
}
