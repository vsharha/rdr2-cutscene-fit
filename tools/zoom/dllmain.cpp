// Copyright (c) 2026 Viktor Sharha

// Tests whether narrowing the camera's field of view at the call to the viewport builder
// (+0x3C6168, called from +0x3212DE once per frame) zooms the drawn picture cleanly,
// including during camera blends and at cuts.
//
// The call's rel32 is redirected to a stub near the call site that jumps to our function.
// It reads the input field of view from the camera frame, optionally replaces it with
//   new = 2 * atan(k * tan(old / 2))
// calls the original, and records the input, the value written, the output after the
// game's own adjustment and clamp, the frame's flags, the letterbox weight and enable
// byte, the four pointer arguments, the first 0x100 bytes of the camera frame (copied
// before the call) and the first 0x40 bytes behind p1, p3 and owner in a ring buffer of
// the last 2400 frames (about 1.3 MB). With mode off the call is passed through unchanged and the tool
// only records. Nothing is patched if the call site or the callee does not match the
// expected bytes; a callee that starts with a jump (E9), as when CutsceneFit has hooked
// it with MinHook, is accepted and logged, so this tool and the plugin can run together.
//
// Settings are read from "zoom.ini" next to zoom.asi, section [zoom]:
//   settle_seconds   wait after the plugin loads before verifying and hooking. Default: 15.
//   mode             off, always (correct every frame) or letterbox (correct only while the
//                    letterbox enable byte is non-zero). Default: off.
//   aspect           screen aspect ratio used for k. 0: the client area of the game's window,
//                    else the primary monitor's resolution. Default: 0.
//   scale            k, overriding the formula k = (16/9) / min(aspect, 2.35). 0: use the
//                    formula. A value such as 0.5 makes the effect obvious. Default: 0.
//   dump_frames      frames written by the dump key, 1..2400. Default: 2400 (the whole ring).
//   mode_key         cycles off, always, letterbox. Default: F7.
//   dump_key         appends the last dump_frames records, oldest first, to zoom-frames.log
//                    next to zoom.asi (zoom.log's directory), and logs one line about it.
//                    The file is not truncated; each dump starts with a "# zoom-frames dump N"
//                    header block describing the fields. The first record, every record with
//                    flag 0x80 and the 3 records on each side carry the full set (pointers,
//                    weight, enable byte, flags, fov, frame bytes 0..0xFF, bytes behind p1,
//                    p3 and owner); the others carry the scalars and the frame qwords that
//                    differ from the previous record, plus the p3 qwords that differ when p3
//                    is the same pointer. Records within 3 of a cut or of a change of p3 are
//                    full. Every R line has p3vt and ownervt, the first qword at p3 and at
//                    owner. A dump of 2400 records is about 1 MB typically and at most about
//                    3.2 MB. Default: F8.
//   hook_key         removes the hook, or installs it again. Default: F6.
// zoom.log gets a summary line every second while the mode is not off, every 10 s otherwise.
// A key is a name (F1..F24, A..Z, 0..9) or a virtual-key code such as 0x78.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <format>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#include <windows.h>

#include "common/keys.h"
#include "common/log.h"
#include "common/mem.h"
#include "common/paths.h"
#include "framelog.h"
#include "hook.h"
#include "ring.h"

