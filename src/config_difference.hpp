#pragma once

#include "model.hpp"

namespace sempervirens {

std::optional<std::vector<SettingDifference>> compare_configuration_files(
    const fs::path& source, const fs::path& target);

} // namespace sempervirens
