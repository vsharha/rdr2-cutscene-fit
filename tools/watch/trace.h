// Copyright (c) 2026 Viktor Sharha

#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>

// Crash-proof diagnostics. Lines go straight to a file with one WriteFile call from
// a fixed buffer: no allocation, no locks, no CRT, so exception handlers and threads
// that are about to die can use it. Nothing is flushed; the OS keeps written data
// when the process dies.
namespace trace {

// Creates (truncates) the file. Lines written before this succeeds are dropped.
bool open(const std::filesystem::path& file);

// The address range reported as "+0xRVA" instead of an absolute address.
void set_module(std::uintptr_t base, std::size_t size);

// Installs a last-called vectored handler that records fatal-looking exceptions, up
// to 64 lines, and an unhandled exception filter that records and chains to the
// previous filter. Both stay installed.
void install_crash_observers();

// One line: "t=<ms> " plus whatever is appended, ended by end(). Text beyond the
// buffer is cut.
class Line {
public:
    Line();
    Line& text(const char* s);
    Line& hex(std::uint64_t value);
    Line& dec(std::uint64_t value);
    Line& address(std::uint64_t value);  // "+0xRVA" in the module, else "abs 0x..."
    void end();

private:
    char buffer_[384];
    std::size_t size_ = 0;
};

}  // namespace trace
