// Copyright (c) 2026 Viktor Sharha

#include "common/keys.h"

#include <cwchar>
#include <cwctype>
#include <windows.h>

namespace keys {

int parse(const std::wstring& text, int fallback) {
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

}  // namespace keys
