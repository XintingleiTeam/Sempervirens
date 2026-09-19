#pragma once

#include <string>
#include <string_view>

namespace sempervirens {

std::wstring utf8_to_wide(std::string_view value);
std::string wide_to_utf8(std::wstring_view value);
std::string utf8_with_replacement(std::string_view value);
std::string ascii_lower(std::string_view value);
std::string invariant_lower_utf8(std::string_view value);
bool unicode_blank(std::string_view value);

} // namespace sempervirens
