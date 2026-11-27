// Copyright (c) 2026 Viktor Sharha

#include "settings.h"

#include <cctype>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <windows.h>

#include "common/keys.h"
#include "common/log.h"

namespace settings {
namespace {

constexpr const wchar_t* kSection = L"CutsceneFit";

std::wstring read_string(const wchar_t* key, const wchar_t* fallback, const std::wstring& path) {
    std::vector<wchar_t> buffer(256);
    GetPrivateProfileStringW(kSection, key, fallback, buffer.data(), static_cast<DWORD>(buffer.size()),
                             path.c_str());
    return buffer.data();
}

bool read_flag(const wchar_t* key, bool fallback, const std::wstring& path) {
    return GetPrivateProfileIntW(kSection, key, fallback ? 1 : 0, path.c_str()) != 0;
}

std::string narrow(const std::wstring& text) {
    return logger::utf8(std::filesystem::path(text));
}

}  // namespace

Settings read(const std::wstring& path) {
    Settings s;
    s.toggle_key = VK_END;
#ifdef CUTSCENEFIT_COMPARE
    s.compare_key = VK_F11;
#endif

    s.enabled = read_flag(L"enabled", s.enabled, path);
    s.bars = read_flag(L"bars", s.bars, path);
    s.log = read_flag(L"log", s.log, path);
#ifdef CUTSCENEFIT_COMPARE
    const std::wstring compare_key = read_string(L"compare_key", L"F11", path);
    s.compare_key = keys::parse(compare_key, -1);
    if (s.compare_key < 0) {
        s.compare_key = VK_F11;
        s.warnings.push_back(std::format("compare_key \"{}\" is not a key name; using F11", narrow(compare_key)));
    }
    const int hold = GetPrivateProfileIntW(kSection, L"compare_hold_ms", 1500, path.c_str());
    if (hold >= 0) s.compare_hold_ms = hold;
    s.compare_freeze = read_flag(L"compare_freeze", true, path);
#endif

    const std::wstring key = read_string(L"toggle_key", L"End", path);
    const int parsed = keys::parse(key, -1);
    if (parsed < 0) {
        s.warnings.push_back(std::format("toggle_key \"{}\" is not a key name; using End", narrow(key)));
    } else {
        s.toggle_key = parsed;
    }

    const std::wstring start = read_string(L"zoom_start", L"cut", path);
    std::string start_name = narrow(start);
    for (char& c : start_name) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    if (start_name == "cut") {
        s.zoom_start = zoom::Start::Cut;
    } else if (start_name == "letterbox") {
        s.zoom_start = zoom::Start::Letterbox;
    } else {
        s.warnings.push_back(std::format("zoom_start \"{}\" is not cut or letterbox; using cut", start_name));
    }

    std::string frame_name = narrow(read_string(L"menu_frame", L"cinematic", path));
    for (char& c : frame_name) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    if (frame_name == "cinematic") {
        s.menu_frame = bars::MenuFrame::Cinematic;
    } else if (frame_name == "original") {
        s.menu_frame = bars::MenuFrame::Original;
    } else if (frame_name == "off") {
        s.menu_frame = bars::MenuFrame::Off;
    } else {
        s.warnings.push_back(
            std::format("menu_frame \"{}\" is not cinematic, original or off; using cinematic", frame_name));
    }

    const std::wstring aspect = read_string(L"aspect", L"0", path);
    wchar_t* end = nullptr;
    const double value = std::wcstod(aspect.c_str(), &end);
    if (end != aspect.c_str() && std::isfinite(value) && value >= 0.0) {
        s.aspect = value;
    } else {
        s.warnings.push_back(std::format("aspect \"{}\" is not a number of at least 0; using 0 (auto)",
                                         narrow(aspect)));
    }
    return s;
}

}  // namespace settings
