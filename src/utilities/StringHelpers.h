#pragma once

#include <string>

inline std::wstring TrimTrailingChars(const std::wstring& str) {
    std::wstring sanitized = str;
    // Remove trailing carriage returns, newlines, or trailing spaces
    while (!sanitized.empty() && (sanitized.back() == L'\r' || sanitized.back() == L'\n' || sanitized.back() == L' ')) {
        sanitized.pop_back();
    }
    return sanitized;
}