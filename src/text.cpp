#include "text.hpp"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <stdexcept>

namespace sempervirens {

std::wstring utf8_to_wide(std::string_view value) {
    if (value.empty()) return {};
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                                          static_cast<int>(value.size()), nullptr, 0);
    if (count <= 0) throw std::runtime_error("Invalid UTF-8 text");
    std::wstring result(static_cast<std::size_t>(count), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                            static_cast<int>(value.size()), result.data(), count) != count)
        throw std::runtime_error("UTF-8 conversion failed");
    return result;
}

std::string wide_to_utf8(std::wstring_view value) {
    if (value.empty()) return {};
    const int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                                          static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (count <= 0) throw std::runtime_error("Invalid UTF-16 text");
    std::string result(static_cast<std::size_t>(count), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                            static_cast<int>(value.size()), result.data(), count, nullptr, nullptr) != count)
        throw std::runtime_error("UTF-16 conversion failed");
    return result;
}

std::string utf8_with_replacement(std::string_view value) {
    if (value.empty()) return {};
    // Encoding.UTF8 in the original application uses replacement fallback.
    // On supported Windows versions, CP_UTF8 without MB_ERR_INVALID_CHARS
    // replaces malformed input with U+FFFD instead of rejecting the string.
    const int count = MultiByteToWideChar(CP_UTF8, 0, value.data(),
                                          static_cast<int>(value.size()), nullptr, 0);
    if (count <= 0) throw std::runtime_error("UTF-8 replacement conversion failed");
    std::wstring wide(static_cast<std::size_t>(count), L'\0');
    if (MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                            wide.data(), count) != count)
        throw std::runtime_error("UTF-8 replacement conversion failed");
    return wide_to_utf8(wide);
}

std::string ascii_lower(std::string_view value) {
    std::string result(value);
    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char c) {
        return c >= 'A' && c <= 'Z' ? static_cast<char>(c + ('a' - 'A')) : static_cast<char>(c);
    });
    return result;
}

std::string invariant_lower_utf8(std::string_view value) {
    if (value.empty()) return {};
    const auto wide = utf8_to_wide(value);
    const int count = LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE,
                                    wide.data(), static_cast<int>(wide.size()), nullptr, 0,
                                    nullptr, nullptr, 0);
    if (count <= 0) throw std::runtime_error("Unicode lowercase conversion failed");
    std::wstring lowered(static_cast<std::size_t>(count), L'\0');
    if (LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE,
                      wide.data(), static_cast<int>(wide.size()), lowered.data(), count,
                      nullptr, nullptr, 0) != count)
        throw std::runtime_error("Unicode lowercase conversion failed");
    return wide_to_utf8(lowered);
}

bool unicode_blank(std::string_view value) {
    const auto wide = utf8_to_wide(value);
    return std::all_of(wide.begin(), wide.end(), [](wchar_t character) {
        return (character >= L'\t' && character <= L'\r') || character == L' ' ||
               character == 0x0085 || character == 0x00A0 || character == 0x1680 ||
               (character >= 0x2000 && character <= 0x200A) ||
               character == 0x2028 || character == 0x2029 || character == 0x202F ||
               character == 0x205F || character == 0x3000;
    });
}

} // namespace sempervirens
