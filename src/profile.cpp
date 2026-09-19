#include "profile.hpp"

#include "option_text.hpp"
#include "path_safety.hpp"
#include "text.hpp"

#include <nlohmann/json.hpp>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdint>
#include <fstream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <unordered_set>

namespace sempervirens {

using json = nlohmann::ordered_json;

static const json* property(const json& object, std::string_view name) {
    const json* found = nullptr;
    for (auto it = object.begin(); it != object.end(); ++it)
        if (ascii_lower(it.key()) == ascii_lower(name)) found = &it.value();
    return found;
}

template <typename T>
static T property_value(const json& object, std::string_view name, T fallback,
                        bool null_is_default = false) {
    const auto* value = property(object, name);
    if (!value) return fallback;
    if (value->is_null()) {
        if (null_is_default) return fallback;
        throw std::runtime_error("Migration profile field cannot be null");
    }
    return value->get<T>();
}

static int schema_version(const json& document) {
    const auto* value = property(document, "schemaVersion");
    if (!value) return 1;
    if (!value->is_number_integer())
        throw std::runtime_error("Migration profile schemaVersion must be an integer");
    if (value->is_number_unsigned()) {
        const auto number = value->get<std::uint64_t>();
        if (number > static_cast<std::uint64_t>(std::numeric_limits<int>::max()))
            throw std::runtime_error("Migration profile schemaVersion is out of range");
        return static_cast<int>(number);
    }
    const auto number = value->get<std::int64_t>();
    if (number < std::numeric_limits<int>::min() || number > std::numeric_limits<int>::max())
        throw std::runtime_error("Migration profile schemaVersion is out of range");
    return static_cast<int>(number);
}

static std::string read_file(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("Cannot open migration profile");
    std::string result(std::istreambuf_iterator<char>{input}, {});
    if (input.bad()) throw std::runtime_error("Cannot read migration profile");
    return decode_text_with_bom_to_utf8(result);
}

// System.Text.Json accepts comments and trailing commas in custom profiles.
// Preserve that input contract without changing quoted strings.
std::string normalize_relaxed_json(std::string_view input) {
    std::string without_comments;
    without_comments.reserve(input.size());
    bool quoted = false;
    bool escaped = false;
    for (std::size_t i = 0; i < input.size(); ++i) {
        const char c = input[i];
        if (quoted) {
            without_comments.push_back(c);
            if (escaped) escaped = false;
            else if (c == '\\') escaped = true;
            else if (c == '"') quoted = false;
            continue;
        }
        if (c == '"') {
            quoted = true;
            without_comments.push_back(c);
            continue;
        }
        if (c == '/' && i + 1 < input.size() && input[i + 1] == '/') {
            without_comments.append("  ");
            i += 2;
            while (i < input.size() && input[i] != '\r' && input[i] != '\n') {
                without_comments.push_back(' ');
                ++i;
            }
            if (i < input.size()) without_comments.push_back(input[i]);
            continue;
        }
        if (c == '/' && i + 1 < input.size() && input[i + 1] == '*') {
            without_comments.append("  ");
            i += 2;
            bool closed = false;
            for (; i < input.size(); ++i) {
                if (input[i] == '*' && i + 1 < input.size() && input[i + 1] == '/') {
                    without_comments.append("  ");
                    ++i;
                    closed = true;
                    break;
                }
                without_comments.push_back(input[i] == '\r' || input[i] == '\n' ? input[i] : ' ');
            }
            if (!closed) throw std::runtime_error("Unclosed JSON comment");
            continue;
        }
        without_comments.push_back(c);
    }

    std::string result;
    result.reserve(without_comments.size());
    quoted = false;
    escaped = false;
    for (std::size_t i = 0; i < without_comments.size(); ++i) {
        const char c = without_comments[i];
        if (quoted) {
            result.push_back(c);
            if (escaped) escaped = false;
            else if (c == '\\') escaped = true;
            else if (c == '"') quoted = false;
            continue;
        }
        if (c == '"') { quoted = true; result.push_back(c); continue; }
        if (c == ',') {
            auto next = i + 1;
            while (next < without_comments.size() &&
                   (without_comments[next] == ' ' || without_comments[next] == '\t' ||
                    without_comments[next] == '\r' || without_comments[next] == '\n')) ++next;
            if (next < without_comments.size() && (without_comments[next] == ']' || without_comments[next] == '}'))
                continue;
        }
        result.push_back(c);
    }
    return result;
}

static std::string enum_name(std::string_view value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == value.npos) return {};
    const auto last = value.find_last_not_of(" \t\r\n");
    return ascii_lower(value.substr(first, last - first + 1));
}

