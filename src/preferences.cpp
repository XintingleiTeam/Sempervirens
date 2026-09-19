#include "preferences.hpp"

#include "atomic_file.hpp"
#include "text.hpp"

#include <nlohmann/json.hpp>

#include <fstream>
#include <stdexcept>
#include <string>

namespace sempervirens {

std::optional<AppPreferences> read_preferences(const fs::path& path) {
    try {
        std::error_code error;
        if (!fs::is_regular_file(path, error) || error || fs::file_size(path, error) > 64 * 1024 || error)
            return std::nullopt;
        std::ifstream input(path, std::ios::binary);
        if (!input) return std::nullopt;
        const auto document = nlohmann::json::parse(input);
        if (!document.is_object()) return std::nullopt;
        AppPreferences result;
        if (const auto item = document.find("Language"); item != document.end() && !item->is_null()) {
            if (!item->is_string()) return std::nullopt;
            const auto language = ascii_lower(item->get<std::string>());
            if (language != "english" && language != "simplifiedchinese" &&
                language != "traditionalchinesehongkong") return std::nullopt;
            result.english = language == "english";
        }
        if (const auto item = document.find("PromptOnClose"); item != document.end() && !item->is_null()) {
            if (!item->is_boolean()) return std::nullopt;
            result.prompt_on_close = item->get<bool>();
        }
        if (const auto item = document.find("CloseAction"); item != document.end() && !item->is_null()) {
            if (!item->is_string()) return std::nullopt;
            result.close_to_tray = ascii_lower(item->get<std::string>()) == "minimizetotray";
        }
        if (const auto item = document.find("MigrationProfilePath"); item != document.end() && !item->is_null()) {
            if (!item->is_string()) return std::nullopt;
            const auto path_value = item->get<std::string>();
            if (!path_value.empty()) result.migration_profile_path = fs::path(utf8_to_wide(path_value));
        }
        if (const auto item = document.find("BuiltInProfile"); item != document.end() && !item->is_null()) {
            if (!item->is_string()) return std::nullopt;
            const auto value = ascii_lower(item->get<std::string>());
            if (value != "xintinglei" && value != "minecraft") return std::nullopt;
            result.use_minecraft_profile = value == "minecraft";
        }
        if (const auto item = document.find("AutoCheckUpdates"); item != document.end() && !item->is_null()) {
            if (!item->is_boolean()) return std::nullopt;
            result.auto_check_updates = item->get<bool>();
        }
        return result;
    } catch (const std::exception&) { return std::nullopt; }
}

void write_preferences(const fs::path& path, const AppPreferences& preferences) {
    fs::create_directories(path.parent_path());
    const auto document = nlohmann::json{
        {"Language", preferences.english ? "English" : "SimplifiedChinese"},
        {"PromptOnClose", preferences.prompt_on_close},
        {"CloseAction", preferences.close_to_tray ? "MinimizeToTray" : "Exit"},
        {"BuiltInProfile", preferences.use_minecraft_profile ? "Minecraft" : "Xintinglei"},
        {"AutoCheckUpdates", preferences.auto_check_updates},
        {"MigrationProfilePath", preferences.migration_profile_path ?
            wide_to_utf8(preferences.migration_profile_path->wstring()) : ""}
    };
    write_file_atomically(path, document.dump(2));
}

} // namespace sempervirens
