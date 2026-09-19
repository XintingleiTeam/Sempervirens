#pragma once

#include "model.hpp"

namespace sempervirens {

struct MigrationExplanation {
    std::wstring introduction;
    std::wstring scope;
    std::wstring difference;
    std::wstring outcome;
    std::wstring technical;
};

MigrationExplanation explain_migration(const PlannedOperation& operation,
                                       const MigrationProfile& profile,
                                       OverwriteStrategy strategy, bool english);
std::wstring display_operation_name(const PlannedOperation& operation, bool english);
std::wstring display_operation_group(const PlannedOperation& operation, bool english);
std::wstring display_profile_name(std::string_view name, bool english);
std::wstring display_binding_name(std::string_view key, bool english);
std::wstring display_binding_value(std::string_view value, bool english);

} // namespace sempervirens
