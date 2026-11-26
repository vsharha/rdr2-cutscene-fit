// Copyright (c) 2026 Viktor Sharha

// Loads a reload build of CutsceneFit (CUTSCENEFIT_RELOAD) and replaces it with a fresh
// copy while the game runs. Each load copies the DLL to a new temporary file first, so
// the build can overwrite the original while a copy is loaded. Before unloading a copy
// it calls cutscenefit_stop, which removes the plugin's hooks and patch and waits until
// no game thread is inside them.
//
// The plugin is given "CutsceneFit.asi" next to this .asi as its own path, so it reads
// CutsceneFit.ini and writes CutsceneFit.log there, as the release build does. The log
// is appended to after the first load.
//
// Settings are read from "CutsceneFitReload.ini" next to CutsceneFitReload.asi,
// section [reload]:
//   payload   path of CutsceneFit.dll. Default: the file this build writes, with a
//             Linux path given on Wine's Z: drive.
//   key       key that reloads the plugin: a key name or a virtual-key code, as for
//             toggle_key in CutsceneFit.ini. Default: F9.
//   watch     1 reloads when the payload's modification time changes and has then
//             stayed the same for half a second, 0 reloads only on the key. Default: 1.

#include <chrono>
#include <cstring>
#include <exception>
#include <filesystem>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>
#include <windows.h>

#include "common/keys.h"
#include "common/log.h"
#include "common/paths.h"

namespace {

using Clock = std::chrono::steady_clock;
using StartFn = int (*)(const wchar_t*, int);
using StopFn = void (*)();

constexpr const wchar_t* kSection = L"reload";
constexpr auto kPollInterval = std::chrono::milliseconds(100);
// How long the payload's modification time must stay unchanged before a reload, so
// that a file the linker is still writing is not copied.
constexpr auto kSettle = std::chrono::milliseconds(500);
constexpr std::wstring_view kCopyPrefix = L"CutsceneFit.";

struct Settings {
    std::filesystem::path payload;
    int key = VK_F9;
    bool watch = true;
};

template <typename Function>
Function export_of(HMODULE module, const char* name) {
    const FARPROC address = GetProcAddress(module, name);
    Function function = nullptr;
    static_assert(sizeof(function) == sizeof(address));
    std::memcpy(&function, &address, sizeof(function));
    return function;
}

std::wstring widen(std::string_view text) {
    if (text.empty()) {
        return {};
    }
    const int size =
        MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), size);
    return result;
}

Settings read_settings(const std::filesystem::path& ini) {
    Settings s;
    std::vector<wchar_t> buffer(1024);
    GetPrivateProfileStringW(kSection, L"payload", L"", buffer.data(), static_cast<DWORD>(buffer.size()),
                             ini.c_str());
    s.payload = buffer.data();
    if (s.payload.empty()) {
        s.payload = widen(CUTSCENEFIT_RELOAD_PAYLOAD);
    }

    GetPrivateProfileStringW(kSection, L"key", L"F9", buffer.data(), static_cast<DWORD>(buffer.size()),
                             ini.c_str());
    const int key = keys::parse(buffer.data(), -1);
    if (key < 0) {
        logger::info("settings: key \"{}\" is not a key name; using F9", logger::utf8(buffer.data()));
    } else {
        s.key = key;
    }
    s.watch = GetPrivateProfileIntW(kSection, L"watch", 1, ini.c_str()) != 0;
    return s;
}

std::optional<std::filesystem::file_time_type> modified(const std::filesystem::path& path) {
    std::error_code ec;
    const auto time = std::filesystem::last_write_time(path, ec);
    if (ec) {
        return std::nullopt;
    }
    return time;
}

// The temporary copies of the payload. Copies left by an earlier game session are
// deleted at start.
class Copies {
public:
    Copies() {
        std::vector<wchar_t> buffer(MAX_PATH + 1);
        const DWORD length = GetTempPathW(static_cast<DWORD>(buffer.size()), buffer.data());
        if (length == 0 || length >= buffer.size()) {
            return;
        }
        directory_ = std::filesystem::path(buffer.data()) / L"CutsceneFitReload";
        std::error_code ec;
        std::filesystem::create_directories(directory_, ec);
        if (ec) {
            directory_.clear();
            return;
        }
        for (const auto& entry : std::filesystem::directory_iterator(directory_, ec)) {
            const std::wstring name = entry.path().filename().wstring();
            if (name.starts_with(kCopyPrefix) && entry.path().extension() == L".dll") {
                std::filesystem::remove(entry.path(), ec);
            }
        }
    }

    [[nodiscard]] bool usable() const { return !directory_.empty(); }
    [[nodiscard]] const std::filesystem::path& directory() const { return directory_; }

    // Copies `payload` to a name not used before in this process. Empty on failure.
    std::filesystem::path make(const std::filesystem::path& payload) {
        const std::filesystem::path copy =
            directory_ / std::format(L"{}{}.{}.dll", kCopyPrefix, GetCurrentProcessId(), ++count_);
        if (!CopyFileW(payload.c_str(), copy.c_str(), FALSE)) {
            logger::info("copying {} failed, error {}", logger::utf8(payload), GetLastError());
            return {};
        }
        return copy;
    }

private:
    std::filesystem::path directory_;
    unsigned count_ = 0;
};