namespace {

using zoom::Mode;

constexpr const char* kExpectedVersion = "1.0.1491.50";
constexpr std::uintptr_t kViewportPointerRva = 0x3EC2610;
constexpr std::uintptr_t kViewportOffset = 0x10;
constexpr std::uintptr_t kViewportFovOffset = 0x640;
constexpr std::uintptr_t kViewArrayRva = 0x3EC0B00;
constexpr std::uintptr_t kViewFovOffset = 0x550;
constexpr std::uintptr_t kShaderFovRva = 0x3AE24B8;
constexpr double kReferenceAspect = 16.0 / 9.0;
constexpr double kMaxAspect = 2.35;
constexpr auto kPollInterval = std::chrono::milliseconds(20);
constexpr auto kSummaryActive = std::chrono::seconds(1);
constexpr auto kSummaryIdle = std::chrono::seconds(10);

std::atomic<bool> g_stop{false};

struct Settings {
    int settle_seconds = 15;
    Mode mode = Mode::Off;
    double aspect = 0.0;
    double scale = 0.0;
    int dump_frames = static_cast<int>(zoom::ring::kCapacity);
    int mode_key = VK_F7;
    int dump_key = VK_F8;
    int hook_key = VK_F6;
};

std::wstring read_string(const wchar_t* key, const wchar_t* fallback, const std::wstring& ini) {
    std::vector<wchar_t> buffer(512);
    GetPrivateProfileStringW(L"zoom", key, fallback, buffer.data(), static_cast<DWORD>(buffer.size()),
                             ini.c_str());
    return buffer.data();
}

int read_int(const wchar_t* key, int fallback, int low, int high, const std::wstring& ini) {
    const int value = static_cast<int>(GetPrivateProfileIntW(L"zoom", key, fallback, ini.c_str()));
    return std::clamp(value, low, high);
}

double read_double(const wchar_t* key, double fallback, const std::wstring& ini) {
    const std::wstring text = read_string(key, L"", ini);
    wchar_t* end = nullptr;
    const double value = std::wcstod(text.c_str(), &end);
    if (text.empty() || end == text.c_str() || !std::isfinite(value) || value < 0.0) {
        return fallback;
    }
    return value;
}

std::optional<Mode> parse_mode(const std::wstring& text) {
    const std::string s = logger::utf8(std::filesystem::path(text));
    if (s == "off") {
        return Mode::Off;
    }
    if (s == "always") {
        return Mode::Always;
    }
    if (s == "letterbox") {
        return Mode::Letterbox;
    }
    return std::nullopt;
}

Settings read_settings(const std::wstring& ini) {
    Settings s;
    s.settle_seconds = read_int(L"settle_seconds", s.settle_seconds, 0, 600, ini);
    const std::wstring mode_text = read_string(L"mode", L"off", ini);
    if (const auto mode = parse_mode(mode_text)) {
        s.mode = *mode;
    } else {
        logger::info("mode \"{}\" is not off, always or letterbox; using off",
                     logger::utf8(std::filesystem::path(mode_text)));
    }
    s.aspect = read_double(L"aspect", s.aspect, ini);
    s.scale = read_double(L"scale", s.scale, ini);
    s.dump_frames = read_int(L"dump_frames", s.dump_frames, 1, static_cast<int>(zoom::ring::kCapacity), ini);
    s.mode_key = keys::parse(read_string(L"mode_key", L"F7", ini), s.mode_key);
    s.dump_key = keys::parse(read_string(L"dump_key", L"F8", ini), s.dump_key);
    s.hook_key = keys::parse(read_string(L"hook_key", L"F6", ini), s.hook_key);
    return s;
}

struct WindowSearch {
    DWORD pid = 0;
    long best_area = 0;
    double aspect = 0.0;
};

BOOL CALLBACK consider_window(HWND window, LPARAM parameter) {
    auto& search = *reinterpret_cast<WindowSearch*>(parameter);
    DWORD pid = 0;
    GetWindowThreadProcessId(window, &pid);
    RECT client{};
    if (pid != search.pid || !IsWindowVisible(window) || !GetClientRect(window, &client)) {
        return TRUE;
    }
    const long width = client.right - client.left;
    const long height = client.bottom - client.top;
    if (width > 0 && height > 0 && width * height > search.best_area) {
        search.best_area = width * height;
        search.aspect = static_cast<double>(width) / static_cast<double>(height);
    }
    return TRUE;
}

struct Correction {
    double aspect = 0.0;
    std::string source;
    double scale = 1.0;
};

Correction resolve_correction(const Settings& settings) {
    Correction c;
    if (settings.aspect > 0.0) {
        c.aspect = settings.aspect;
        c.source = "ini";
    } else {
        WindowSearch search;
        search.pid = GetCurrentProcessId();
        EnumWindows(consider_window, reinterpret_cast<LPARAM>(&search));
        if (search.aspect > 0.0) {
            c.aspect = search.aspect;
            c.source = "game window client area";
        } else {
            const int width = GetSystemMetrics(SM_CXSCREEN);
            const int height = GetSystemMetrics(SM_CYSCREEN);
            c.aspect = height > 0 ? static_cast<double>(width) / height : kReferenceAspect;
            c.source = "primary monitor";
        }
    }
    if (settings.scale > 0.0) {
        c.scale = settings.scale;
    } else {
        c.scale = kReferenceAspect / std::min(c.aspect, kMaxAspect);
    }
    return c;
}

void apply_correction(const Settings& settings) {
    const Correction c = resolve_correction(settings);
    zoom::hook::set_scale(static_cast<float>(c.scale));
    logger::info("aspect {:.4f} ({}), k {:.5f}{}", c.aspect, c.source, c.scale,
                 settings.scale > 0.0 ? " (scale from ini)" : "");
}

bool pressed(int key, bool& was_down) {
    const bool down = (GetAsyncKeyState(key) & 0x8000) != 0;
    const bool edge = down && !was_down;
    was_down = down;
    return edge;
}

template <typename T>
std::optional<T> read_value(std::uintptr_t address) {
    T value{};
    if (mem::copy_readable(address, reinterpret_cast<std::uint8_t*>(&value), sizeof(value)) != 0) {
        return std::nullopt;
    }
    return value;
}

std::string show(const std::optional<float>& value) {
    return value ? std::format("{:.4f}", *value) : "unreadable";
}

std::string show(const std::optional<std::uint8_t>& value) {
    return value ? std::format("{}", static_cast<int>(*value)) : "unreadable";
}

std::optional<float> viewport_fov(std::uintptr_t base) {
    const auto owner = read_value<std::uint64_t>(base + kViewportPointerRva);
    if (!owner || *owner == 0) {
        return std::nullopt;
    }
    return read_value<float>(*owner + kViewportOffset + kViewportFovOffset);
}

class Reporter {
public:
    Reporter(const mem::Region& module, std::string version)
        : base_(module.base), version_(std::move(version)) {
        LARGE_INTEGER frequency;
        QueryPerformanceFrequency(&frequency);
        frequency_ = static_cast<double>(frequency.QuadPart);
        last_time_ = std::chrono::steady_clock::now();
        last_count_ = zoom::ring::count();
    }

