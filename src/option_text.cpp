#include "option_text.hpp"

#include "text.hpp"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <fstream>
#include <climits>
#include <cstdint>
#include <iterator>
#include <stdexcept>
#include <string_view>

namespace sempervirens {

namespace {

std::wstring decode_utf8_replacing_invalid(std::string_view bytes) {
    if (bytes.empty()) return {};
    if (bytes.size() > static_cast<std::size_t>(INT_MAX))
        throw std::runtime_error("options.txt line is too long");
    const auto count = MultiByteToWideChar(CP_UTF8, 0, bytes.data(),
                                           static_cast<int>(bytes.size()), nullptr, 0);
    if (count <= 0) throw std::runtime_error("Cannot decode options.txt");
    std::wstring decoded(static_cast<std::size_t>(count), L'\0');
    if (MultiByteToWideChar(CP_UTF8, 0, bytes.data(), static_cast<int>(bytes.size()),
                            decoded.data(), count) != count)
        throw std::runtime_error("Cannot decode options.txt");
    return decoded;
}

std::wstring decode_utf16(std::string_view bytes, bool little_endian) {
    std::wstring decoded;
    decoded.reserve((bytes.size() + 1) / 2);
    const auto byte = [&](std::size_t index) {
        return static_cast<std::uint8_t>(bytes[index]);
    };
    for (std::size_t index = 0; index + 1 < bytes.size(); index += 2) {
        const auto unit = little_endian ?
            static_cast<std::uint16_t>(byte(index) | (byte(index + 1) << 8)) :
            static_cast<std::uint16_t>((byte(index) << 8) | byte(index + 1));
        if (unit >= 0xD800 && unit <= 0xDBFF) {
            if (index + 3 < bytes.size()) {
                const auto next = little_endian ?
                    static_cast<std::uint16_t>(byte(index + 2) | (byte(index + 3) << 8)) :
                    static_cast<std::uint16_t>((byte(index + 2) << 8) | byte(index + 3));
                if (next >= 0xDC00 && next <= 0xDFFF) {
                    decoded.push_back(static_cast<wchar_t>(unit));
                    decoded.push_back(static_cast<wchar_t>(next));
                    index += 2;
                    continue;
                }
            }
            decoded.push_back(static_cast<wchar_t>(0xFFFD));
        } else if (unit >= 0xDC00 && unit <= 0xDFFF)
            decoded.push_back(static_cast<wchar_t>(0xFFFD));
        else decoded.push_back(static_cast<wchar_t>(unit));
    }
    if (bytes.size() % 2 != 0) decoded.push_back(static_cast<wchar_t>(0xFFFD));
    return decoded;
}

void append_scalar(std::wstring& decoded, std::uint32_t scalar) {
    if (scalar > 0x10FFFF || (scalar >= 0xD800 && scalar <= 0xDFFF)) scalar = 0xFFFD;
    if (scalar <= 0xFFFF) decoded.push_back(static_cast<wchar_t>(scalar));
    else {
        scalar -= 0x10000;
        decoded.push_back(static_cast<wchar_t>(0xD800 + (scalar >> 10)));
        decoded.push_back(static_cast<wchar_t>(0xDC00 + (scalar & 0x3FF)));
    }
}

std::wstring decode_utf32(std::string_view bytes, bool little_endian) {
    std::wstring decoded;
    decoded.reserve((bytes.size() + 3) / 4);
    const auto byte = [&](std::size_t index) {
        return static_cast<std::uint8_t>(bytes[index]);
    };
    for (std::size_t index = 0; index + 3 < bytes.size(); index += 4) {
        const auto scalar = little_endian ?
            (std::uint32_t(byte(index)) | std::uint32_t(byte(index + 1)) << 8 |
             std::uint32_t(byte(index + 2)) << 16 | std::uint32_t(byte(index + 3)) << 24) :
            (std::uint32_t(byte(index)) << 24 | std::uint32_t(byte(index + 1)) << 16 |
             std::uint32_t(byte(index + 2)) << 8 | std::uint32_t(byte(index + 3)));
        append_scalar(decoded, scalar);
    }
    if (bytes.size() % 4 != 0) decoded.push_back(static_cast<wchar_t>(0xFFFD));
    return decoded;
}

std::wstring decode_options_text(std::string_view bytes) {
    if (bytes.starts_with("\xEF\xBB\xBF")) return decode_utf8_replacing_invalid(bytes.substr(3));
    if (bytes.starts_with(std::string_view("\xFF\xFE\x00\x00", 4)))
        return decode_utf32(bytes.substr(4), true);
    if (bytes.starts_with(std::string_view("\x00\x00\xFE\xFF", 4)))
        return decode_utf32(bytes.substr(4), false);
    if (bytes.starts_with("\xFF\xFE")) return decode_utf16(bytes.substr(2), true);
    if (bytes.starts_with("\xFE\xFF")) return decode_utf16(bytes.substr(2), false);
    return decode_utf8_replacing_invalid(bytes);
}

} // namespace

std::string decode_text_with_bom_to_utf8(std::string_view bytes) {
    return wide_to_utf8(decode_options_text(bytes));
}

std::vector<std::string> read_option_lines(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("Cannot read options.txt");
    const std::string content(std::istreambuf_iterator<char>{input}, {});
    if (input.bad()) throw std::runtime_error("Cannot read options.txt");
    const auto decoded = decode_options_text(content);
    std::vector<std::string> lines;
    std::size_t start = 0;
    for (std::size_t index = 0; index < decoded.size(); ++index) {
        if (decoded[index] != L'\r' && decoded[index] != L'\n') continue;
        lines.push_back(wide_to_utf8(std::wstring_view(decoded).substr(start, index - start)));
        if (decoded[index] == L'\r' && index + 1 < decoded.size() && decoded[index + 1] == L'\n') ++index;
        start = index + 1;
    }
    if (start < decoded.size()) lines.push_back(wide_to_utf8(std::wstring_view(decoded).substr(start)));
    return lines;
}

} // namespace sempervirens
