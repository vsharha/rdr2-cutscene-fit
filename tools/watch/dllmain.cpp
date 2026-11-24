// Copyright (c) 2026 Viktor Sharha

// Finds which instructions write the field of view of the camera that is drawn,
// using hardware write watchpoints on elements of the view array.
//
// Settings are read from "watch.ini" next to watch.asi, section [watch]:
//   settle_seconds       wait after the plugin loads before the first survey. Default: 15.
//   survey_slots         how many view array elements the survey lists (1..256). Default: 16.
//   survey_repeats       surveys logged automatically after the settle delay. Default: 3.
//   survey_interval_ms   pause between those surveys. Default: 1000.
//   arm_key              toggles the watchpoints. Default: F9.
//   survey_key           logs a survey on demand. Default: F10.
//   slots                what to watch, up to 4 elements: "auto", or a list such as
//                        "0,1,2,3" or "4-7". "auto" takes survey values in 1..179 degrees,
//                        one element per distinct value first, then any other plausible
//                        element, then the lowest unused indices. Default: auto.
//   max_slots            most elements watched, 1..4. Default: 4.
//   stack                call stack recorded per hit: "none"; "scan" (first values on the raw
//                        stack that point into the game's executable sections); "unwind"
//                        (RtlVirtualUnwind; can fault on unusual unwind data). Default: scan.
//   scan_qwords          stack qwords examined by "scan". Default: 256.
//   trust_dr6            1: a hit needs its DR6 bit. 0: a single step without any of our DR6
//                        bits and without the trap flag, while armed, also counts as a hit
//                        (recorded as slot 255 if several slots are watched). Default: 0.
//   arm_threads          "all", or "main" for only the thread that loaded the plugin.
//                        Default: all.
//   max_threads          cap on armed threads, in Toolhelp order; 0 for none. Default: 0.
//   arm_delay_ms         pause between threads while arming. Default: 0.
//   flush_seconds        how often the hit table is logged while armed. Default: 5.
//   rearm_seconds        how often threads created since arming are covered. Default: 2.
//   rearm_all            1: rewrite the registers of every thread on each rearm instead
//                        of only new threads. Default: 0.
//   table_rows           most rows of the hit table logged per flush. Default: 40.
// Besides watch.log, "watch.trace" next to it receives unbuffered lines written from
// exception handlers and between arming steps; see trace.h.
// A key is a name (F1..F24, A..Z, 0..9) or a virtual-key code such as 0x78.

#include <algorithm>
#include <atomic>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <exception>
#include <filesystem>
#include <optional>
#include <string>
#include <thread>
#include <vector>
#include <windows.h>

#include "common/log.h"
#include "common/mem.h"
#include "common/paths.h"
#include "trace.h"
#include "watchpoint.h"

