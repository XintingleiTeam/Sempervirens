#pragma once

#include "model.hpp"

#include <string_view>

namespace sempervirens {

fs::path full_path(const fs::path& path);
bool is_within(const fs::path& root, const fs::path& candidate, bool allow_root = true);
bool is_safe_relative_pattern(std::string_view value);
void ensure_no_reparse_points_between(const fs::path& root, const fs::path& candidate);
bool is_reparse_point(const fs::path& path);

} // namespace sempervirens
