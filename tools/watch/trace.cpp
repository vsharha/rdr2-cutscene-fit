// Copyright (c) 2026 Viktor Sharha

#include "trace.h"

#include <atomic>
#include <windows.h>

namespace trace {
namespace {

HANDLE g_file = INVALID_HANDLE_VALUE;
std::atomic<std::uintptr_t> g_base{0};
std::atomic<std::size_t> g_size{0};
std::atomic<unsigned> g_observed{0};
constexpr unsigned kMaxObserved = 64;
LPTOP_LEVEL_EXCEPTION_FILTER g_previous_filter = nullptr;

bool fatal_looking(DWORD code) {
    switch (code) {
        case EXCEPTION_ACCESS_VIOLATION:
        case EXCEPTION_IN_PAGE_ERROR:
        case EXCEPTION_ILLEGAL_INSTRUCTION:
        case EXCEPTION_PRIV_INSTRUCTION:
        case EXCEPTION_STACK_OVERFLOW:
        case EXCEPTION_INT_DIVIDE_BY_ZERO:
        case EXCEPTION_SINGLE_STEP:
        case EXCEPTION_BREAKPOINT:
            return true;
        default:
            return false;
    }
}

void describe_exception(const char* tag, const EXCEPTION_POINTERS* info) {
    const EXCEPTION_RECORD& record = *info->ExceptionRecord;
    Line line;
    line.text(tag).text(" exc=").hex(record.ExceptionCode).text(" rip=").address(
        reinterpret_cast<std::uint64_t>(record.ExceptionAddress));
    line.text(" tid=").dec(GetCurrentThreadId());
    if (info->ContextRecord != nullptr) {
        line.text(" rsp=").hex(info->ContextRecord->Rsp).text(" dr6=").hex(info->ContextRecord->Dr6);
    }
    if (record.ExceptionCode == EXCEPTION_ACCESS_VIOLATION && record.NumberParameters >= 2) {
        line.text(" av_kind=").dec(record.ExceptionInformation[0]).text(" av_addr=").hex(
            record.ExceptionInformation[1]);
    }
    line.end();
}

LONG CALLBACK observe(EXCEPTION_POINTERS* info) {
    if (fatal_looking(info->ExceptionRecord->ExceptionCode) &&
        g_observed.fetch_add(1, std::memory_order_relaxed) < kMaxObserved) {
        describe_exception("X", info);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

LONG WINAPI unhandled(EXCEPTION_POINTERS* info) {
    describe_exception("UNHANDLED", info);
    return g_previous_filter != nullptr ? g_previous_filter(info) : EXCEPTION_CONTINUE_SEARCH;
}

}  // namespace

bool open(const std::filesystem::path& file) {
    // Append-only access makes each WriteFile land at the end whatever the thread.
    g_file = CreateFileW(file.c_str(), FILE_APPEND_DATA | SYNCHRONIZE, FILE_SHARE_READ, nullptr,
                         CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    return g_file != INVALID_HANDLE_VALUE;
}

void set_module(std::uintptr_t base, std::size_t size) {
    g_base.store(base);
    g_size.store(size);
}

void install_crash_observers() {
    AddVectoredExceptionHandler(0, observe);
    g_previous_filter = SetUnhandledExceptionFilter(unhandled);
}

Line::Line() {
    text("t=").dec(GetTickCount64()).text(" ");
}

Line& Line::text(const char* s) {
    while (*s != '\0' && size_ + 1 < sizeof(buffer_)) {
        buffer_[size_++] = *s++;
    }
    return *this;
}

Line& Line::hex(std::uint64_t value) {
    char digits[16];
    int n = 0;
    do {
        digits[n++] = "0123456789ABCDEF"[value & 0xF];
        value >>= 4;
    } while (value != 0);
    text("0x");
    while (n > 0 && size_ + 1 < sizeof(buffer_)) {
        buffer_[size_++] = digits[--n];
    }
    return *this;
}

Line& Line::dec(std::uint64_t value) {
    char digits[20];
    int n = 0;
    do {
        digits[n++] = static_cast<char>('0' + value % 10);
        value /= 10;
    } while (value != 0);
    while (n > 0 && size_ + 1 < sizeof(buffer_)) {
        buffer_[size_++] = digits[--n];
    }
    return *this;
}

Line& Line::address(std::uint64_t value) {
    const std::uintptr_t base = g_base.load(std::memory_order_relaxed);
    if (value >= base && value - base < g_size.load(std::memory_order_relaxed)) {
        return text("+").hex(value - base);
    }
    return text("abs ").hex(value);
}

void Line::end() {
    if (g_file == INVALID_HANDLE_VALUE) {
        return;
    }
    buffer_[size_++] = '\n';
    DWORD written = 0;
    WriteFile(g_file, buffer_, static_cast<DWORD>(size_), &written, nullptr);
}

}  // namespace trace
