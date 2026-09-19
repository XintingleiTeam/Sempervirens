#include "scan.hpp"

#include "option_text.hpp"
#include "config_difference.hpp"
#include "nbt.hpp"
#include "path_safety.hpp"
#include "text.hpp"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <stdexcept>
#include <system_error>
#include <unordered_map>
#include <unordered_set>

namespace sempervirens {

bool MigrationPlan::valid() const {
    if (!error.empty()) return false;
    return std::any_of(operations.begin(), operations.end(), [](const PlannedOperation& operation) {
        return operation.selected && (operation.status == ScanStatus::found ||
                                      operation.status == ScanStatus::identical ||
                                      operation.status == ScanStatus::conflict);
    });
}

static void check_cancelled(std::stop_token token) {
    if (token.stop_requested()) throw std::runtime_error("Scan cancelled");
}

static std::wstring file_progress(std::wstring_view label, std::size_t count,
                                  bool english, bool compared = false) {
    return std::wstring(label) + (english ? L" · " : compared ? L" · 已核对 " : L" · 已检查 ") +
           std::to_wstring(count) + (english ? L" files checked" : L" 个文件");
}

static std::string lower_path(const fs::path& path) {
    return invariant_lower_utf8(wide_to_utf8(full_path(path).wstring()));
}

static fs::path windows_rule_path(std::wstring path) {
    std::replace(path.begin(), path.end(), L'/', L'\\');
    return fs::path(path);
}

static fs::path windows_rule_path(std::string_view value) {
    return windows_rule_path(utf8_to_wide(value));
}

static bool safe_file(const fs::path& path) {
    std::error_code error;
    if (!fs::is_regular_file(path, error) || error) return false;
    try { return !is_reparse_point(path); }
    catch (...) { return false; }
}

static bool safe_directory(const fs::path& path) {
    std::error_code error;
    if (!fs::is_directory(path, error) || error) return false;
    try { return !is_reparse_point(path); }
    catch (...) { return false; }
}

static std::vector<fs::path> enumerate_files(const fs::path& path, bool recursive,
                                             std::stop_token token,
                                             const std::function<void(std::size_t)>& on_file = {}) {
    std::vector<fs::path> files;
    if (safe_file(path)) { files.push_back(path); return files; }
    if (!safe_directory(path)) return files;
    std::vector<fs::path> pending{path};
    while (!pending.empty()) {
        check_cancelled(token);
        const auto current = pending.back();
        pending.pop_back();
        std::error_code error;
        for (fs::directory_iterator it(current, fs::directory_options::skip_permission_denied, error), end;
             it != end && !error; it.increment(error)) {
            check_cancelled(token);
            if (safe_file(it->path())) {
                files.push_back(it->path());
                if (on_file && files.size() % 64 == 0) on_file(files.size());
            }
            else if (recursive && safe_directory(it->path())) pending.push_back(it->path());
            error.clear();
        }
    }
    return files;
}

static std::optional<fs::path> first_target_only_file(
    const fs::path& target, const std::unordered_set<std::string>& source_paths,
    std::stop_token token) {
    if (!safe_directory(target)) return std::nullopt;
    std::vector<fs::path> pending{target};
    while (!pending.empty()) {
        check_cancelled(token);
        const auto current = pending.back();
        pending.pop_back();
        std::error_code error;
        for (fs::directory_iterator it(current, fs::directory_options::skip_permission_denied, error), end;
             it != end && !error; it.increment(error)) {
            check_cancelled(token);
            if (safe_directory(it->path())) pending.push_back(it->path());
            else if (safe_file(it->path())) {
                const auto relative = it->path().lexically_relative(target);
                if (!source_paths.contains(invariant_lower_utf8(wide_to_utf8(relative.wstring()))))
                    return relative;
            }
            error.clear();
        }
    }
    return std::nullopt;
}

static bool files_equal(const fs::path& left, const fs::path& right, std::stop_token token) {
    std::error_code error;
    if (!safe_file(left) || !safe_file(right)) return false;
    const auto left_size = fs::file_size(left, error);
    if (error) return false;
    const auto right_size = fs::file_size(right, error);
    if (error || left_size != right_size) return false;
    std::ifstream a(left, std::ios::binary);
    std::ifstream b(right, std::ios::binary);
    if (!a || !b) return false;
    std::array<char, 64 * 1024> left_buffer{};
    std::array<char, 64 * 1024> right_buffer{};
    while (true) {
        check_cancelled(token);
        a.read(left_buffer.data(), left_buffer.size());
        b.read(right_buffer.data(), right_buffer.size());
        const auto n = a.gcount();
        if (n != b.gcount()) return false;
        if (n == 0) return a.eof() && b.eof();
        if (!std::equal(left_buffer.begin(), left_buffer.begin() + n, right_buffer.begin())) return false;
    }
}

static std::string trim(std::string_view value) {
    const auto begin = value.find_first_not_of(" \t\r\n");
    if (begin == value.npos) return {};
    const auto end = value.find_last_not_of(" \t\r\n");
    return std::string(value.substr(begin, end - begin + 1));
}

static bool option_key(std::string_view line, std::string& key, std::string& value) {
    const auto colon = line.find(':');
    key = colon == line.npos ? "" : trim(line.substr(0, colon));
    value = colon == line.npos ? "" : trim(line.substr(colon + 1));
    return colon != line.npos && colon > 0 && !key.empty() && key.front() != '#';
}

static bool option_category(std::string_view key, std::string_view category) {
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

static std::map<std::string, std::string> option_values(const fs::path& path, std::string_view category) {
    std::map<std::string, std::string> values;
    if (!safe_file(path)) return values;
    for (const auto& line : read_option_lines(path)) {
        std::string key, value;
        if (option_key(line, key, value) && option_category(key, category))
            values[invariant_lower_utf8(key)] = value;
    }
    return values;
}

static PlannedOperation base_operation(const MigrationRule& rule, std::size_t rule_index,
                                       const fs::path& source, const fs::path& target,
                                       ScanStatus status, std::string detail = {}) {
    PlannedOperation operation;
    operation.rule_index = rule_index;
    operation.id = rule.id;
    operation.group = rule.group;
    operation.name = rule.name;
    operation.description = rule.description;
    operation.source_path = source;
    operation.target_path = target;
    operation.relative_key = rule.id + ":status";
    operation.status = status;
    operation.detail = std::move(detail);
    operation.is_builtin = rule.kind == RuleKind::builtin;
    operation.recursive = rule.recursive;
    return operation;
}

static PlannedOperation found_operation(const MigrationRule& rule, std::size_t index,
                                        const fs::path& source, const fs::path& target,
                                        ScanStatus status, std::uintmax_t size, bool directory,
                                        std::string detail = {}) {
    auto operation = base_operation(rule, index, source, target, status, std::move(detail));
    operation.relative_key = rule.id + ":" + wide_to_utf8(source.wstring());
    operation.size = size;
    operation.selected = rule.default_selected;
    operation.is_directory = directory;
    return operation;
}

static PlannedOperation scan_option_rule(const MigrationRule& rule, std::size_t index,
                                         const fs::path& source, const fs::path& target) {
    if (!safe_file(source)) return base_operation(rule, index, source, target, ScanStatus::not_found);
    try {
        const auto source_values = option_values(source, rule.category.empty() ? "bindings" : rule.category);
        const auto target_values = option_values(target, rule.category.empty() ? "bindings" : rule.category);
        OptionScanInfo payload;
        payload.source_count = static_cast<int>(source_values.size());
        bool identical = !source_values.empty();
        for (const auto& [key, value] : source_values) {
            const auto existing = target_values.find(key);
            if (existing == target_values.end()) { identical = false; continue; }
            if (existing->second != value) {
                payload.changes.push_back({key, value, existing->second});
                identical = false;
            }
        }
        const auto status = !payload.changes.empty() ? ScanStatus::conflict :
                            identical ? ScanStatus::identical : ScanStatus::found;
        auto operation = found_operation(rule, index, source, target, status, fs::file_size(source), false);
        operation.payload = std::move(payload);
        return operation;
    } catch (const std::exception& error) {
        return base_operation(rule, index, source, target, ScanStatus::unreadable, error.what());
    }
}

static std::vector<ServerEntry> read_servers(const fs::path& path) {
    const auto root = read_nbt_compound(path);
    const auto* list = root.get("servers");
    std::vector<ServerEntry> result;
    if (!list || list->type != NbtType::list || list->item_type != NbtType::compound) return result;
    for (const auto& item : list->items) {
        if (item.type != NbtType::compound) continue;
        ServerEntry entry;
        entry.name = item.get_string("name");
        entry.address = item.get_string("ip");
        if (unicode_blank(entry.name)) entry.name = entry.address;
        entry.identity = invariant_lower_utf8(unicode_blank(entry.address) ? entry.name : entry.address);
        if (unicode_blank(entry.name)) entry.name = "Server " + std::to_string(result.size() + 1);
        entry.source_index = static_cast<int>(result.size());
        entry.payload = std::make_shared<NbtTag>(item);
        result.push_back(std::move(entry));
    }
    return result;
}

static std::vector<PlannedOperation> scan_server_rule(const MigrationRule& rule, std::size_t index,
                                                       const fs::path& source, const fs::path& target) {
    std::vector<PlannedOperation> result;
    if (!safe_file(source)) {
        result.push_back(base_operation(rule, index, source, target, ScanStatus::not_found));
        return result;
    }
    try {
        const auto source_entries = read_servers(source);
        if (source_entries.empty()) {
            result.push_back(base_operation(rule, index, source, target, ScanStatus::empty));
            return result;
        }
        std::unordered_map<std::string, ServerEntry> targets;
        if (safe_file(target)) for (auto& entry : read_servers(target)) {
            const auto identity = entry.identity;
            if (!targets.emplace(identity, std::move(entry)).second)
                throw std::runtime_error("Duplicate server identity in destination list");
        }
        for (const auto& entry : source_entries) {
            const auto existing = targets.find(entry.identity);
            const auto same = existing != targets.end() && nbt_equal(*entry.payload, *existing->second.payload);
            const auto status = existing == targets.end() ?
                (safe_file(target) ? ScanStatus::conflict : ScanStatus::found) :
                same ? ScanStatus::identical : ScanStatus::conflict;
            auto operation = found_operation(rule, index, source, target, status, 0, false,
                                             entry.name + " · " + entry.address);
            operation.relative_key = rule.id + ":" + entry.identity;
            if (existing != targets.end())
                operation.comparison = ServerComparisonInfo{entry.name, existing->second.name,
                    !nbt_server_other_fields_equal(*entry.payload, *existing->second.payload)};
            operation.payload = entry;
            result.push_back(std::move(operation));
        }
    } catch (const std::exception& error) {
        result.push_back(base_operation(rule, index, source, target, ScanStatus::config_error, error.what()));
    }
    return result;
}

static PlannedOperation scan_file_rule(const MigrationRule& rule, std::size_t index,
                                       const fs::path& source, const fs::path& target,
                                       std::stop_token token) {
    if (!safe_file(source)) return base_operation(rule, index, source, target, ScanStatus::not_found);
    std::error_code error;
    const auto size = fs::file_size(source, error);
    if (error) return base_operation(rule, index, source, target, ScanStatus::unreadable);
    if (fs::exists(target, error) && !safe_file(target))
        return found_operation(rule, index, source, target, ScanStatus::conflict, size, false);
    const auto status = !safe_file(target) ? ScanStatus::found :
                        files_equal(source, target, token) ? ScanStatus::identical : ScanStatus::conflict;
    auto operation = found_operation(rule, index, source, target, status, size, false);
    if (status == ScanStatus::conflict) {
        const auto target_size = fs::file_size(target, error);
        if (!error) operation.comparison = FileComparisonInfo{size, target_size,
            compare_configuration_files(source, target)};
    }
    return operation;
}

static PlannedOperation scan_directory_rule(const MigrationRule& rule, std::size_t index,
                                            const fs::path& source, const fs::path& target,
                                            bool recursive, std::stop_token token,
                                            const ScanProgressCallback& progress,
                                            bool english,
                                            bool include_target_only = false) {
    if (!safe_directory(source)) return base_operation(rule, index, source, target, ScanStatus::not_found);
    const auto label = utf8_to_wide(rule.name);
    if (progress) progress({std::nullopt, label});
    const auto files = enumerate_files(source, recursive, token, [&](std::size_t examined) {
        if (progress) progress({std::nullopt, file_progress(label, examined, english)});
    });
    if (files.empty() && !include_target_only)
        return base_operation(rule, index, source, target, ScanStatus::empty);
    std::error_code target_error;
    if (fs::exists(target, target_error) && !safe_directory(target))
        return found_operation(rule, index, source, target, ScanStatus::conflict, 0, true);
    FolderComparisonInfo comparison;
    comparison.source_count = static_cast<int>(files.size());
    std::uintmax_t size = 0;
    std::unordered_set<std::string> source_paths;
    for (std::size_t i = 0; i < files.size(); ++i) {
        check_cancelled(token);
        const auto relative = files[i].lexically_relative(source);
        source_paths.insert(invariant_lower_utf8(wide_to_utf8(relative.wstring())));
        std::error_code error;
        size += fs::file_size(files[i], error);
        if (error) return base_operation(rule, index, source, target, ScanStatus::unreadable);
        const auto destination = target / relative;
        if (!safe_file(destination)) {
            ++comparison.added_count;
            if (comparison.examples.size() < 8) comparison.examples.push_back({relative, FolderDifferenceKind::added});
        } else if (files_equal(files[i], destination, token)) {
            ++comparison.same_count;
        } else {
            ++comparison.changed_count;
            if (comparison.examples.size() < 8) comparison.examples.push_back({relative, FolderDifferenceKind::changed});
        }
        if (progress && (i % 16 == 0 || i + 1 == files.size()))
            progress({static_cast<int>((i + 1) * 100 / files.size()),
                      file_progress(label, i + 1, english)});
        if (include_target_only && (comparison.added_count > 0 || comparison.changed_count > 0)) {
            comparison.counts_complete = false;
            break;
        }
    }
    if (include_target_only && comparison.added_count == 0 && comparison.changed_count == 0) {
        if (progress && safe_directory(target)) progress({std::nullopt, label});
        if (const auto relative = first_target_only_file(target, source_paths, token)) {
            comparison.examples.push_back({*relative, FolderDifferenceKind::target_only});
            comparison.counts_complete = false;
        }
    }
    const bool has_difference = comparison.added_count > 0 || comparison.changed_count > 0 ||
        std::any_of(comparison.examples.begin(), comparison.examples.end(), [](const FolderDifference& item) {
            return item.kind == FolderDifferenceKind::target_only;
        });
    const auto status = include_target_only
        ? (!safe_directory(target) ? ScanStatus::found : has_difference ? ScanStatus::conflict : ScanStatus::identical)
        : comparison.changed_count > 0 ? ScanStatus::conflict
        : comparison.same_count == comparison.source_count ? ScanStatus::identical : ScanStatus::found;
    auto operation = found_operation(rule, index, source, target, status, size, true);
    operation.recursive = recursive;
    operation.comparison = std::move(comparison);
    return operation;
}

static bool glob_match(std::wstring_view pattern, std::wstring_view value) {
    const auto psize = pattern.size();
    const auto vsize = value.size();
    std::vector<std::vector<std::int8_t>> memo(psize + 1, std::vector<std::int8_t>(vsize + 1, -1));
    const auto match = [&](const auto& self, std::size_t p, std::size_t v) -> bool {
        auto& cell = memo[p][v];
        if (cell != -1) return cell != 0;
        bool result = false;
        if (p == psize) result = v == vsize;
        else if (pattern[p] == L'*') {
            const bool double_star = p + 1 < psize && pattern[p + 1] == L'*';
            const auto next = p + (double_star ? 2 : 1);
            result = self(self, next, v) ||
                (v < vsize && (double_star || value[v] != L'/') && self(self, p, v + 1));
        } else if (v < vsize && (pattern[p] == L'?' && value[v] != L'/' ||
            CompareStringOrdinal(&pattern[p], 1, &value[v], 1, TRUE) == CSTR_EQUAL)) {
            result = self(self, p + 1, v + 1);
        }
        cell = result ? 1 : 0;
        return result;
    };
    return match(match, 0, 0);
}

static std::vector<PlannedOperation> scan_glob_rule(const MigrationRule& rule, std::size_t index,
                                                    const fs::path& source_root, const fs::path& target_root,
                                                    std::stop_token token, const ScanProgressCallback& progress,
                                                    bool english) {
    auto pattern = utf8_to_wide(rule.source);
    std::replace(pattern.begin(), pattern.end(), L'\\', L'/');
    const auto wildcard = pattern.find_first_of(L"*?");
    const auto directory_boundary = wildcard == std::wstring::npos
        ? pattern.find_last_of(L'/') : pattern.substr(0, wildcard).find_last_of(L'/');
    const auto prefix = directory_boundary == std::wstring::npos
        ? std::wstring{} : pattern.substr(0, directory_boundary + 1);
    std::vector<PlannedOperation> result;
    std::size_t examined = 0;
    const auto label = utf8_to_wide(rule.name);
    if (progress) progress({std::nullopt, label});
    const auto files = enumerate_files(source_root, true, token, [&](std::size_t count) {
        if (progress) progress({std::nullopt, file_progress(label, count, english)});
    });
    for (const auto& source : files) {
        check_cancelled(token);
        auto relative = source.lexically_relative(source_root).wstring();
        std::replace(relative.begin(), relative.end(), L'\\', L'/');
        if (glob_match(pattern, relative)) {
            auto suffix = prefix.empty() ? relative : relative.substr(prefix.size());
            const auto target = target_root / windows_rule_path(rule.target) / windows_rule_path(suffix);
            ensure_no_reparse_points_between(target_root, target);
            auto operation = scan_file_rule(rule, index, source, target, token);
            operation.detail = wide_to_utf8(relative);
            result.push_back(std::move(operation));
        }
        if (++examined % 64 == 0 && progress)
            progress({std::nullopt, file_progress(label, examined, english, true)});
    }
    if (result.empty()) {
        const auto source = source_root / windows_rule_path(rule.source);
        const auto target = target_root / windows_rule_path(rule.target);
        result.push_back(base_operation(rule, index, source, target, ScanStatus::not_found));
    }
    return result;
}

static std::vector<PlannedOperation> scan_world_rule(const MigrationRule& rule, std::size_t index,
                                                     const fs::path& source, const fs::path& target,
                                                     std::stop_token token, const ScanProgressCallback& progress,
                                                     bool english) {
    std::vector<PlannedOperation> result;
    if (!safe_directory(source)) {
        result.push_back(base_operation(rule, index, source, target, ScanStatus::not_found));
        return result;
    }
    std::vector<fs::path> worlds;
    std::error_code error;
    for (fs::directory_iterator it(source, fs::directory_options::skip_permission_denied, error), end;
         it != end && !error; it.increment(error)) {
        check_cancelled(token);
        if (safe_directory(it->path())) worlds.push_back(it->path());
    }
    for (std::size_t world_index = 0; world_index < worlds.size(); ++world_index) {
        check_cancelled(token);
        const auto& world = worlds[world_index];
        const ScanProgressCallback world_progress = [&](const ScanProgress& value) {
            if (!progress) return;
            auto mapped = value;
            if (mapped.percent)
                mapped.percent = static_cast<int>((world_index * 100 + *mapped.percent) / worlds.size());
            progress(mapped);
        };
        auto operation = scan_directory_rule(rule, index, world, target / world.filename(),
                                             true, token, world_progress, english, true);
        auto world_name = world.filename().wstring();
        const auto level = world / "level.dat";
        if (safe_file(level)) try {
            const auto root = read_nbt_compound(level);
            const auto* data = root.get("Data");
            if (data && data->type == NbtType::compound) {
                const auto name = data->get_string("LevelName");
                if (!name.empty()) world_name = utf8_to_wide(name);
            }
        } catch (...) { /* An unreadable level.dat does not hide the world folder. */ }
        operation.payload = WorldEntry{world.filename().wstring(), world_name};
        operation.relative_key = rule.id + ":" + wide_to_utf8(world.filename().wstring());
        result.push_back(std::move(operation));
        if (progress) progress({static_cast<int>((world_index + 1) * 100 / worlds.size()),
                                utf8_to_wide(rule.name) +
                                    (english ? L" · " : L" · 已检查 ") +
                                    std::to_wstring(world_index + 1) +
                                    (english ? L" worlds checked" : L" 个世界")});
    }
    if (result.empty()) result.push_back(base_operation(rule, index, source, target, ScanStatus::empty));
    return result;
}

static bool try_probe_writable(const fs::path& candidate) {
    HANDLE handle = CreateFileW(candidate.c_str(), GENERIC_WRITE | DELETE, 0, nullptr, CREATE_NEW,
                                FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        const auto error = GetLastError();
        if (error == ERROR_FILE_EXISTS || error == ERROR_ALREADY_EXISTS) return false;
        throw std::system_error(static_cast<int>(error), std::system_category(),
                                "Destination instance is not writable");
    }
    if (!CloseHandle(handle))
        throw std::system_error(static_cast<int>(GetLastError()), std::system_category(),
                                "Could not remove destination write probe");
    return true;
}

#ifdef SEMPERVIRENS_TESTING
bool try_probe_writable_for_test(const fs::path& candidate) {
    return try_probe_writable(candidate);
}
#endif

static void probe_writable(const fs::path& root) {
    static std::atomic_uint64_t next_probe_id{0};
    for (int attempt = 0; attempt < 32; ++attempt) {
        const auto probe = root / (L".sempervirens-cpp-write-test-" +
            std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()) +
            L"-" + std::to_wstring(next_probe_id.fetch_add(1, std::memory_order_relaxed)) + L".tmp");
        if (try_probe_writable(probe)) return;
    }
    throw std::runtime_error("Could not reserve a destination write probe");
}

MigrationPlan build_plan(MigrationProfile profile, const fs::path& source_root,
                         const fs::path& target_root, std::stop_token token,
                         const ScanProgressCallback& progress, bool english) {
    const auto source = full_path(source_root);
    const auto target = full_path(target_root);
    if (!safe_directory(source) || !safe_directory(target))
        throw std::runtime_error(english ?
            "The source and destination must both be existing folders." :
            "迁出和迁入实例必须都是现有文件夹。");
    if (is_within(source, target) || is_within(target, source))
        throw std::runtime_error(english ?
            "The source and destination cannot be the same folder or contain one another." :
            "迁出和迁入实例不能相同，也不能互相包含。");
    ensure_no_reparse_points_between(source, source);
    ensure_no_reparse_points_between(target, target);
    probe_writable(target);
    MigrationPlan plan{source, target, std::move(profile), {}, {}};
    if (progress) progress({0, english ? L"Scanning data" : L"正在扫描内容"});
    const auto count = plan.profile.rules.size();
    for (std::size_t i = 0; i < count; ++i) {
        check_cancelled(token);
        const auto& rule = plan.profile.rules[i];
        const auto rule_source = source / windows_rule_path(rule.source);
        const auto rule_target = target / windows_rule_path(rule.target);
        if (rule.kind == RuleKind::glob) {
            const auto wildcard = rule_source.wstring().find_first_of(L"*?");
            const auto static_prefix = fs::path(rule_source.wstring().substr(0, wildcard)).parent_path();
            ensure_no_reparse_points_between(source, static_prefix);
        } else {
            ensure_no_reparse_points_between(source, rule_source);
        }
        ensure_no_reparse_points_between(target, rule_target);
        const ScanProgressCallback rule_progress = [&](const ScanProgress& value) {
            if (!progress) return;
            if (!value.percent) { progress(value); return; }
            progress({static_cast<int>((i * 100 + std::clamp(*value.percent, 0, 100)) / count),
                      value.message});
        };
        if (progress) progress({static_cast<int>(i * 100 / count), utf8_to_wide(rule.name)});
        std::vector<PlannedOperation> operations;
        if (rule.kind == RuleKind::builtin && rule.builtin == BuiltinKind::options)
            operations.push_back(scan_option_rule(rule, i, rule_source, rule_target));
        else if (rule.kind == RuleKind::builtin && rule.builtin == BuiltinKind::servers)
            operations = scan_server_rule(rule, i, rule_source, rule_target);
        else if (rule.kind == RuleKind::builtin && rule.builtin == BuiltinKind::screenshots)
            operations.push_back(scan_directory_rule(rule, i, rule_source, rule_target,
                                                     true, token, rule_progress, english));
        else if (rule.kind == RuleKind::builtin && rule.builtin == BuiltinKind::worlds)
            operations = scan_world_rule(rule, i, rule_source, rule_target, token, rule_progress, english);
        else if (rule.kind == RuleKind::glob)
            operations = scan_glob_rule(rule, i, source, target, token, rule_progress, english);
        else if (rule.kind == RuleKind::file)
            operations.push_back(scan_file_rule(rule, i, rule_source, rule_target, token));
        else if (rule.kind == RuleKind::directory)
            operations.push_back(scan_directory_rule(rule, i, rule_source, rule_target,
                                                     rule.recursive, token, rule_progress, english));
        else
            operations.push_back(base_operation(rule, i, rule_source, rule_target, ScanStatus::config_error));
        plan.operations.insert(plan.operations.end(), std::make_move_iterator(operations.begin()),
                               std::make_move_iterator(operations.end()));
        if (progress) progress({static_cast<int>((i + 1) * 100 / count), utf8_to_wide(rule.name)});
    }
    if (progress) progress({100, english ? L"Scan complete" : L"扫描完成"});
    std::unordered_map<std::string, std::vector<const PlannedOperation*>> by_target;
    for (const auto& operation : plan.operations) {
        if (!operation.selected || (operation.status != ScanStatus::found &&
            operation.status != ScanStatus::identical && operation.status != ScanStatus::conflict)) continue;
        by_target[lower_path(operation.target_path)].push_back(&operation);
    }
    for (const auto& [_, group] : by_target) {
        std::unordered_set<std::string> ids;
        bool mergeable = true;
        for (const auto* operation : group) {
            ids.insert(invariant_lower_utf8(operation->id));
            const auto& rule = plan.profile.rules[operation->rule_index];
            mergeable &= operation->is_builtin &&
                (rule.builtin == BuiltinKind::options || rule.builtin == BuiltinKind::servers);
        }
        if (ids.size() > 1 && !mergeable) {
            plan.error = "Migration profile creates duplicate destination paths";
            break;
        }
    }
    return plan;
}

} // namespace sempervirens