namespace {

constexpr const char* kExpectedVersion = "1.0.1491.50";
constexpr std::uintptr_t kViewArrayRva = 0x3EC0B00;
constexpr std::uintptr_t kViewStride = 0x690;
constexpr std::uintptr_t kFovOffset = 0x550;
constexpr std::uintptr_t kShaderFovRva = 0x3AE24B8;
constexpr int kMaxSurveySlots = 256;
// The thread that attached the DLL: the game's main thread if the loader runs there.
std::atomic<DWORD> g_loader_thread_id{0};
constexpr auto kPollInterval = std::chrono::milliseconds(20);

struct Settings {
    int settle_seconds = 15;
    int survey_slots = 16;
    int survey_repeats = 3;
    int survey_interval_ms = 1000;
    int arm_key = VK_F9;
    int survey_key = VK_F10;
    std::string slots = "auto";
    int max_slots = 4;
    bool trust_dr6 = false;
    std::string arm_threads = "all";
    int max_threads = 0;
    int arm_delay_ms = 0;
    std::string stack = "scan";
    int scan_qwords = 256;
    int flush_seconds = 5;
    int rearm_seconds = 2;
    bool rearm_all = false;
    int table_rows = 40;
};

std::wstring read_string(const wchar_t* key, const wchar_t* fallback, const std::wstring& ini) {
    std::vector<wchar_t> buffer(512);
    GetPrivateProfileStringW(L"watch", key, fallback, buffer.data(),
                             static_cast<DWORD>(buffer.size()), ini.c_str());
    return buffer.data();
}

int read_int(const wchar_t* key, int fallback, int low, int high, const std::wstring& ini) {
    const int value = static_cast<int>(GetPrivateProfileIntW(L"watch", key, fallback, ini.c_str()));
    return std::clamp(value, low, high);
}

std::string narrow(const std::wstring& wide) {
    return logger::utf8(std::filesystem::path(wide));
}

int parse_key(const std::wstring& text, int fallback) {
    std::wstring s;
    for (const wchar_t c : text) {
        if (!std::iswspace(c)) {
            s += static_cast<wchar_t>(std::towupper(c));
        }
    }
    if (s.size() == 1 && ((s[0] >= L'A' && s[0] <= L'Z') || (s[0] >= L'0' && s[0] <= L'9'))) {
        return s[0];
    }
    if (s.size() >= 2 && s[0] == L'F') {
        wchar_t* end = nullptr;
        const long n = std::wcstol(s.c_str() + 1, &end, 10);
        if (*end == L'\0' && n >= 1 && n <= 24) {
            return VK_F1 + static_cast<int>(n) - 1;
        }
    }
    wchar_t* end = nullptr;
    const long code = std::wcstol(s.c_str(), &end, 0);
    if (!s.empty() && *end == L'\0' && code > 0 && code < 256) {
        return static_cast<int>(code);
    }
    return fallback;
}

Settings read_settings(const std::wstring& ini) {
    Settings s;
    s.settle_seconds = read_int(L"settle_seconds", s.settle_seconds, 0, 600, ini);
    s.survey_slots = read_int(L"survey_slots", s.survey_slots, 1, kMaxSurveySlots, ini);
    s.survey_repeats = read_int(L"survey_repeats", s.survey_repeats, 0, 1000, ini);
    s.survey_interval_ms = read_int(L"survey_interval_ms", s.survey_interval_ms, 50, 60000, ini);
    s.arm_key = parse_key(read_string(L"arm_key", L"F9", ini), s.arm_key);
    s.survey_key = parse_key(read_string(L"survey_key", L"F10", ini), s.survey_key);
    s.slots = narrow(read_string(L"slots", L"auto", ini));
    s.max_slots = read_int(L"max_slots", s.max_slots, 1, 4, ini);
    s.stack = narrow(read_string(L"stack", L"scan", ini));
    s.scan_qwords = read_int(L"scan_qwords", s.scan_qwords, 1, 4096, ini);
    s.trust_dr6 = read_int(L"trust_dr6", 0, 0, 1, ini) != 0;
    s.arm_threads = narrow(read_string(L"arm_threads", L"all", ini));
    s.max_threads = read_int(L"max_threads", 0, 0, 100000, ini);
    s.arm_delay_ms = read_int(L"arm_delay_ms", 0, 0, 10000, ini);
    s.flush_seconds = read_int(L"flush_seconds", s.flush_seconds, 1, 3600, ini);
    s.rearm_seconds = read_int(L"rearm_seconds", s.rearm_seconds, 1, 3600, ini);
    s.rearm_all = read_int(L"rearm_all", 0, 0, 1, ini) != 0;
    s.table_rows = read_int(L"table_rows", s.table_rows, 1, 512, ini);
    return s;
}

// Parses "0,1,2" and "4-7". Returns nothing when the text is not a list.
std::optional<std::vector<int>> parse_slot_list(const std::string& text) {
    std::vector<int> slots;
    const char* p = text.data();
    const char* const end = p + text.size();
    const auto skip_space = [&] {
        while (p != end && (*p == ' ' || *p == '\t')) {
            ++p;
        }
    };
    const auto number = [&](int& out) {
        skip_space();
        const auto r = std::from_chars(p, end, out);
        p = r.ptr;
        return r.ec == std::errc{} && out >= 0 && out < kMaxSurveySlots;
    };
    while (p != end) {
        int first = 0;
        int last = 0;
        if (!number(first)) {
            return std::nullopt;
        }
        last = first;
        skip_space();
        if (p != end && *p == '-') {
            ++p;
            if (!number(last) || last < first) {
                return std::nullopt;
            }
        }
        for (int s = first; s <= last; ++s) {
            slots.push_back(s);
        }
        skip_space();
        if (p != end && *p++ != ',') {
            return std::nullopt;
        }
    }
    if (slots.empty()) {
        return std::nullopt;
    }
    return slots;
}

bool readable(std::uintptr_t address, std::size_t length) {
    MEMORY_BASIC_INFORMATION info{};
    if (VirtualQuery(reinterpret_cast<const void*>(address), &info, sizeof(info)) == 0) {
        return false;
    }
    constexpr DWORD kBlocked = PAGE_NOACCESS | PAGE_GUARD;
    if (info.State != MEM_COMMIT || info.Protect == 0 || (info.Protect & kBlocked) != 0) {
        return false;
    }
    return address + length <= reinterpret_cast<std::uintptr_t>(info.BaseAddress) + info.RegionSize;
}

std::optional<float> read_float(std::uintptr_t address) {
    if (!readable(address, sizeof(float))) {
        return std::nullopt;
    }
    float value = 0.0f;
    std::memcpy(&value, reinterpret_cast<const void*>(address), sizeof(value));
    return value;
}

std::string show(const std::optional<float>& value) {
    return value ? std::format("{:.4f}", *value) : "unreadable";
}

bool plausible_fov(const std::optional<float>& value) {
    return value && std::isfinite(*value) && *value >= 1.0f && *value <= 179.0f;
}

struct Survey {
    std::optional<float> shader;
    std::vector<std::optional<float>> slots;
};

class Watcher {
public:
    Watcher(const mem::Region& module, const Settings& settings)
        : module_(module), settings_(settings) {}

