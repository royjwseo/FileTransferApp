#pragma once
#include <cstddef>
#include <string>
#include <string_view>

namespace FileName {
    constexpr size_t MAX_UTF8_BYTES = 400;
    constexpr size_t MAX_UTF16_UNITS = 100;
    std::wstring ToWide(std::string_view utf8);
    std::string ToUtf8(std::wstring_view utf16);
    bool IsSafe(std::string_view utf8);
}