static RuleKind parse_rule_kind(const std::string& value) {
    const auto lower = enum_name(value);
    if (lower == "file" || lower == "0") return RuleKind::file;
    if (lower == "directory" || lower == "1") return RuleKind::directory;
    if (lower == "glob" || lower == "2") return RuleKind::glob;
    if (lower == "builtin" || lower == "3") return RuleKind::builtin;
    throw std::runtime_error("Unsupported migration rule kind");
}

static std::optional<BuiltinKind> parse_builtin(const json& rule) {
    const auto* builtin = property(rule, "builtin");
    if (!builtin || builtin->is_null()) return std::nullopt;
    const auto value = enum_name(builtin->get<std::string>());
    if (value == "options" || value == "0") return BuiltinKind::options;
    if (value == "servers" || value == "1") return BuiltinKind::servers;
    if (value == "screenshots" || value == "2") return BuiltinKind::screenshots;
    if (value == "worlds" || value == "3") return BuiltinKind::worlds;
    throw std::runtime_error("Unsupported built-in migration rule");
}

static MigrationProfile parse_profile(std::string_view content, const fs::path& origin) {
    try {
        const auto document = json::parse(normalize_relaxed_json(content));
        if (!document.is_object()) throw std::runtime_error("Migration profile must be an object");
        MigrationProfile profile;
        profile.schema_version = schema_version(document);
        profile.name = property_value(document, "name", std::string{"Sempervirens default profile"});
        profile.description = property_value(document, "description", std::string{}, true);
        profile.loaded_from = origin;
        const auto* rules = property(document, "rules");
        if (profile.schema_version < 1 || unicode_blank(profile.name) || !rules ||
            !rules->is_array() || rules->empty())
            throw std::runtime_error("Migration profile is missing required fields");
        std::unordered_set<std::string> ids;
        for (const auto& item : *rules) {
            if (!item.is_object()) throw std::runtime_error("Migration rule must be an object");
            MigrationRule rule;
            rule.id = property_value(item, "id", std::string{});
            rule.group = property_value(item, "group", std::string{}, true);
            rule.name = property_value(item, "name", std::string{});
            rule.description = property_value(item, "description", std::string{}, true);
            rule.kind = parse_rule_kind(property_value(item, "kind", std::string{"file"}));
            rule.builtin = parse_builtin(item);
            rule.category = property_value(item, "category", std::string{}, true);
            rule.source = property_value(item, "source", std::string{});
            rule.target = property_value(item, "target", std::string{});
            rule.recursive = property_value(item, "recursive", true);
            rule.default_selected = property_value(item, "defaultSelected", false);
            rule.risk = property_value(item, "risk", std::string{"safe"});
            if (unicode_blank(rule.id) || !ids.insert(invariant_lower_utf8(rule.id)).second ||
                unicode_blank(rule.name) ||
                !is_safe_relative_pattern(rule.source) || !is_safe_relative_pattern(rule.target) ||
                (rule.risk != "safe" && rule.risk != "optional" && rule.risk != "warning"))
                throw std::runtime_error("Migration profile contains an invalid rule");
            profile.rules.push_back(std::move(rule));
        }
        return profile;
    } catch (const json::exception& error) {
        throw std::runtime_error(std::string("Migration profile JSON could not be parsed: ") + error.what());
    }
}

MigrationProfile load_profile(const fs::path& path) {
    return parse_profile(read_file(path), full_path(path));
}

MigrationProfile load_builtin_profile(BuiltInProfile profile) {
    const auto resource_id = profile == BuiltInProfile::minecraft ? 102 : 101;
    const auto module = GetModuleHandleW(nullptr);
    const auto resource = FindResourceW(module, MAKEINTRESOURCEW(resource_id), MAKEINTRESOURCEW(10));
    if (!resource) throw std::runtime_error("Embedded migration profile was not found");
    const auto data = LoadResource(module, resource);
    const auto size = SizeofResource(module, resource);
    if (!data || size == 0) throw std::runtime_error("Embedded migration profile is empty");
    const auto* bytes = static_cast<const char*>(LockResource(data));
    if (!bytes) throw std::runtime_error("Embedded migration profile could not be read");
    const auto origin = profile == BuiltInProfile::minecraft ?
        fs::path(L"Embedded Minecraft profile") : fs::path(L"Embedded Xintinglei profile");
    return parse_profile(std::string_view(bytes, size), origin);
}

MigrationProfile load_default_profile(const fs::path& application_directory) {
    const auto nested = application_directory / "profiles" / "vanilla.json";
    if (fs::is_regular_file(nested)) return load_profile(nested);
    const auto adjacent = application_directory / "vanilla.json";
    if (fs::is_regular_file(adjacent)) return load_profile(adjacent);
    return load_builtin_profile(BuiltInProfile::xintinglei);
}

} // namespace sempervirens