    std::uintptr_t slot_address(int slot) const {
        return module_.base + kViewArrayRva + static_cast<std::uintptr_t>(slot) * kViewStride +
               kFovOffset;
    }

    Survey take_survey(int count) const {
        Survey survey;
        survey.shader = read_float(module_.base + kShaderFovRva);
        for (int i = 0; i < count; ++i) {
            survey.slots.push_back(read_float(slot_address(i)));
        }
        return survey;
    }

    void log_survey(const char* label) const {
        const Survey survey = take_survey(settings_.survey_slots);
        std::string line;
        int plausible = 0;
        for (std::size_t i = 0; i < survey.slots.size(); ++i) {
            line += std::format(" {}={}", i, show(survey.slots[i]));
            plausible += plausible_fov(survey.slots[i]) ? 1 : 0;
        }
        logger::info("survey {}: shader fov {}; {} of {} slots in 1..179:{}", label,
                     show(survey.shader), plausible, survey.slots.size(), line);
    }

    void toggle() {
        if (watchpoint::armed()) {
            disarm("key");
        } else {
            arm();
        }
    }

    void tick(std::chrono::steady_clock::time_point now) {
        if (!watchpoint::armed()) {
            return;
        }
        if (now >= next_rearm_) {
            next_rearm_ = now + std::chrono::seconds(settings_.rearm_seconds);
            const watchpoint::Applied r = watchpoint::rearm(settings_.rearm_all);
            if (r.updated != 0 || r.failed != 0) {
                logger::info("rearm{}: {} thread(s) updated, {} failed",
                             settings_.rearm_all ? " (all threads)" : "", r.updated, r.failed);
            }
        }
        if (now >= next_flush_) {
            next_flush_ = now + std::chrono::seconds(settings_.flush_seconds);
            if (watchpoint::total_hits() != flushed_hits_) {
                flush();
            }
        }
    }

