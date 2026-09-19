#include "path_safety.hpp"
#include "text.hpp"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <stdexcept>

namespace sempervirens {

fs::path full_path(const fs::path& path) {
    return fs::absolute(path).lexically_normal();
}

static bool same_component(const fs::path& left, const fs::path& right) {
    const auto a = left.wstring();
    const auto b = right.wstring();
    return CompareStringOrdinal(a.c_str(), static_cast<int>(a.size()),
                                b.c_str(), static_cast<int>(b.size()), TRUE) == CSTR_EQUAL;
}

bool is_within(const fs::path& root, const fs::path& candidate, bool allow_root) {
    const auto base = full_path(root);
    const auto path = full_path(candidate);
    auto a = base.begin();
    auto b = path.begin();
    for (; a != base.end(); ++a, ++b) {
        if (b == path.end() || !same_component(*a, *b)) return false;
    }
    return allow_root || b != path.end();
}

bool is_safe_relative_pattern(std::string_view value) {
    if (value.empty() || value.front() == '/' || value.front() == '\\') return false;
    if (value.find(':') != value.npos || value.find('\0') != value.npos) return false;
    if (unicode_blank(value)) return false;
    std::size_t start = 0;
    while (start < value.size()) {
        auto end = value.find_first_of("/\\", start);
        if (end == value.npos) end = value.size();
        const auto part = value.substr(start, end - start);
        if (part == "." || part == "..") return false;
        start = end + 1;
    }
    return true;
}

bool is_reparse_point(const fs::path& path) {
    const auto attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES)
        throw std::runtime_error("Cannot inspect path attributes: " + wide_to_utf8(path.wstring()) +
                                 " (Win32 " + std::to_string(GetLastError()) + ")");
    return (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
}

void ensure_no_reparse_points_between(const fs::path& root, const fs::path& candidate) {
    const auto base = full_path(root);
    const auto path = full_path(candidate);
    if (!is_within(base, path)) throw std::runtime_error("Path is outside instance root");
    auto current = base;
    if (is_reparse_point(current)) throw std::runtime_error("Instance root is a junction or symbolic link");
    auto segment = path.begin();
    for (auto base_segment = base.begin(); base_segment != base.end(); ++base_segment) ++segment;
    for (; segment != path.end(); ++segment) {
        if (*segment == L".") continue;
        current /= *segment;
        const auto attributes = GetFileAttributesW(current.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES) {
            const auto error = GetLastError();
            if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) break;
            throw std::runtime_error("Cannot inspect path attributes: " + wide_to_utf8(current.wstring()) +
                                     " (Win32 " + std::to_string(error) + ")");
        }
        if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
            throw std::runtime_error("Path contains a junction or symbolic link");
    }
}

} // namespace sempervirens