class Plugin {
public:
    Plugin(std::filesystem::path asi, Copies& copies) : asi_(std::move(asi)), copies_(copies) {}

    // Replaces the loaded copy, if any, with a new copy of `payload`. The new copy is
    // made first, so a payload that cannot be read leaves the loaded copy running.
    // Returns false only in that case; a copy that fails to load or start counts as
    // done, so that the same file is not tried again.
    bool reload(const std::filesystem::path& payload) {
        const std::filesystem::path copy = copies_.make(payload);
        if (copy.empty()) {
            return false;
        }
        unload();

        HMODULE module = LoadLibraryW(copy.c_str());
        if (module == nullptr) {
            logger::info("loading {} failed, error {}", logger::utf8(copy), GetLastError());
            remove(copy);
            return true;
        }
        const auto start = export_of<StartFn>(module, "cutscenefit_start");
        const auto stop = export_of<StopFn>(module, "cutscenefit_stop");
        if (start == nullptr || stop == nullptr) {
            logger::info("{} has no cutscenefit_start and cutscenefit_stop; build it with "
                         "-DCUTSCENEFIT_RELOAD=ON",
                         logger::utf8(payload));
            FreeLibrary(module);
            remove(copy);
            return true;
        }
        if (start(asi_.c_str(), loads_ > 0 ? 1 : 0) == 0) {
            logger::info("cutscenefit_start failed");
            FreeLibrary(module);
            remove(copy);
            return true;
        }
        ++loads_;
        module_ = module;
        stop_ = stop;
        copy_ = copy;
        logger::info("load {}: {}", loads_, logger::utf8(copy));
        return true;
    }

private:
    void unload() {
        if (module_ == nullptr) {
            return;
        }
        const auto began = Clock::now();
        stop_();
        FreeLibrary(module_);
        const auto ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - began).count();
        if (GetModuleHandleW(copy_.c_str()) != nullptr) {
            logger::info("stopped in {} ms, but {} is still loaded: something else holds a reference", ms,
                         logger::utf8(copy_));
        } else {
            logger::info("stopped and unloaded in {} ms", ms);
            remove(copy_);
        }
        module_ = nullptr;
        stop_ = nullptr;
        copy_.clear();
    }

    static void remove(const std::filesystem::path& path) {
        std::error_code ec;
        std::filesystem::remove(path, ec);
    }

    std::filesystem::path asi_;
    Copies& copies_;
    HMODULE module_ = nullptr;
    StopFn stop_ = nullptr;
    std::filesystem::path copy_;
    unsigned loads_ = 0;
};

bool pressed(int key, bool& was_down) {
    const bool down = (GetAsyncKeyState(key) & 0x8000) != 0;
    const bool edge = down && !was_down;
    was_down = down;
    return edge;
}

DWORD WINAPI run(LPVOID parameter) {
    const auto self = static_cast<HMODULE>(parameter);
    try {
        const std::filesystem::path asi = paths::module_path(self);
        logger::open(asi);
        const Settings s = read_settings(std::filesystem::path(asi).replace_extension(L".ini"));
        logger::info("payload {}, key 0x{:X}, watch {}", logger::utf8(s.payload), s.key, s.watch);

        Copies copies;
        if (!copies.usable()) {
            logger::info("no temporary directory for the copies; doing nothing");
            return 1;
        }
        logger::info("copies go to {}", logger::utf8(copies.directory()));

        Plugin plugin(asi.parent_path() / L"CutsceneFit.asi", copies);
        // The modification time of the payload last copied, unset until a copy succeeds.
        std::optional<std::filesystem::file_time_type> loaded;
        if (const auto current = modified(s.payload)) {
            if (plugin.reload(s.payload)) {
                loaded = current;
            }
        } else {
            logger::info("{} does not exist yet; waiting for it", logger::utf8(s.payload));
        }

        // A modification time that differs from the loaded one, and when it was first seen.
        std::optional<std::filesystem::file_time_type> pending;
        Clock::time_point pending_since{};
        bool key_down = false;
        for (;;) {
            std::this_thread::sleep_for(kPollInterval);
            const auto now = Clock::now();
            if (pressed(s.key, key_down)) {
                logger::info("reload key");
                const auto current = modified(s.payload);
                if (plugin.reload(s.payload)) {
                    loaded = current;
                }
                pending.reset();
                continue;
            }
            if (!s.watch) {
                continue;
            }
            const auto current = modified(s.payload);
            if (!current || current == loaded) {
                pending.reset();
                continue;
            }
            if (current != pending) {
                pending = current;
                pending_since = now;
                continue;
            }
            if (now - pending_since >= kSettle) {
                logger::info("{} changed", logger::utf8(s.payload));
                if (plugin.reload(s.payload)) {
                    loaded = current;
                }
                pending.reset();
            }
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
        if (HANDLE thread = CreateThread(nullptr, 0, run, instance, 0, nullptr)) {
            CloseHandle(thread);
        }
    }
    return TRUE;
}