    void disarm(const char* reason) {
        if (!watchpoint::armed()) {
            return;
        }
        const watchpoint::Applied r = watchpoint::disarm();
        logger::info("disarmed ({}): {} thread(s) cleared, {} failed", reason, r.updated, r.failed);
        flush();
    }

private:
    std::vector<int> choose_slots() const {
        if (const auto list = parse_slot_list(settings_.slots)) {
            return *list;
        }
        if (settings_.slots != "auto") {
            logger::info("slots \"{}\" is not a list; using auto", settings_.slots);
        }

        const Survey survey = take_survey(settings_.survey_slots);
        std::vector<int> chosen;
        const auto taken = [&chosen](int s) {
            return std::find(chosen.begin(), chosen.end(), s) != chosen.end();
        };
        const auto room = [&chosen, this] {
            return chosen.size() < static_cast<std::size_t>(settings_.max_slots);
        };

        std::vector<float> seen;
        for (int i = 0; i < settings_.survey_slots && room(); ++i) {
            if (plausible_fov(survey.slots[i]) &&
                std::find(seen.begin(), seen.end(), *survey.slots[i]) == seen.end()) {
                seen.push_back(*survey.slots[i]);
                chosen.push_back(i);
            }
        }
        for (int i = 0; i < settings_.survey_slots && room(); ++i) {
            if (plausible_fov(survey.slots[i]) && !taken(i)) {
                chosen.push_back(i);
            }
        }
        for (int i = 0; i < settings_.survey_slots && room(); ++i) {
            if (!taken(i)) {
                chosen.push_back(i);
            }
        }
        std::sort(chosen.begin(), chosen.end());
        return chosen;
    }

    void arm() {
        std::vector<int> slots = choose_slots();
        const auto limit = static_cast<std::size_t>(settings_.max_slots);
        if (slots.size() > limit) {
            logger::info("{} slots requested; only the first {} are watched (max_slots, debug registers)",
                         slots.size(), limit);
            slots.resize(limit);
        }

        std::vector<watchpoint::Target> targets;
        std::string described;
        for (const int slot : slots) {
            const std::uintptr_t address = slot_address(slot);
            if (address < module_.base || address + sizeof(float) > module_.end() ||
                !readable(address, sizeof(float))) {
                logger::info("slot {} at 0x{:X} is outside the module or unreadable; skipped", slot,
                             address);
                continue;
            }
            targets.push_back({address, slot});
            described += std::format(" {}(rva 0x{:X}, now {})", slot, address - module_.base,
                                     show(read_float(address)));
        }
        if (targets.empty()) {
            logger::info("arm: no usable slot");
            return;
        }

        logger::info("arming slots{} on {} thread(s)", described, watchpoint::other_thread_count());
        const watchpoint::Applied r = watchpoint::arm(targets);
        if (!watchpoint::armed()) {
            logger::info("arm: the exception handler could not be installed");
            return;
        }
        const auto now = std::chrono::steady_clock::now();
        next_rearm_ = now + std::chrono::seconds(settings_.rearm_seconds);
        next_flush_ = now + std::chrono::seconds(settings_.flush_seconds);
        flushed_hits_ = 0;
        logger::info("armed on slots{}: {} thread(s) updated, {} failed", described, r.updated,
                     r.failed);
    }

    void flush() {
        flushed_hits_ = watchpoint::total_hits();
        const Survey survey = take_survey(0);
        logger::info("hit table (shader fov now {})", show(survey.shader));
        watchpoint::flush(module_.base, module_.size, static_cast<std::size_t>(settings_.table_rows));
    }

