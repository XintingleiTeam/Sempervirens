#pragma once

#include "model.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace sempervirens {

std::string decode_text_with_bom_to_utf8(std::string_view bytes);
std::vector<std::string> read_option_lines(const fs::path& path);

} // namespace sempervirens
