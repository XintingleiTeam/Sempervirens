#include "config_difference.hpp"

#include "option_text.hpp"
#include "profile.hpp"
#include "text.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>
#include <iterator>
#include <map>
#include <string_view>

namespace sempervirens {

namespace {
constexpr std::uintmax_t maximum_file_bytes = 512 * 1024;
constexpr std::size_t maximum_fields = 512;
using Values = std::map<std::string, std::pair<std::string, std::string>>;

std::string trim(std::string_view value) {
    const auto start = value.find_first_not_of(" \t\r\n");
    if (start == value.npos) return {};
    return std::string(value.substr(start, value.find_last_not_of(" \t\r\n") - start + 1));
}

std::string read_text(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("Cannot read configuration file");
    std::string result(std::istreambuf_iterator<char>{input}, {});
    if (input.bad()) throw std::runtime_error("Cannot read configuration file");
    return decode_text_with_bom_to_utf8(result);
}

bool valid_depth(std::string_view json) {
    int depth = 0;
    bool quoted = false, escaped = false;
    for (const char c : json) {
        if (quoted) {
            if (escaped) escaped = false;
            else if (c == '\\') escaped = true;
            else if (c == '"') quoted = false;
        } else if (c == '"') quoted = true;
        else if (c == '{' || c == '[') { if (++depth > 64) return false; }
        else if (c == '}' || c == ']') --depth;
    }
    return depth == 0 && !quoted;
}

bool valid_json_number(std::string_view number) {
    std::size_t index = 0;
    if (index < number.size() && number[index] == '-') ++index;
    if (index == number.size()) return false;
    if (number[index] == '0') ++index;
    else {
        if (number[index] < '1' || number[index] > '9') return false;
        do { ++index; } while (index < number.size() && number[index] >= '0' && number[index] <= '9');
    }
    if (index < number.size() && number[index] == '.') {
        ++index;
        const auto first_digit = index;
        while (index < number.size() && number[index] >= '0' && number[index] <= '9') ++index;
        if (index == first_digit) return false;
    }
    if (index < number.size() && (number[index] == 'e' || number[index] == 'E')) {
        ++index;
        if (index < number.size() && (number[index] == '+' || number[index] == '-')) ++index;
        const auto first_digit = index;
        while (index < number.size() && number[index] >= '0' && number[index] <= '9') ++index;
        if (index == first_digit) return false;
    }
    return index == number.size();
}

// JsonElement.ToString keeps a number's source spelling. Encode validated
// number tokens as strings before DOM parsing so exponents and -0 survive.
std::string preserve_json_numbers(std::string_view json) {
    std::string result;
    result.reserve(json.size());
    bool quoted = false, escaped = false;
    for (std::size_t index = 0; index < json.size(); ++index) {
        const auto character = json[index];
        if (quoted) {
            result.push_back(character);
            if (escaped) escaped = false;
            else if (character == '\\') escaped = true;
            else if (character == '"') quoted = false;
            continue;
        }
        if (character == '"') { quoted = true; result.push_back(character); continue; }
        if (character == '-' || (character >= '0' && character <= '9')) {
            const auto start = index;
            while (index < json.size() && json[index] != ',' && json[index] != ']' &&
                   json[index] != '}' && json[index] != ' ' && json[index] != '\t' &&
                   json[index] != '\r' && json[index] != '\n') ++index;
            const auto number = json.substr(start, index - start);
            if (!valid_json_number(number)) throw std::runtime_error("Invalid JSON number");
            result.push_back('"');
            result.append(number);
            result.push_back('"');
            --index;
            continue;
        }
        result.push_back(character);
    }
    return result;
}

bool flatten(const nlohmann::ordered_json& item, std::string path, Values& values, int depth = 0) {
    if (depth > 64 || values.size() >= maximum_fields) return false;
    if (item.is_object()) {
        for (auto it = item.begin(); it != item.end(); ++it)
            if (!flatten(it.value(), path.empty() ? it.key() : path + "." + it.key(), values, depth + 1))
                return false;
        return true;
    }
    if (item.is_array()) {
        for (std::size_t i = 0; i < item.size(); ++i)
            if (!flatten(item[i], path + "[" + std::to_string(i) + "]", values, depth + 1)) return false;
        return true;
    }
    if (!path.empty()) {
        const auto value = item.is_string() ? item.get<std::string>() :
            item.is_null() ? std::string(1, '\0') :
            item.is_boolean() ? (item.get<bool>() ? "True" : "False") : item.dump();
        const auto key = invariant_lower_utf8(path);
        if (const auto existing = values.find(key); existing != values.end())
            existing->second.second = value;
        else values.emplace(key, std::make_pair(std::move(path), value));
    }
    return true;
}

std::optional<Values> read_json(const fs::path& path) {
    const auto normalized = normalize_relaxed_json(read_text(path));
    if (!valid_depth(normalized)) return std::nullopt;
    Values values;
    if (!flatten(nlohmann::ordered_json::parse(preserve_json_numbers(normalized)), {}, values)) return std::nullopt;
    return values;
}

std::optional<Values> read_key_values(const fs::path& path) {
    const auto content = read_text(path);
    Values values;
    std::string section;
    std::size_t start = 0;
    while (start < content.size()) {
        const auto end = content.find_first_of("\r\n", start);
        const auto value = trim(std::string_view(content).substr(start,
            end == std::string::npos ? std::string::npos : end - start));
        if (end == std::string::npos) start = content.size();
        else {
            start = end + 1;
            if (content[end] == '\r' && start < content.size() && content[start] == '\n') ++start;
        }
        if (value.empty() || value[0] == '#' || value[0] == ';') continue;
        if (value.size() >= 2 && value.front() == '[' && value.back() == ']') {
            section = trim(std::string_view(value).substr(1, value.size() - 2));
            continue;
        }
        const auto equal = value.find('=');
        const auto colon = value.find(':');
        const auto separator = equal == value.npos ? colon : colon == value.npos ? equal : std::min(equal, colon);
        if (separator == value.npos || separator == 0) continue;
        const auto key = trim(std::string_view(value).substr(0, separator));
        if (key.empty()) continue;
        auto name = section.empty() ? key : section + "." + key;
        const auto lower = invariant_lower_utf8(name);
        if (values.contains(lower) || values.size() >= maximum_fields) return std::nullopt;
        values[lower] = {std::move(name), trim(std::string_view(value).substr(separator + 1))};
    }
    if (values.empty()) return std::nullopt;
    return values;
}
} // namespace

std::optional<std::vector<SettingDifference>> compare_configuration_files(
    const fs::path& source, const fs::path& target) {
    try {
        std::error_code error;
        if (fs::file_size(source, error) > maximum_file_bytes || error ||
            fs::file_size(target, error) > maximum_file_bytes || error) return std::nullopt;
        const auto extension = ascii_lower(wide_to_utf8(source.extension().wstring()));
        std::optional<Values> source_values, target_values;
        if (extension == ".json" || extension == ".json5") {
            source_values = read_json(source);
            target_values = read_json(target);
        } else if (extension == ".cfg" || extension == ".properties") {
            source_values = read_key_values(source);
            target_values = read_key_values(target);
        } else return std::nullopt;
        if (!source_values || !target_values) return std::nullopt;
        std::vector<SettingDifference> differences;
        std::map<std::string, bool> keys;
        for (const auto& [key, _] : *source_values) keys[key] = true;
        for (const auto& [key, _] : *target_values) keys[key] = true;
        for (const auto& [key, _] : keys) {
            const auto source_it = source_values->find(key);
            const auto target_it = target_values->find(key);
            const auto source_present = source_it != source_values->end();
            const auto target_present = target_it != target_values->end();
            if (source_present && target_present && source_it->second.second == target_it->second.second) continue;
            differences.push_back({source_present ? source_it->second.first : target_it->second.first,
                target_present ? std::optional<std::string>(target_it->second.second) : std::nullopt,
                source_present ? std::optional<std::string>(source_it->second.second) : std::nullopt});
        }
        return differences;
    } catch (const std::exception&) { return std::nullopt; }
}

} // namespace sempervirens