    mem::Region module_;
    Settings settings_;
    std::chrono::steady_clock::time_point next_rearm_{};
    std::chrono::steady_clock::time_point next_flush_{};
    unsigned long long flushed_hits_ = 0;
};

bool pressed(int key, bool& was_down) {
    const bool down = (GetAsyncKeyState(key) & 0x8000) != 0;
    const bool edge = down && !was_down;
    was_down = down;
    return edge;
}

void log_settings(const Settings& s) {
    logger::info("settings: settle_seconds={} survey_slots={} survey_repeats={} "
                 "survey_interval_ms={} arm_key=0x{:X} survey_key=0x{:X} slots={} max_slots={} stack={} scan_qwords={} "
                 "trust_dr6={} arm_threads={} max_threads={} arm_delay_ms={} flush_seconds={} rearm_seconds={} rearm_all={} table_rows={}",
                 s.settle_seconds, s.survey_slots, s.survey_repeats, s.survey_interval_ms, s.arm_key,
                 s.survey_key, s.slots, s.max_slots, s.stack, s.scan_qwords, s.trust_dr6 ? 1 : 0, s.arm_threads,
                 s.max_threads, s.arm_delay_ms, s.flush_seconds, s.rearm_seconds, s.rearm_all ? 1 : 0,
                 s.table_rows);
}

DWORD WINAPI run(LPVOID parameter) {
    const auto self = static_cast<HMODULE>(parameter);
    try {
        logger::open(self);

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
            logger::info("WARNING: offsets are for {}; slots may not hold view data", kExpectedVersion);
        }
        log_settings(settings);

        const std::filesystem::path trace_path =
            (logger::directory().empty() ? asi.parent_path() : logger::directory()) /
            std::filesystem::path(asi).stem().concat(L".trace");
        trace::set_module(module.base, module.size);
        if (trace::open(trace_path)) {
            trace::install_crash_observers();
            logger::info("trace file {}; loader thread id {}", logger::utf8(trace_path),
                         g_loader_thread_id.load());
        } else {
            logger::info("could not open trace file {}", logger::utf8(trace_path));
        }

        const std::uintptr_t last_slot_end =
            module.base + kViewArrayRva + static_cast<std::uintptr_t>(kMaxSurveySlots) * kViewStride;
        if (last_slot_end > module.end()) {
            logger::info("the view array runs past the module end; reads are guarded per slot");
        }

        std::this_thread::sleep_for(std::chrono::seconds(settings.settle_seconds));

        watchpoint::Config config;
        if (settings.stack == "none") {
            config.stack = watchpoint::StackMode::None;
        } else if (settings.stack == "unwind") {
            config.stack = watchpoint::StackMode::Unwind;
        } else if (settings.stack != "scan") {
            logger::info("stack \"{}\" is not none, scan or unwind; using scan", settings.stack);
        }
        config.trust_dr6 = settings.trust_dr6;
        config.main_thread_only = settings.arm_threads == "main";
        if (!config.main_thread_only && settings.arm_threads != "all") {
            logger::info("arm_threads \"{}\" is not all or main; using all", settings.arm_threads);
        }
        config.main_thread = g_loader_thread_id.load();
        config.max_threads = static_cast<std::size_t>(settings.max_threads);
        config.arm_delay_ms = static_cast<unsigned long>(settings.arm_delay_ms);
        config.scan_qwords = static_cast<std::size_t>(settings.scan_qwords);
        std::vector<watchpoint::Range> executable;
        for (const mem::Section& section : mem::executable_sections(module)) {
            executable.push_back({section.region.base, section.region.end()});
            logger::info("executable section {} rva 0x{:X} size 0x{:X}", section.name,
                         section.region.base - module.base, section.region.size);
        }
        config.executable = executable;
        watchpoint::configure(config);

        Watcher watcher(module, settings);
        for (int i = 1; i <= settings.survey_repeats; ++i) {
            watcher.log_survey(std::format("{}/{}", i, settings.survey_repeats).c_str());
            if (i < settings.survey_repeats) {
                std::this_thread::sleep_for(std::chrono::milliseconds(settings.survey_interval_ms));
            }
        }
        logger::info("ready: key 0x{:X} arms or disarms, key 0x{:X} logs a survey", settings.arm_key,
                     settings.survey_key);

        bool arm_down = false;
        bool survey_down = false;
        for (;;) {
            if (pressed(settings.arm_key, arm_down)) {
                watcher.toggle();
            }
            if (pressed(settings.survey_key, survey_down)) {
                watcher.log_survey("on demand");
            }
            watcher.tick(std::chrono::steady_clock::now());
            std::this_thread::sleep_for(kPollInterval);
        }
    } catch (const std::exception& e) {
        logger::info("failed: {}", e.what());
        return 1;
    }
}

}  // namespace

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(instance);
        g_loader_thread_id.store(GetCurrentThreadId());
        if (HANDLE thread = CreateThread(nullptr, 0, run, instance, 0, nullptr)) {
            CloseHandle(thread);
        }
    }
    return TRUE;
}
