#include "execution.hpp"

#include "atomic_file.hpp"
#include "nbt.hpp"
#include "option_text.hpp"
#include "path_safety.hpp"
#include "text.hpp"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <unordered_map>
#include <unordered_set>

namespace sempervirens {

namespace {
struct CopySummary {
    int copied = 0;
    int overwritten = 0;
    int backed_up = 0;
    int skipped = 0;
    int failed = 0;
    bool is_skipped = false;
    std::string message;
};

void check_cancelled(std::stop_token token) {
    if (token.stop_requested()) throw std::runtime_error("Migration cancelled");
}

std::string same_settings_message(bool english) {
    return english ? "The destination already has matching data" : "目标里已经有相同内容";
}

std::string lower_path(const fs::path& path) {
    return invariant_lower_utf8(wide_to_utf8(full_path(path).wstring()));
}

bool safe_file(const fs::path& path) {
    std::error_code error;
    if (!fs::is_regular_file(path, error) || error) return false;
    return !is_reparse_point(path);
}

bool safe_directory(const fs::path& path) {
    std::error_code error;
    if (!fs::is_directory(path, error) || error) return false;
    return !is_reparse_point(path);
}

std::vector<fs::path> enumerate_files(const fs::path& root, bool recursive, std::stop_token token) {
    std::vector<fs::path> result;
    if (safe_file(root)) { result.push_back(root); return result; }
    if (!safe_directory(root)) return result;
    std::vector<fs::path> pending{root};
    while (!pending.empty()) {
        check_cancelled(token);
        const auto current = pending.back();
        pending.pop_back();
        std::error_code error;
        for (fs::directory_iterator it(current, fs::directory_options::skip_permission_denied, error), end;
             it != end && !error; it.increment(error)) {
            check_cancelled(token);
            if (safe_file(it->path())) result.push_back(it->path());
            else if (recursive && safe_directory(it->path())) pending.push_back(it->path());
            error.clear();
        }
    }
    return result;
}

bool files_equal(const fs::path& left, const fs::path& right, std::stop_token token) {
    if (!safe_file(left) || !safe_file(right)) return false;
    std::error_code error;
    if (fs::file_size(left, error) != fs::file_size(right, error) || error) return false;
    std::ifstream a(left, std::ios::binary), b(right, std::ios::binary);
    if (!a || !b) return false;
    std::array<char, 64 * 1024> x{}, y{};
    do {
        check_cancelled(token);
        a.read(x.data(), x.size()); b.read(y.data(), y.size());
        if (a.gcount() != b.gcount() || !std::equal(x.begin(), x.begin() + a.gcount(), y.begin())) return false;
    } while (a.gcount() != 0);
    return a.eof() && b.eof();
}

std::string trim(std::string_view value) {
    const auto start = value.find_first_not_of(" \t\r\n");
    if (start == value.npos) return {};
    return std::string(value.substr(start, value.find_last_not_of(" \t\r\n") - start + 1));
}

bool option_key(std::string_view line, std::string& key, std::string& value) {
    const auto colon = line.find(':');
    if (colon == line.npos) return false;
    key = trim(line.substr(0, colon));
    value = trim(line.substr(colon + 1));
    return !key.empty() && key.front() != '#';
}

bool option_category(std::string_view key, std::string_view category) {
    const auto lower = ascii_lower(key);
    const auto group = ascii_lower(category);
    if (group == "bindings") return lower.starts_with("key_");
    if (group == "audio") return lower.starts_with("soundcategory_") || lower == "music" || lower == "sound";
    if (group != "video") return false;
    static const std::unordered_set<std::string> video = {
        "fov", "gamma", "renderdistance", "simulationdistance", "entitydistancescaling", "particles",
        "guiscale", "fullscreen", "fullscreenresolution", "viewbobbing", "attackindicator", "narrator",
        "language", "rawmouseinput", "mousesensitivity", "invertymouse", "biomeblendradius",
        "chatvisibility", "chatopacity", "chatscale", "chatwidth", "chatheightfocused",
        "chatheightunfocused", "telemetryoptin"
    };
    return video.contains(lower);
}

void ensure_target(const fs::path& root, const fs::path& path) {
    if (!is_within(root, path)) throw std::runtime_error("Migration target escapes destination instance");
    ensure_no_reparse_points_between(root, path);
}

#ifdef SEMPERVIRENS_TESTING
std::function<void(std::uint64_t, std::uint64_t)> copy_progress_observer_for_test;
#endif

DWORD CALLBACK copy_progress(LARGE_INTEGER total, LARGE_INTEGER transferred, LARGE_INTEGER, LARGE_INTEGER,
                             DWORD, DWORD, HANDLE, HANDLE, LPVOID context) {
#ifdef SEMPERVIRENS_TESTING
    if (copy_progress_observer_for_test)
        copy_progress_observer_for_test(static_cast<std::uint64_t>(transferred.QuadPart),
                                        static_cast<std::uint64_t>(total.QuadPart));
#endif
    const auto* token = static_cast<const std::stop_token*>(context);
    return token->stop_requested() ? PROGRESS_CANCEL : PROGRESS_CONTINUE;
}

void remove_partial_copy(const fs::path& path) {
    const auto attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) return;
    if (attributes & FILE_ATTRIBUTE_READONLY)
        SetFileAttributesW(path.c_str(), attributes & ~FILE_ATTRIBUTE_READONLY);
    DeleteFileW(path.c_str());
}

void copy_file_cancelable(const fs::path& source, const fs::path& destination,
                          const fs::path& target_root, std::stop_token token) {
    check_cancelled(token);
    ensure_target(target_root, destination);
    static std::atomic_uint64_t next_copy_id{0};
    for (int attempt = 0; attempt < 32; ++attempt) {
        const auto temporary = destination.parent_path() /
            (L".sempervirens-copy-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
             std::to_wstring(GetTickCount64()) + L"-" +
             std::to_wstring(next_copy_id.fetch_add(1, std::memory_order_relaxed)) + L".tmp");
        ensure_target(target_root, temporary);
        if (!CopyFileExW(source.c_str(), temporary.c_str(), copy_progress, &token, nullptr,
                         COPY_FILE_FAIL_IF_EXISTS)) {
            const auto error = GetLastError();
            if (error == ERROR_FILE_EXISTS || error == ERROR_ALREADY_EXISTS) continue;
            remove_partial_copy(temporary);
            check_cancelled(token);
            throw std::system_error(static_cast<int>(error), std::system_category(), "Cannot copy file");
        }
        try {
            check_cancelled(token);
            ensure_target(target_root, destination);
            if (!MoveFileExW(temporary.c_str(), destination.c_str(),
                             MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
                throw std::system_error(static_cast<int>(GetLastError()), std::system_category(),
                                        "Cannot replace destination file");
        } catch (...) {
            remove_partial_copy(temporary);
            throw;
        }
        return;
    }
    throw std::runtime_error("Could not reserve a temporary copy path");
}

bool backup_file(const fs::path& path, const fs::path& target_root, const fs::path& backup_root,
                 std::unordered_set<std::string>& backed_up, OverwriteStrategy strategy,
                 std::stop_token token) {
    if (strategy != OverwriteStrategy::backup_and_replace || !safe_file(path)) return false;
    const auto key = lower_path(path);
    if (backed_up.contains(key)) return false;
    ensure_target(target_root, path);
    const auto backup = backup_root / path.lexically_relative(target_root);
    ensure_target(target_root, backup);
    fs::create_directories(backup.parent_path());
    ensure_target(target_root, backup);
    copy_file_cancelable(path, backup, target_root, token);
    backed_up.insert(key);
    return true;
}

CopySummary copy_operation(const PlannedOperation& operation, const fs::path& target_root,
                           OverwriteStrategy strategy, const fs::path& backup_root,
                           std::unordered_set<std::string>& backed_up, std::stop_token token,
                           bool english) {
    CopySummary summary;
    int unchanged = 0;
    const auto source_is_file = safe_file(operation.source_path);
    const auto files = enumerate_files(operation.source_path, operation.recursive, token);
    if (!source_is_file && !safe_directory(operation.source_path))
        throw std::runtime_error("Source item is no longer readable");
    for (const auto& file : files) {
        check_cancelled(token);
        const auto target = source_is_file ? operation.target_path :
            operation.target_path / file.lexically_relative(operation.source_path);
        try {
            ensure_no_reparse_points_between(target_root, target);
            if (!safe_file(file)) throw std::runtime_error("Source file is no longer readable");
            std::error_code error;
            const auto exists = fs::exists(target, error);
            if (error) throw std::runtime_error("Cannot inspect destination file");
            if (exists && !safe_file(target)) throw std::runtime_error("Destination path is occupied by a folder or link");
            if (exists && files_equal(file, target, token)) { ++unchanged; continue; }
            if (exists && strategy == OverwriteStrategy::skip) { ++summary.skipped; continue; }
            if (exists && backup_file(target, target_root, backup_root, backed_up, strategy, token)) ++summary.backed_up;
            fs::create_directories(target.parent_path());
            ensure_target(target_root, target);
            copy_file_cancelable(file, target, target_root, token);
            ++summary.copied;
            if (exists) ++summary.overwritten;
        } catch (const std::exception&) { check_cancelled(token); ++summary.failed; }
    }
    summary.is_skipped = summary.copied == 0 && summary.skipped > 0;
    summary.message = summary.failed ?
        "Copied " + std::to_string(summary.copied) + " file(s); " +
            std::to_string(summary.failed) + " file(s) failed" :
        summary.copied == 0 && unchanged > 0 ? same_settings_message(english) :
        summary.is_skipped ? "Destination files already exist, so this item was skipped" :
        "Processed " + std::to_string(summary.copied) + " file(s)";
    return summary;
}

void write_text_atomically(const fs::path& target, const fs::path& target_root,
                           const std::string& content, std::stop_token token) {
    fs::create_directories(target.parent_path());
    ensure_target(target_root, target);
    write_file_atomically(target, content, token);
}

CopySummary execute_options(const MigrationPlan& plan, const std::vector<const PlannedOperation*>& operations,
                            const fs::path& target_root, OverwriteStrategy strategy,
                            const fs::path& backup_root, std::unordered_set<std::string>& backed_up,
                            std::stop_token token, bool english) {
    const auto source = operations.front()->source_path;
    const auto target = operations.front()->target_path;
    ensure_target(target_root, target);
    if (!safe_file(source)) throw std::runtime_error("Source options.txt is no longer readable");
    std::unordered_set<std::string> categories;
    for (const auto* operation : operations)
        categories.insert(ascii_lower(plan.profile.rules.at(operation->rule_index).category.empty() ?
                                      "bindings" : plan.profile.rules.at(operation->rule_index).category));
    const auto source_lines = read_option_lines(source);
    std::error_code error;
    const auto target_exists = fs::exists(target, error);
    if (error || (target_exists && !safe_file(target)))
        throw std::runtime_error("Destination options.txt is not a regular file");
    auto target_lines = target_exists ? read_option_lines(target) : std::vector<std::string>{};
    std::unordered_map<std::string, std::size_t> target_map;
    for (std::size_t i = 0; i < target_lines.size(); ++i) {
        std::string key, value;
        if (option_key(target_lines[i], key, value) &&
            !target_map.emplace(invariant_lower_utf8(key), i).second)
            throw std::runtime_error("Duplicate setting key in destination options.txt");
    }
    auto selected_category = [&](std::string_view key) {
        for (const auto& category : categories) if (option_category(key, category)) return true;
        return false;
    };
    bool changed = false;
    for (const auto& line : source_lines) {
        std::string key, value;
        if (!option_key(line, key, value) || !selected_category(key)) continue;
        const auto existing = target_map.find(invariant_lower_utf8(key));
        std::string old_key, old_value;
        if (existing == target_map.end() || !option_key(target_lines[existing->second], old_key, old_value) ||
            old_value != value) { changed = true; break; }
    }
    if (!changed) return {0, 0, 0, 0, 0, false, same_settings_message(english)};
    if (target_exists && strategy == OverwriteStrategy::skip)
        return {0, 0, 0, 1, 0, true,
                "The destination already has personal settings, so this item was skipped"};
    CopySummary summary;
    if (target_exists && backup_file(target, target_root, backup_root, backed_up, strategy, token)) ++summary.backed_up;
    for (const auto& line : source_lines) {
        check_cancelled(token);
        std::string key, value;
        if (!option_key(line, key, value) || !selected_category(key)) continue;
        const auto lower = invariant_lower_utf8(key);
        const auto existing = target_map.find(lower);
        if (existing == target_map.end()) {
            target_map[lower] = target_lines.size();
            target_lines.push_back(line);
        } else target_lines[existing->second] = line;
    }
    std::string output;
    for (const auto& line : target_lines) { output += line; output += "\r\n"; }
    write_text_atomically(target, target_root, output, token);
    summary.copied = 1;
    summary.overwritten = target_exists ? 1 : 0;
    summary.message = "Personal settings migrated";
    return summary;
}

std::string server_identity(const NbtTag& tag) {
    const auto address = tag.get_string("ip");
    const auto name = tag.get_string("name");
    return invariant_lower_utf8(unicode_blank(address) ? name : address);
}

NbtTag& server_list(NbtTag& root) {
    auto* list = root.get("servers");
    if (list && list->type == NbtType::list && list->item_type == NbtType::compound) return *list;
    NbtTag replacement;
    replacement.type = NbtType::list;
    replacement.item_type = NbtType::compound;
    root.set("servers", std::move(replacement));
    return *root.get("servers");
}

std::unordered_map<std::string, NbtTag> server_map(const NbtTag& root) {
    std::unordered_map<std::string, NbtTag> result;
    const auto* list = root.get("servers");
    if (!list || list->type != NbtType::list || list->item_type != NbtType::compound) return result;
    for (const auto& entry : list->items) if (entry.type == NbtType::compound)
        if (!result.emplace(server_identity(entry), entry).second)
            throw std::runtime_error("Duplicate server identity in server list");
    return result;
}

CopySummary execute_servers(const std::vector<const PlannedOperation*>& operations,
                            const fs::path& target_root, OverwriteStrategy strategy,
                            const fs::path& backup_root, std::unordered_set<std::string>& backed_up,
                            std::stop_token token, bool english) {
    const auto source = operations.front()->source_path;
    const auto target = operations.front()->target_path;
    ensure_target(target_root, target);
    if (!safe_file(source)) throw std::runtime_error("Source servers.dat is no longer readable");
    const auto source_root = read_nbt_compound(source);
    const auto source_entries = server_map(source_root);
    std::error_code error;
    const auto target_exists = fs::exists(target, error);
    if (error || (target_exists && !safe_file(target)))
        throw std::runtime_error("Destination servers.dat is not a regular file");
    NbtTag target_document = target_exists ? read_nbt_compound(target) : source_root;
    const auto existing_entries = target_exists ? server_map(target_document) :
        std::unordered_map<std::string, NbtTag>{};
    auto& list = server_list(target_document);
    if (!target_exists) list.items.clear();
    std::vector<std::string> selected_ids;
    std::unordered_set<std::string> selected_lookup;
    for (const auto* operation : operations) {
        if (const auto* entry = std::get_if<ServerEntry>(&operation->payload))
            if (selected_lookup.insert(entry->identity).second)
                selected_ids.push_back(entry->identity);
    }
    if (selected_ids.empty()) return {0, 0, 0, 0, 0, true, "No server was selected"};
    bool all_same = true;
    int conflicts = 0;
    bool changed_conflict = false;
    for (const auto& id : selected_ids) {
        const auto old = existing_entries.find(id);
        const auto incoming = source_entries.find(id);
        if (incoming == source_entries.end()) throw std::runtime_error("A selected source server is missing");
        if (old != existing_entries.end()) {
            ++conflicts;
            if (!nbt_equal(old->second, incoming->second)) changed_conflict = true;
        } else all_same = false;
        if (old != existing_entries.end() && !nbt_equal(old->second, incoming->second)) all_same = false;
    }
    if (all_same) return {0, 0, 0, 0, 0, false, same_settings_message(english)};
    if (strategy == OverwriteStrategy::skip && conflicts == static_cast<int>(selected_ids.size()))
        return {0, 0, 0, conflicts, 0, true,
                "Every selected server already exists, so this item was skipped"};
    CopySummary summary;
    if (target_exists && changed_conflict &&
        backup_file(target, target_root, backup_root, backed_up, strategy, token)) ++summary.backed_up;
    if (strategy != OverwriteStrategy::skip)
        std::erase_if(list.items, [&](const NbtTag& item) {
            return item.type == NbtType::compound && selected_lookup.contains(server_identity(item));
        });
    for (const auto& id : selected_ids) {
        check_cancelled(token);
        const auto existing = existing_entries.find(id);
        if (strategy == OverwriteStrategy::skip && existing != existing_entries.end()) continue;
        auto copy = source_entries.at(id);
        if (existing != existing_entries.end()) {
            const auto existing_name = existing->second.get_string("name");
            if (copy.get_string("name") != existing_name) {
                NbtTag name;
                name.type = NbtType::string;
                name.text = existing_name;
                copy.set("name", std::move(name));
            }
        }
        list.items.push_back(std::move(copy));
    }
    fs::create_directories(target.parent_path());
    ensure_target(target_root, target);
    write_nbt_compound(target, target_document, token);
    summary.copied = 1;
    summary.overwritten = conflicts;
    summary.message = "Processed " + std::to_string(selected_ids.size()) + " server(s)";
    return summary;
}

std::wstring timestamp() {
    SYSTEMTIME now{};
    GetLocalTime(&now);
    wchar_t value[32]{};
    swprintf_s(value, L"%04u-%02u-%02u_%02u-%02u-%02u", now.wYear, now.wMonth, now.wDay,
               now.wHour, now.wMinute, now.wSecond);
    return value;
}

void write_report(const MigrationResult& result, OverwriteStrategy strategy,
                  std::stop_token token) {
    ensure_target(result.target_root, result.log_path);
    fs::create_directories(result.log_path.parent_path());
    ensure_target(result.target_root, result.log_path);
    std::ostringstream output;
    SYSTEMTIME created{};
    GetLocalTime(&created);
    char created_at[24]{};
    std::snprintf(created_at, sizeof(created_at), "%04u-%02u-%02u %02u:%02u:%02u",
                  created.wYear, created.wMonth, created.wDay,
                  created.wHour, created.wMinute, created.wSecond);
    output << "\xEF\xBB\xBF";
    output << "Sempervirens migration record\r\n"
           << "Created: " << created_at << "\r\n"
           << "Profile: " << result.profile_name << "\r\n"
           << "Source: " << wide_to_utf8(result.source_root.wstring()) << "\r\n"
           << "Destination: " << wide_to_utf8(result.target_root.wstring()) << "\r\n"
           << "Conflict strategy: " << (strategy == OverwriteStrategy::skip ? "Skip" :
               strategy == OverwriteStrategy::replace ? "Replace" : "BackupAndReplace") << "\r\n\r\n"
           << "Item summary: " << result.success_count << " succeeded; " << result.skipped_count
           << " skipped; " << result.failed_count << " failed\r\n"
           << "File summary: " << result.files_copied << " copied; " << result.files_overwritten
           << " replaced; " << result.files_backed_up << " backed up; "
           << result.files_failed << " failed\r\n"
           << "Backup folder: " << (result.backup_root ? wide_to_utf8(result.backup_root->wstring()) :
                                     "No backup was created")
           << "\r\n\r\nItems:\r\n";
    for (const auto& item : result.items) {
        output << "- " << item.group << " / " << item.name << " [" << item.status << "]\r\n"
               << "  Source: " << wide_to_utf8(item.source_path.wstring()) << "\r\n"
               << "  Destination: " << wide_to_utf8(item.target_path.wstring()) << "\r\n"
               << "  Result: " << item.message << "\r\n";
    }
    write_file_atomically(result.log_path, output.str(), token);
}

} // namespace

#ifdef SEMPERVIRENS_TESTING
void set_copy_progress_observer_for_test(std::function<void(std::uint64_t, std::uint64_t)> observer) {
    copy_progress_observer_for_test = std::move(observer);
}
#endif

MigrationResult execute_migration(const MigrationPlan& plan, OverwriteStrategy strategy,
                                  std::stop_token token, const ScanProgressCallback& progress,
                                  bool english) {
    if (!plan.valid()) throw std::runtime_error(plan.error.empty() ? "Migration plan is not valid" : plan.error);
    if (!safe_directory(plan.source_root) || !safe_directory(plan.target_root) ||
        is_within(plan.source_root, plan.target_root) || is_within(plan.target_root, plan.source_root))
        throw std::runtime_error(english ?
            "The source or destination instance changed after scanning. Scan again before migrating." :
            "扫描后迁出或迁入实例发生了变化，请重新扫描后再迁移。");
    ensure_no_reparse_points_between(plan.source_root, plan.source_root);
    ensure_no_reparse_points_between(plan.target_root, plan.target_root);
    const auto stamp = timestamp();
    const auto backup_root = plan.target_root / "SempervirensBackups" / fs::path(stamp);
    const auto report_root = plan.target_root / "SempervirensReports";
    MigrationResult result;
    result.profile_name = plan.profile.name;
    result.source_root = plan.source_root;
    result.target_root = plan.target_root;
    result.log_path = report_root / (L"sempervirens-report-" + stamp + L".txt");
    std::vector<const PlannedOperation*> selected;
    for (const auto& operation : plan.operations)
        if (operation.selected && (operation.status == ScanStatus::found ||
            operation.status == ScanStatus::identical || operation.status == ScanStatus::conflict))
            selected.push_back(&operation);
    std::unordered_set<std::string> processed_options, processed_servers, backed_up;
    for (std::size_t index = 0; index < selected.size(); ++index) {
        check_cancelled(token);
        const auto& operation = *selected[index];
        if (progress) progress({static_cast<int>(std::min<std::size_t>(98, index * 100 / selected.size())),
                                (english ? L"Processing: " : L"正在迁移：") +
                                    utf8_to_wide(operation.name)});
        const auto& rule = plan.profile.rules.at(operation.rule_index);
        const bool options = operation.is_builtin && rule.builtin == BuiltinKind::options;
        const bool servers = operation.is_builtin && rule.builtin == BuiltinKind::servers;
        const auto key = lower_path(operation.target_path);
        if (options && !processed_options.insert(key).second) continue;
        if (servers && !processed_servers.insert(key).second) continue;
        MigrationItemResult item;
        item.name = options ? "Personal settings" : servers ? "Servers" : operation.name;
        item.group = options ? "Personal settings" : servers ? "Servers" : operation.group;
        item.source_path = operation.source_path;
        item.target_path = operation.target_path;
        try {
            ensure_no_reparse_points_between(plan.source_root, operation.source_path);
            ensure_target(plan.target_root, operation.target_path);
            CopySummary summary;
            if (options || servers) {
                std::vector<const PlannedOperation*> group;
                for (const auto* candidate : selected) {
                    const auto& candidate_rule = plan.profile.rules.at(candidate->rule_index);
                    if (lower_path(candidate->target_path) == key && candidate->is_builtin &&
                        candidate_rule.builtin == (options ? BuiltinKind::options : BuiltinKind::servers))
                        group.push_back(candidate);
                }
                summary = options ? execute_options(plan, group, plan.target_root, strategy,
                                                    backup_root, backed_up, token, english) :
                                    execute_servers(group, plan.target_root, strategy,
                                                    backup_root, backed_up, token, english);
            } else summary = copy_operation(operation, plan.target_root, strategy, backup_root,
                                            backed_up, token, english);
            result.files_copied += summary.copied;
            result.files_overwritten += summary.overwritten;
            result.files_failed += summary.failed;
            if (options || servers) {
                if (summary.is_skipped) ++result.skipped_count;
                else ++result.success_count;
                item.status = summary.is_skipped ? "Skipped" : "Success";
            } else {
                if (summary.copied == 0 && summary.skipped > 0) ++result.skipped_count;
                else if (summary.failed) ++result.failed_count;
                else ++result.success_count;
                item.status = summary.failed ? "Failed" : summary.copied == 0 ? "Skipped" : "Success";
            }
            item.message = summary.message;
            item.files_copied = summary.copied;
            item.files_overwritten = summary.overwritten;
            item.files_backed_up = summary.backed_up;
        } catch (const std::exception& error) {
            check_cancelled(token);
            ++result.failed_count;
            ++result.files_failed;
            item.status = "Failed";
            item.message = error.what();
        }
        result.items.push_back(std::move(item));
    }
    result.files_backed_up = static_cast<int>(backed_up.size());
    if (!backed_up.empty()) result.backup_root = backup_root;
    if (progress) progress({100, english ? L"Migration complete. Writing the record…" :
                                           L"迁移完成，正在保存记录"});
    try { write_report(result, strategy, token); }
    catch (const std::exception&) {
        check_cancelled(token);
        throw std::runtime_error("Migration record could not be saved");
    }
    return result;
}

} // namespace sempervirens
