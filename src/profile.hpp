#pragma once

#include "model.hpp"

#include <string_view>

namespace sempervirens {

enum class BuiltInProfile { xintinglei, minecraft };

std::string normalize_relaxed_json(std::string_view input);

MigrationProfile load_profile(const fs::path& path);
MigrationProfile load_builtin_profile(BuiltInProfile profile);
MigrationProfile load_default_profile(const fs::path& application_directory);

} // namespace sempervirens