    void tick(std::chrono::steady_clock::time_point now) {
        const auto interval = zoom::hook::mode() == Mode::Off ? kSummaryIdle : kSummaryActive;
        if (now - last_time_ < interval) {
            return;
        }
        const std::uint64_t count = zoom::ring::count();
        const double seconds = std::chrono::duration<double>(now - last_time_).count();
        zoom::Record last;
        const bool have_last = count != 0 && zoom::ring::read(count - 1, last);
        const auto shader = read_value<float>(base_ + kShaderFovRva);
        const auto slot0 = read_value<float>(base_ + kViewArrayRva + kViewFovOffset);
        const auto letterbox = read_value<std::uint8_t>(base_ + zoom::hook::kLetterboxEnableRva);

        logger::info("summary: {} calls in {:.2f} s, last input {} output {}, viewport fov {}, "
                     "view slot 0 fov {}, shader fov {}, letterbox {}, mode {}, hook {}",
                     count - last_count_, seconds,
                     have_last ? show(std::optional<float>(last.input)) : "none",
                     have_last ? show(std::optional<float>(last.output)) : "none",
                     show(viewport_fov(base_)), show(slot0), show(shader), show(letterbox),
                     zoom::mode_name(zoom::hook::mode()),
                     zoom::hook::installed() ? "installed" : "removed");
        last_time_ = now;
        last_count_ = count;
    }

