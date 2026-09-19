#pragma once

#include <filesystem>
#include <optional>

namespace sempervirens {

namespace fs = std::filesystem;

struct AppPreferences {
    bool english = false;
    bool prompt_on_close = true;
    bool close_to_tray = false;
    std::optional<fs::path> migration_profile_path;
    bool use_minecraft_profile = false;
    bool auto_check_updates = true;
};

std::optional<AppPreferences> read_preferences(const fs::path& path);
void write_preferences(const fs::path& path, const AppPreferences& preferences);

} // namespace sempervirens
