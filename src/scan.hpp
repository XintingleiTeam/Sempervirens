#pragma once

#include "instance_discovery.hpp"
#include "model.hpp"

#include <stop_token>

namespace sempervirens {

MigrationPlan build_plan(MigrationProfile profile, const fs::path& source_root,
                         const fs::path& target_root, std::stop_token cancellation = {},
                         const ScanProgressCallback& progress = {}, bool english = false);

#ifdef SEMPERVIRENS_TESTING
bool try_probe_writable_for_test(const fs::path& candidate);
#endif

} // namespace sempervirens
