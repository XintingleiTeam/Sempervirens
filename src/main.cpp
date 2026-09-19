#include "instance_discovery.hpp"
#include "presentation.hpp"
#include "profile.hpp"
#include "scan.hpp"
#include "text.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <iostream>

using namespace sempervirens;

int wmain(int argc, wchar_t** argv) {
    try {
        if (argc == 3 && std::wstring_view(argv[1]) == L"--default-profile-json") {
            try {
                const auto loaded = load_default_profile(fs::path(argv[2]));
                std::cout << nlohmann::json{{"valid", true}, {"name", loaded.name},
                                            {"ruleCount", loaded.rules.size()}}.dump() << '\n';
            } catch (const std::exception&) {
                std::cout << "{\"valid\":false}\n";
            }
            return 0;
        }
        if (argc == 3 && std::wstring_view(argv[1]) == L"--profile-json") {
            try {
                const auto loaded = load_profile(fs::path(argv[2]));
                nlohmann::json rules = nlohmann::json::array();
                for (const auto& rule : loaded.rules) {
                    const auto kind = rule.kind == RuleKind::file ? "file" :
                        rule.kind == RuleKind::directory ? "directory" :
                        rule.kind == RuleKind::glob ? "glob" : "builtin";
                    const auto builtin = !rule.builtin ? nlohmann::json(nullptr) :
                        nlohmann::json(*rule.builtin == BuiltinKind::options ? "options" :
                            *rule.builtin == BuiltinKind::servers ? "servers" :
                            *rule.builtin == BuiltinKind::screenshots ? "screenshots" : "worlds");
                    rules.push_back({{"id", rule.id}, {"group", rule.group}, {"name", rule.name},
                        {"description", rule.description}, {"kind", kind}, {"builtin", builtin},
                        {"category", rule.category}, {"source", rule.source}, {"target", rule.target},
                        {"recursive", rule.recursive}, {"defaultSelected", rule.default_selected},
                        {"risk", rule.risk}});
                }
                std::cout << nlohmann::json{{"valid", true},
                    {"schemaVersion", loaded.schema_version}, {"name", loaded.name},
                    {"description", loaded.description}, {"rules", std::move(rules)}}.dump() << '\n';
            } catch (const std::exception&) {
                std::cout << "{\"valid\":false}\n";
            }
            return 0;
        }
        if (argc == 3 && std::wstring_view(argv[1]) == L"--discover-json") {
            const auto selection = discover_instances(fs::path(argv[2]),
                L"默认 Minecraft 实例", L"当前文件夹");
            nlohmann::json candidates = nlohmann::json::array();
            for (const auto& item : selection.candidates)
                candidates.push_back({
                    {"name", wide_to_utf8(item.display_name)},
                    {"root", wide_to_utf8(item.root.wstring())},
                    {"isolated", item.version_isolated},
                    {"markers", item.marker_count},
                    {"usable", item.usable},
                    {"options", item.has_options},
                    {"servers", item.has_servers},
                    {"screenshots", item.screenshot_count},
                    {"worlds", item.world_count},
                    {"note", wide_to_utf8(item.access_note)}
                });
            std::cout << nlohmann::json{{"candidates", std::move(candidates)}}.dump() << '\n';
            return 0;
        }
        if (argc == 5 && std::wstring_view(argv[1]) == L"--plan-json") {
            const auto profile = load_profile(fs::path(argv[2]));
            const auto plan = build_plan(profile, fs::path(argv[3]), fs::path(argv[4]));
            nlohmann::json operations = nlohmann::json::array();
            const auto relative = [](const fs::path& root, const fs::path& path) {
                auto value = wide_to_utf8(path.lexically_relative(root).wstring());
                std::replace(value.begin(), value.end(), '\\', '/');
                return value;
            };
            const auto status_name = [](ScanStatus status) -> const char* {
                switch (status) {
                case ScanStatus::found: return "Found";
                case ScanStatus::identical: return "Identical";
                case ScanStatus::not_found: return "NotFound";
                case ScanStatus::partial: return "Partial";
                case ScanStatus::unreadable: return "Unreadable";
                case ScanStatus::config_error: return "ConfigError";
                case ScanStatus::empty: return "Empty";
                case ScanStatus::conflict: return "Conflict";
                }
                return "Unknown";
            };
            for (const auto& operation : plan.operations) {
                nlohmann::json settings = nullptr;
                if (const auto* file = std::get_if<FileComparisonInfo>(&operation.comparison);
                    file && file->settings) {
                    settings = nlohmann::json::array();
                    for (const auto& change : *file->settings)
                        settings.push_back({{"key", change.key}, {"target", change.target_value},
                                            {"source", change.source_value}});
                }
                operations.push_back({
                    {"id", operation.id}, {"name", operation.name},
                    {"status", status_name(operation.status)}, {"selected", operation.selected},
                    {"source", relative(plan.source_root, operation.source_path)},
                    {"target", relative(plan.target_root, operation.target_path)},
                    {"size", operation.size}, {"directory", operation.is_directory},
                    {"builtin", operation.is_builtin}, {"settings", std::move(settings)},
                    {"difference", wide_to_utf8(explain_migration(operation, profile,
                        OverwriteStrategy::backup_and_replace, false).difference)}
                });
            }
            std::cout << nlohmann::json{{"valid", plan.valid()}, {"error", plan.error},
                                       {"operations", std::move(operations)}}.dump() << '\n';
            return 0;
        }
        if (argc != 3 || std::wstring_view(argv[1]) != L"--inspect") {
            std::cout << "Sempervirens core inspector (read-only)\n"
                      << "Usage: Sempervirens-inspect.exe --inspect <instance-folder>\n"
                      << "       Sempervirens-inspect.exe --plan-json <profile> <source> <destination>\n";
            return argc == 1 ? 0 : 2;
        }
        const auto profile = load_default_profile(fs::current_path());
        const auto selection = discover_instances(fs::path(argv[2]), L"默认 Minecraft 实例", L"当前文件夹");
        std::cout << "Profile: " << profile.name << " (" << profile.rules.size() << " rules)\n";
        for (const auto& instance : selection.candidates) {
            std::cout << wide_to_utf8(instance.display_name) << " | "
                      << wide_to_utf8(instance.root.wstring()) << " | "
                      << instance.screenshot_count << " screenshots | "
                      << instance.world_count << " worlds\n";
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Inspection failed: " << error.what() << '\n';
        return 1;
    }
}
