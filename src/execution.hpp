#pragma once

#include "instance_discovery.hpp"
#include "model.hpp"

#include <stop_token>
#ifdef SEMPERVIRENS_TESTING
#include <cstdint>
#include <functional>
#endif

namespace sempervirens {

struct MigrationItemResult {
    std::string name;
    std::string group;
    std::string status;
    std::string message;
    fs::path source_path;
    fs::path target_path;
    int files_copied = 0;
    int files_overwritten = 0;
    int files_backed_up = 0;
};

struct MigrationResult {
    std::string profile_name;
    fs::path source_root;
    fs::path target_root;
    std::optional<fs::path> backup_root;
    fs::path log_path;
    int success_count = 0;
    int skipped_count = 0;
    int failed_count = 0;
    int files_copied = 0;
    int files_overwritten = 0;
    int files_backed_up = 0;
    int files_failed = 0;
    std::vector<MigrationItemResult> items;
};

MigrationResult execute_migration(const MigrationPlan& plan, OverwriteStrategy strategy,
                                  std::stop_token cancellation = {},
                                  const ScanProgressCallback& progress = {}, bool english = false);

#ifdef SEMPERVIRENS_TESTING
void set_copy_progress_observer_for_test(std::function<void(std::uint64_t, std::uint64_t)> observer);
#endif

} // namespace sempervirens