    // Appends the newest `frames` records to zoom-frames.log, next to zoom.log.
    void dump(int frames) const {
        std::filesystem::path directory = logger::directory();
        if (directory.empty()) {
            directory = paths::fallback_directory();
        }
        const std::filesystem::path file = directory / zoom::framelog::kFileName;
        const zoom::framelog::Result result = zoom::framelog::write(file, frames, frequency_, version_);
        if (!result.written) {
            logger::info("dump: could not write {}", logger::utf8(file));
            return;
        }
        logger::info("dump: {} records ({} with the full set, {} skipped), {} bytes appended to {}",
                     result.records, result.full_records, result.skipped, result.bytes,
                     logger::utf8(file));
    }

private:
    std::uintptr_t base_;
    double frequency_ = 1.0;
    std::string version_;
    std::chrono::steady_clock::time_point last_time_{};
    std::uint64_t last_count_ = 0;
};

Mode next_mode(Mode mode) {
    switch (mode) {
        case Mode::Off:
            return Mode::Always;
        case Mode::Always:
            return Mode::Letterbox;
        case Mode::Letterbox:
            break;
    }
    return Mode::Off;
}

DWORD WINAPI run(LPVOID parameter) {
    const auto self = static_cast<HMODULE>(parameter);
    try {
        logger::open(paths::module_path(self));

        const std::filesystem::path asi = paths::module_path(self);
        const std::wstring ini = std::filesystem::path(asi).replace_extension(L".ini").wstring();
        const Settings settings = read_settings(ini);

        const mem::Region module = mem::main_module();
        if (!module) {
            logger::info("the main module has no valid PE header");
            return 1;
        }
        const std::string version = mem::main_module_version();
        logger::info("module base 0x{:X}, size 0x{:X}, game version {}", module.base, module.size,
                     version);
        if (version != kExpectedVersion) {
            logger::info("WARNING: offsets are for {}", kExpectedVersion);
        }
        logger::info("settings: settle_seconds={} mode={} aspect={} scale={} dump_frames={} "
                     "mode_key=0x{:X} dump_key=0x{:X} hook_key=0x{:X}",
                     settings.settle_seconds, zoom::mode_name(settings.mode), settings.aspect,
                     settings.scale, settings.dump_frames, settings.mode_key, settings.dump_key,
                     settings.hook_key);

        std::this_thread::sleep_for(std::chrono::seconds(settings.settle_seconds));

        if (!zoom::hook::verify(module)) {
            logger::info("not patching");
            return 1;
        }
        zoom::hook::set_mode(settings.mode);
        apply_correction(settings);
        zoom::hook::install();
        logger::info("ready: key 0x{:X} cycles the mode, 0x{:X} dumps the ring to zoom-frames.log, 0x{:X} removes or "
                     "installs the hook", settings.mode_key, settings.dump_key, settings.hook_key);

        Reporter reporter(module, version);
        bool mode_down = false;
        bool dump_down = false;
        bool hook_down = false;
        while (!g_stop.load()) {
            if (pressed(settings.mode_key, mode_down)) {
                const Mode mode = next_mode(zoom::hook::mode());
                zoom::hook::set_mode(mode);
                logger::info("mode {}", zoom::mode_name(mode));
                apply_correction(settings);
            }
            if (pressed(settings.dump_key, dump_down)) {
                reporter.dump(settings.dump_frames);
            }
            if (pressed(settings.hook_key, hook_down)) {
                if (zoom::hook::installed()) {
                    zoom::hook::remove();
                } else {
                    zoom::hook::install();
                }
            }
            reporter.tick(std::chrono::steady_clock::now());
            std::this_thread::sleep_for(kPollInterval);
        }
        return 0;
    } catch (const std::exception& e) {
        logger::info("failed: {}", e.what());
        return 1;
    }
}

}  // namespace

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(instance);
        if (HANDLE thread = CreateThread(nullptr, 0, run, instance, 0, nullptr)) {
            CloseHandle(thread);
        }
    } else if (reason == DLL_PROCESS_DETACH && reserved == nullptr) {
        g_stop.store(true);
        zoom::hook::shutdown();
    }
    return TRUE;
}
