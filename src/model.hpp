#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace sempervirens {

namespace fs = std::filesystem;
struct NbtTag;

enum class RuleKind { file, directory, glob, builtin };
enum class BuiltinKind { options, servers, screenshots, worlds };
enum class ScanStatus { found, identical, not_found, partial, unreadable, config_error, empty, conflict };
enum class OverwriteStrategy { skip, replace, backup_and_replace };

struct MigrationRule {
    std::string id;
    std::string group;
    std::string name;
    std::string description;
    RuleKind kind = RuleKind::file;
    std::optional<BuiltinKind> builtin;
    std::string category;
    std::string source;
    std::string target;
    bool recursive = true;
    bool default_selected = false;
    std::string risk = "safe";
};

struct MigrationProfile {
    int schema_version = 1;
    std::string name;
    std::string description;
    std::vector<MigrationRule> rules;
    fs::path loaded_from;
};

struct InstanceInfo {
    fs::path root;
    std::wstring display_name;
    bool version_isolated = false;
    bool usable = false;
    int marker_count = 0;
    bool has_options = false;
    bool has_servers = false;
    int screenshot_count = 0;
    int world_count = 0;
    std::wstring access_note;
};

struct InstanceSelection {
    fs::path selected_path;
    std::vector<InstanceInfo> candidates;
};

struct ScanProgress {
    std::optional<int> percent;
    std::wstring message;
};

struct OptionChange {
    std::string key;
    std::string source_value;
    std::string target_value;
};

struct OptionScanInfo {
    int source_count = 0;
    std::vector<OptionChange> changes;
};

struct ServerEntry {
    std::string name;
    std::string address;
    std::string identity;
    int source_index = 0;
    std::shared_ptr<NbtTag> payload;
};

struct ServerComparisonInfo {
    std::string source_name;
    std::string target_name;
    bool other_fields_differ = false;
};

struct WorldEntry {
    std::wstring folder_name;
    std::wstring world_name;
};

enum class FolderDifferenceKind { added, changed, target_only };

struct FolderDifference {
    fs::path relative_path;
    FolderDifferenceKind kind;
};

struct FolderComparisonInfo {
    int source_count = 0;
    int added_count = 0;
    int changed_count = 0;
    int same_count = 0;
    bool counts_complete = true;
    std::vector<FolderDifference> examples;
};

struct SettingDifference {
    std::string key;
    std::optional<std::string> target_value;
    std::optional<std::string> source_value;
};

struct FileComparisonInfo {
    std::uintmax_t source_bytes = 0;
    std::uintmax_t target_bytes = 0;
    std::optional<std::vector<SettingDifference>> settings;
};

using OperationComparison = std::variant<std::monostate, FolderComparisonInfo, ServerComparisonInfo,
                                         FileComparisonInfo>;
using OperationPayload = std::variant<std::monostate, OptionScanInfo, ServerEntry, WorldEntry>;

struct PlannedOperation {
    std::size_t rule_index = 0;
    std::string id;
    std::string group;
    std::string name;
    std::string description;
    fs::path source_path;
    fs::path target_path;
    std::string relative_key;
    ScanStatus status = ScanStatus::not_found;
    std::uintmax_t size = 0;
    std::string detail;
    bool selected = false;
    bool is_directory = false;
    bool is_builtin = false;
    bool recursive = true;
    OperationPayload payload;
    OperationComparison comparison;
};

struct MigrationPlan {
    fs::path source_root;
    fs::path target_root;
    MigrationProfile profile;
    std::vector<PlannedOperation> operations;
    std::string error;

    bool valid() const;
};

} // namespace sempervirens
