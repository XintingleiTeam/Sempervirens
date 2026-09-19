#include "presentation.hpp"

#include "text.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <unordered_map>

namespace sempervirens {

namespace {
using Pair = std::pair<std::wstring, std::wstring>;

std::wstring choose(bool english, std::wstring_view chinese, std::wstring_view translated) {
    return std::wstring(english ? translated : chinese);
}

std::wstring readable(std::string_view value) {
    std::wstring result;
    bool separated = true;
    char previous = 0;
    for (char character : value) {
        if (character == '.' || character == '_' || character == '-') {
            if (!result.empty() && result.back() != L' ') result.push_back(L' ');
            separated = true;
        } else {
            if (!separated && std::isupper(static_cast<unsigned char>(character)) &&
                (std::islower(static_cast<unsigned char>(previous)) ||
                 std::isdigit(static_cast<unsigned char>(previous)))) result.push_back(L' ');
            result.push_back(separated ? static_cast<wchar_t>(std::toupper(static_cast<unsigned char>(character))) :
                                       static_cast<wchar_t>(character));
            separated = false;
        }
        previous = character;
    }
    return result;
}

std::wstring short_value(std::string_view value) {
    auto converted = utf8_to_wide(value);
    if (converted.size() > 60) converted = converted.substr(0, 60) + L"…";
    return converted;
}

std::wstring friendly_value(std::string_view key, std::string_view value, bool english) {
    const auto lower = ascii_lower(key);
    const auto lowered = ascii_lower(value);
    if (lower.starts_with("key_")) return display_binding_value(value, english);
    if (lower.starts_with("soundcategory_") || lower == "sound" || lower == "music") {
        try {
            const auto fraction = std::stod(std::string(value));
            if (fraction >= 0 && fraction <= 1)
                return std::to_wstring(static_cast<int>(std::round(fraction * 100))) + L"%";
        } catch (...) {}
    }
    if (lower == "guiscale" && value == "0") return choose(english, L"自动", L"Auto");
    if (lower == "particles") {
        if (value == "0") return choose(english, L"全部", L"All");
        if (value == "1") return choose(english, L"减少", L"Decreased");
        if (value == "2") return choose(english, L"最少", L"Minimal");
    }
    if (lowered == "true") return choose(english, L"开启", L"On");
    if (lowered == "false") return choose(english, L"关闭", L"Off");
    return short_value(value);
}

std::wstring option_name(std::string_view key, bool english) {
    static const std::unordered_map<std::string, Pair> names = {
        {"soundcategory_master", {L"主音量", L"Master volume"}},
        {"soundcategory_music", {L"音乐音量", L"Music volume"}},
        {"soundcategory_weather", {L"天气音量", L"Weather volume"}},
        {"soundcategory_blocks", {L"方块音效", L"Block sounds"}},
        {"soundcategory_hostile", {L"敌对生物音量", L"Hostile creatures"}},
        {"soundcategory_neutral", {L"中立生物音量", L"Friendly creatures"}},
        {"soundcategory_players", {L"玩家音效", L"Player sounds"}},
        {"soundcategory_ambient", {L"环境音效", L"Ambient sounds"}},
        {"soundcategory_voice", {L"语音音量", L"Voice sounds"}},
        {"soundcategory_records", {L"唱片音量", L"Record volume"}},
        {"soundcategory_friendly", {L"友好生物音量", L"Friendly creatures"}},
        {"music", {L"音乐音量", L"Music volume"}},
        {"sound", {L"音效音量", L"Sound volume"}},
        {"renderdistance", {L"渲染距离", L"Render distance"}},
        {"simulationdistance", {L"模拟距离", L"Simulation distance"}},
        {"entitydistancescaling", {L"实体显示距离", L"Entity render distance"}},
        {"particles", {L"粒子效果", L"Particles"}},
        {"guiscale", {L"界面缩放", L"Interface scale"}},
        {"fullscreen", {L"全屏模式", L"Fullscreen"}},
        {"fullscreenresolution", {L"全屏分辨率", L"Fullscreen resolution"}},
        {"viewbobbing", {L"视角晃动", L"View bobbing"}},
        {"attackindicator", {L"攻击指示器", L"Attack indicator"}},
        {"narrator", {L"旁白", L"Narrator"}},
        {"language", {L"游戏语言", L"Language"}},
        {"rawmouseinput", {L"原始鼠标输入", L"Raw mouse input"}},
        {"mousesensitivity", {L"鼠标灵敏度", L"Mouse sensitivity"}},
        {"invertymouse", {L"反转鼠标 Y 轴", L"Invert mouse Y axis"}},
        {"biomeblendradius", {L"生物群系混合范围", L"Biome blend"}},
        {"chatvisibility", {L"聊天可见性", L"Chat visibility"}},
        {"chatopacity", {L"聊天透明度", L"Chat opacity"}},
        {"chatscale", {L"聊天界面缩放", L"Chat scale"}},
        {"chatwidth", {L"聊天框宽度", L"Chat width"}},
        {"chatheightfocused", {L"聊天框展开高度", L"Focused chat height"}},
        {"chatheightunfocused", {L"聊天框收起高度", L"Unfocused chat height"}},
        {"telemetryoptin", {L"遥测数据发送", L"Telemetry sharing"}},
        {"fov", {L"视野范围", L"Field of view"}},
        {"gamma", {L"亮度", L"Brightness"}}
    };
    const auto lower = ascii_lower(key);
    if (const auto it = names.find(lower); it != names.end()) return english ? it->second.second : it->second.first;
    if (lower.starts_with("key_")) return display_binding_name(key, english);
    if (lower.starts_with("soundcategory_")) return readable(key.substr(14)) + choose(english, L"音量", L" volume");
    return readable(key);
}

std::wstring describe_options(const PlannedOperation& operation, const OptionScanInfo& options, bool english) {
    if (operation.status == ScanStatus::identical)
        return choose(english, L"全部设置与目标一致。", L"All settings already match the destination.");
    if (options.changes.empty())
        return choose(english, L"可导入 ", L"Ready to import ") + std::to_wstring(options.source_count) +
            choose(english, L" 项设置，无冲突。", L" settings. No conflicts.");
    std::wstring result;
    for (const auto& change : options.changes) {
        if (!result.empty()) result += L"\n";
        result += L"• " + option_name(change.key, english) + (english ? L": " : L"：") +
            friendly_value(change.key, change.target_value, english) + L" → " +
            friendly_value(change.key, change.source_value, english);
    }
    return result;
}

std::wstring describe_folder(const FolderComparisonInfo& folder, bool english) {
    if (folder.added_count == 0 && folder.changed_count == 0 && folder.examples.empty())
        return choose(english, L"文件均与目标一致。", L"All files already match the destination.");
    std::wstring result = choose(english, L"迁出目录共 ", L"Source contains ") +
        std::to_wstring(folder.source_count) + choose(english, L" 个文件。", L" files.");
    if (folder.counts_complete) result += choose(english, L" 新增 ", L" New: ") +
        std::to_wstring(folder.added_count) + choose(english, L"，不同 ", L"; different: ") +
        std::to_wstring(folder.changed_count) + L"。";
    for (const auto& example : folder.examples) {
        result += L"\n• ";
        result += example.kind == FolderDifferenceKind::added ? choose(english, L"目标缺少：", L"New: ") :
                  example.kind == FolderDifferenceKind::changed ? choose(english, L"内容不同：", L"Different: ") :
                  choose(english, L"目标独有（保留）：", L"Destination-only (kept): ");
        result += example.relative_path.wstring();
    }
    if (!folder.counts_complete) result += choose(english, L"\n仅列出先发现的差异。", L"\nOnly the first differences are shown.");
    return result;
}

std::wstring configuration_value(const std::optional<std::string>& value, bool english) {
    if (!value) return choose(english, L"无此项", L"Not present");
    if (value->size() == 1 && (*value)[0] == '\0') return choose(english, L"空值", L"Null");
    const auto lower = ascii_lower(*value);
    if (lower == "true") return choose(english, L"开启", L"On");
    if (lower == "false") return choose(english, L"关闭", L"Off");
    auto text = utf8_to_wide(*value);
    std::replace(text.begin(), text.end(), L'\r', L' ');
    std::replace(text.begin(), text.end(), L'\n', L' ');
    if (text.size() > 60) text = text.substr(0, 60) + L"…";
    return text;
}

std::wstring readable_setting_key(std::string_view key, bool english) {
    std::wstring result;
    std::size_t start = 0;
    while (start <= key.size()) {
        const auto end = key.find('.', start);
        const auto part = key.substr(start, end == key.npos ? key.npos : end - start);
        if (!part.empty()) {
            if (!result.empty()) result += L" / ";
            const auto lower = ascii_lower(part);
            if (lower == "enabled") result += choose(english, L"是否启用", L"Enabled");
            else if (lower == "hotkeys") result += choose(english, L"快捷键", L"Hotkeys");
            else if (lower == "openmenu") result += choose(english, L"打开菜单", L"Open menu");
            else {
                std::string separated;
                separated.reserve(part.size() + 4);
                for (std::size_t index = 0; index < part.size(); ++index) {
                    const auto character = part[index];
                    if (index > 0 && character >= 'A' && character <= 'Z' &&
                        ((part[index - 1] >= 'a' && part[index - 1] <= 'z') ||
                         (part[index - 1] >= '0' && part[index - 1] <= '9')))
                        separated.push_back(' ');
                    separated.push_back(character == '_' ? ' ' : character);
                }
                result += utf8_to_wide(separated);
            }
        }
        if (end == key.npos) break;
        start = end + 1;
    }
    return result;
}

std::wstring file_size(std::uintmax_t bytes) {
    if (bytes < 1024) return std::to_wstring(bytes) + L" B";
    const auto unit = bytes < 1024 * 1024 ? 1024.0 :
        bytes < 1024ull * 1024 * 1024 ? 1024.0 * 1024 : 1024.0 * 1024 * 1024;
    const auto suffix = bytes < 1024 * 1024 ? L" KB" :
        bytes < 1024ull * 1024 * 1024 ? L" MB" : L" GB";
    std::wostringstream stream;
    stream << std::fixed << std::setprecision(unit == 1024.0 * 1024 * 1024 ? 2 : 1)
           << static_cast<double>(bytes) / unit;
    auto number = stream.str();
    while (number.find(L'.') != std::wstring::npos && number.back() == L'0') number.pop_back();
    if (!number.empty() && number.back() == L'.') number.pop_back();
    return number + suffix;
}

std::wstring user_facing_detail(const PlannedOperation& operation, bool english) {
    if (operation.status == ScanStatus::unreadable)
        return choose(english,
            L"无法读取此内容。请确认文件未被其他程序占用，并检查访问权限和文件完整性。",
            L"This content could not be read. Close programs using it, then check access permissions and file integrity.");
    if (operation.status == ScanStatus::config_error) {
        const auto detail = ascii_lower(operation.detail);
        if (detail.find("duplicate server identity") != std::string::npos)
            return choose(english,
                L"服务器列表中存在重复地址。请先在目标实例中整理重复服务器，再重新扫描。",
                L"The server list contains duplicate addresses. Remove the duplicates in the destination, then scan again.");
        if (detail.find("duplicate setting key") != std::string::npos)
            return choose(english,
                L"设置文件中存在重复项目。请先整理目标设置文件，再重新扫描。",
                L"The settings file contains duplicate entries. Clean up the destination settings, then scan again.");
        return choose(english,
            L"文件格式异常或内容不完整，无法安全解析。请修复或重新生成该文件后再扫描。",
            L"The file format is invalid or incomplete and cannot be read safely. Repair or recreate the file, then scan again.");
    }
    return utf8_to_wide(operation.detail);
}

std::wstring describe_file(const PlannedOperation& operation, const FileComparisonInfo& file, bool english) {
    const auto filename = operation.target_path.filename().wstring();
    if (!file.settings)
        return filename + choose(english, L" 的内容不同。文件大小：目标 ", L" differs. File sizes: destination ") +
            file_size(file.target_bytes) + L" → " + choose(english, L"迁出 ", L"source ") +
            file_size(file.source_bytes) +
            choose(english, L"；无法逐项比较。", L". Individual values cannot be compared.");
    if (file.settings->empty())
        return filename + choose(english, L" 可识别的设置值相同；文件格式、顺序或其他内容不同。",
                                 L" has the same parsed settings; its formatting, order, or other file content differs.");
    std::wstring result;
    for (const auto& change : *file.settings) {
        if (!result.empty()) result += L"\n";
        result += L"• " + readable_setting_key(change.key, english) + L": " +
            configuration_value(change.target_value, english) +
            L" → " + configuration_value(change.source_value, english);
    }
    return result;
}

std::wstring introduction(const PlannedOperation& operation, const MigrationRule& rule, bool english) {
    const auto id = ascii_lower(rule.id);
    if (id.find("xaero") != std::string::npos)
        return choose(english, L"Xaero 保存小地图、世界地图、航点及显示偏好。",
                      L"Xaero keeps your minimap, world map, waypoints and display preferences.");
    if (id.find("litematica") != std::string::npos || id.find("schematic") != std::string::npos)
        return choose(english, L"Litematica 保存建筑投影和个人操作偏好。",
                      L"Litematica saves build blueprints and personal placement preferences.");
    if (id.find("itemscroller") != std::string::npos)
        return choose(english, L"Item Scroller 用于快速移动、整理物品，也可保存操作预设。",
                      L"Item Scroller helps move and arrange items quickly.");
    if (id.find("chesttracker") != std::string::npos)
        return choose(english, L"Chest Tracker 记录你遇到过的箱子与容器。",
                      L"Chest Tracker remembers containers you have encountered.");
    if (id.find("inventory_profiles") != std::string::npos)
        return choose(english, L"Inventory Profiles Next 用于背包整理和物品栏操作。",
                      L"Inventory Profiles Next manages inventory sorting.");
    if (id.find("tweakeroo") != std::string::npos)
        return choose(english, L"Tweakeroo 保存快捷键和辅助操作功能的偏好。",
                      L"Tweakeroo stores shortcuts and convenience-feature preferences.");
    if (rule.builtin == BuiltinKind::options)
        return choose(english, L"Minecraft 把按键、声音和画面等偏好保存在设置文件中。",
                      L"Minecraft stores personal controls, sound and display preferences in one settings file.");
    if (rule.builtin == BuiltinKind::servers)
        return choose(english, L"服务器列表保存常用的联机地址和名称。",
                      L"The server list remembers multiplayer addresses and names.");
    if (rule.builtin == BuiltinKind::worlds)
        return choose(english, L"单人世界包含地图、进度和其他本地存档数据。",
                      L"A single-player world includes terrain, progress and other local save data.");
    if (rule.builtin == BuiltinKind::screenshots)
        return choose(english, L"截图是游戏中保存的图片。", L"Screenshots are pictures saved in-game.");
    (void)operation;
    return choose(english, L"这是迁移规则识别出的个人数据。",
                  L"This is personal data identified by the active migration profile.");
}

std::wstring scope(const PlannedOperation& operation, const MigrationRule& rule, bool english) {
    if (rule.builtin == BuiltinKind::options) {
        const auto category = ascii_lower(rule.category);
        if (category == "bindings") return choose(english,
            L"只合并按键设置；目标实例的其他选项保持原样。",
            L"Only key bindings are merged. Other destination settings stay unchanged.");
        if (category == "audio") return choose(english, L"只合并音量和各类声音设置。",
                                                L"Only volume and sound-category settings are merged.");
        if (category == "video") return choose(english,
            L"只合并画面、视距和相关界面设置；不同电脑上效果可能不同。",
            L"Only display, view-distance and related interface settings are merged. The result may differ between computers.");
        return choose(english, L"只合并所选的这一组设置。",
                      L"Only the selected group of settings is merged.");
    }
    if (rule.builtin == BuiltinKind::servers)
        return choose(english, L"只处理这一条服务器；目标列表里的其他服务器保留。",
                      L"Only this server entry is considered; other destination servers remain.");
    if (rule.builtin == BuiltinKind::worlds)
        return choose(english, L"复制世界的整个文件夹，包括存档数据与玩家进度。",
                      L"The whole world folder is copied, including save data and player progress.");
    const auto description = !english && !unicode_blank(rule.description) ?
        utf8_to_wide(rule.description) + L"\n" : std::wstring{};
    if (operation.is_directory)
        return description + choose(english,
            L"复制这个文件夹中的文件；遇到目标中已有的文件时，逐个按所选方式处理。",
            L"Files in this folder are copied. Existing files are handled individually using the selected policy.");
    return description + choose(english, L"复制整个文件；不会逐项合并文件内部的设置。",
                  L"This file is copied as a whole; its contents are not merged field by field.");
}

std::wstring difference(const PlannedOperation& operation, bool english) {
    if (operation.status == ScanStatus::not_found)
        return choose(english, L"迁出实例中没有找到这项内容。", L"The source item was not found.");
    if (operation.status == ScanStatus::unreadable || operation.status == ScanStatus::config_error)
        return choose(english, L"这项内容目前无法读取，请检查路径与权限。",
                      L"This item cannot be read. Check paths and permissions.");
    if (const auto* options = std::get_if<OptionScanInfo>(&operation.payload))
        return describe_options(operation, *options, english);
    if (const auto* server = std::get_if<ServerComparisonInfo>(&operation.comparison)) {
        std::wstring result;
        if (server->source_name != server->target_name)
            result = choose(english, L"• 服务器名称：", L"• Server name: ") +
                utf8_to_wide(server->target_name) + L" → " + utf8_to_wide(server->source_name) +
                choose(english, L"（迁移后保留目标名称）", L" (destination name stays)");
        if (server->other_fields_differ) {
            if (!result.empty()) result += L"\n";
            result += choose(english, L"• 服务器图标或其他保存选项不同。",
                             L"• Server icon or other saved options differ.");
        }
        return result.empty() ? choose(english, L"这条服务器与目标一致。",
                                       L"This server already matches the destination.") : result;
    }
    if (const auto* folder = std::get_if<FolderComparisonInfo>(&operation.comparison))
        return describe_folder(*folder, english);
    if (const auto* file = std::get_if<FileComparisonInfo>(&operation.comparison))
        return describe_file(operation, *file, english);
    if (const auto* server = std::get_if<ServerEntry>(&operation.payload))
        if (operation.status == ScanStatus::found)
            return choose(english, L"目标列表中没有这条服务器，将新增：", L"This server will be added: ") +
                utf8_to_wide(server->name) + L"（" + utf8_to_wide(server->address) + L"）";
    if (operation.status == ScanStatus::identical)
        return choose(english, L"目标实例已有相同内容，无需重复写入。",
                      L"The destination already has identical content.");
    if (operation.status == ScanStatus::found)
        return choose(english, L"目标实例尚无这项内容，将新增。",
                      L"The destination does not contain this item yet; it will be added.");
    return choose(english, L"目标实例已有同名但不同的内容。",
                  L"Same-named content exists in the destination but differs.");
}

std::wstring outcome(const PlannedOperation& operation, const MigrationRule& rule,
                     OverwriteStrategy strategy, bool english) {
    if (operation.status == ScanStatus::identical || operation.status == ScanStatus::not_found ||
        operation.status == ScanStatus::unreadable || operation.status == ScanStatus::config_error)
        return choose(english, L"这项内容不会改动目标文件。", L"No destination files will change for this item.");
    if (rule.builtin == BuiltinKind::options) {
        if (strategy == OverwriteStrategy::skip && fs::exists(operation.target_path))
            return choose(english, L"保留目标设置文件；这一组设置不会导入。",
                          L"Keep destination settings; this group will not be imported.");
        if (strategy == OverwriteStrategy::backup_and_replace && operation.status == ScanStatus::conflict)
            return choose(english, L"先备份目标设置文件，再合并所选设置；其他设置保持原样。",
                          L"Back up destination settings, then merge the selected group.");
        return choose(english, L"合并所选设置组；其他设置保持原样。",
                      L"Merge the selected settings group; other settings stay unchanged.");
    }
    if (rule.builtin == BuiltinKind::servers) {
        if (strategy == OverwriteStrategy::skip && operation.status == ScanStatus::conflict)
            return choose(english, L"保留目标实例中已有的这条服务器。",
                          L"Keep the existing destination server entry.");
        if (operation.status == ScanStatus::conflict)
            return strategy == OverwriteStrategy::backup_and_replace ?
                choose(english, L"先备份服务器列表，再更新记录；目标名称保留。",
                       L"Back up the server list, then update the entry while keeping its name.") :
                choose(english, L"更新服务器记录，但保留目标中的名称。",
                       L"Update the server entry while keeping its destination name.");
        return choose(english, L"把这条服务器加入目标列表。", L"Add this server to the destination list.");
    }
    if (operation.status == ScanStatus::found)
        return choose(english, L"复制到目标实例，不替换已有文件。",
                      L"Copy this item without replacing existing files.");
    if (strategy == OverwriteStrategy::skip)
        return operation.is_directory ? choose(english, L"保留已有文件，只复制目标尚无的文件。",
                                               L"Keep existing files and copy only missing ones.") :
            choose(english, L"保留目标中的文件，不复制这一项。",
                   L"Keep the destination file; this item will not be copied.");
    if (strategy == OverwriteStrategy::backup_and_replace)
        return choose(english, L"先备份目标中不同的文件，再用迁出文件覆盖。",
                      L"Back up differing destination files, then replace them.");
    return choose(english, L"直接覆盖目标中的不同文件，不创建备份。",
                  L"Replace differing files without creating a backup.");
}

} // namespace

std::wstring display_binding_name(std::string_view key, bool english) {
    auto action = key.starts_with("key_key.") ? key.substr(8) : key.starts_with("key_") ? key.substr(4) : key;
    if (action.starts_with("key.")) action.remove_prefix(4);
    static const std::unordered_map<std::string, Pair> actions = {
        {"forward", {L"前进", L"Move forward"}}, {"back", {L"后退", L"Move backward"}},
        {"left", {L"向左移动", L"Strafe left"}}, {"right", {L"向右移动", L"Strafe right"}},
        {"jump", {L"跳跃", L"Jump"}}, {"sneak", {L"潜行", L"Sneak"}},
        {"sprint", {L"疾跑", L"Sprint"}}, {"attack", {L"攻击 / 破坏", L"Attack / destroy"}},
        {"use", {L"使用 / 放置", L"Use / place"}}, {"pickitem", {L"选取方块", L"Pick block"}},
        {"drop", {L"丢弃物品", L"Drop item"}}, {"inventory", {L"打开物品栏", L"Open inventory"}},
        {"chat", {L"打开聊天", L"Open chat"}}, {"playerlist", {L"玩家列表", L"Player list"}},
        {"command", {L"输入命令", L"Open command"}}, {"advancements", {L"进度", L"Advancements"}},
        {"swapoffhand", {L"交换副手物品", L"Swap offhand item"}},
        {"screenshot", {L"截图", L"Screenshot"}},
        {"toggleperspective", {L"切换视角", L"Toggle perspective"}},
        {"smoothcamera", {L"平滑镜头", L"Smooth camera"}},
        {"fullscreen", {L"切换全屏", L"Toggle fullscreen"}},
        {"savetoolbaractivator", {L"保存快捷栏", L"Save hotbar"}},
        {"loadtoolbaractivator", {L"载入快捷栏", L"Load hotbar"}},
        {"socialinteractions", {L"社交互动", L"Social interactions"}}
    };
    const auto lowered = ascii_lower(action);
    if (const auto it = actions.find(lowered); it != actions.end())
        return english ? it->second.second : it->second.first;
    if (lowered.starts_with("hotbar.")) {
        try {
            const auto slot = std::stoi(std::string(action.substr(7)));
            if (slot >= 1 && slot <= 9)
                return choose(english, L"快捷栏第 ", L"Hotbar slot ") + std::to_wstring(slot) +
                    (english ? L"" : L" 格");
        } catch (...) {}
    }
    if (const auto separator = action.find('.'); separator != action.npos && separator > 0 &&
        separator + 1 < action.size())
        return readable(action.substr(0, separator)) + L" · " + readable(action.substr(separator + 1));
    return readable(action);
}

std::wstring display_binding_value(std::string_view value, bool english) {
    const auto lower = ascii_lower(value);
    if (lower.starts_with("key.keyboard.")) {
        const auto key = lower.substr(13);
        static const std::unordered_map<std::string, Pair> names = {
            {"unknown", {L"未设置", L"Unassigned"}}, {"space", {L"空格", L"Space"}},
            {"enter", {L"回车", L"Enter"}}, {"escape", {L"Esc", L"Esc"}},
            {"tab", {L"Tab", L"Tab"}}, {"backspace", {L"退格", L"Backspace"}},
            {"up", {L"上方向键", L"Up arrow"}}, {"down", {L"下方向键", L"Down arrow"}},
            {"left", {L"左方向键", L"Left arrow"}}, {"right", {L"右方向键", L"Right arrow"}},
            {"left.shift", {L"左 Shift", L"Left Shift"}},
            {"right.shift", {L"右 Shift", L"Right Shift"}},
            {"left.control", {L"左 Ctrl", L"Left Ctrl"}},
            {"right.control", {L"右 Ctrl", L"Right Ctrl"}},
            {"left.alt", {L"左 Alt", L"Left Alt"}}, {"right.alt", {L"右 Alt", L"Right Alt"}}
        };
        if (const auto it = names.find(key); it != names.end())
            return english ? it->second.second : it->second.first;
        if (key.size() == 1 && std::isalnum(static_cast<unsigned char>(key[0])))
            return std::wstring(1, static_cast<wchar_t>(std::toupper(static_cast<unsigned char>(key[0]))));
        if (key.starts_with("keypad.")) return choose(english, L"小键盘 ", L"Numpad ") + readable(key.substr(7));
        return readable(key);
    }
    if (lower.starts_with("key.mouse.")) {
        const auto button = lower.substr(10);
        if (button == "left") return choose(english, L"鼠标左键", L"Left mouse button");
        if (button == "right") return choose(english, L"鼠标右键", L"Right mouse button");
        if (button == "middle") return choose(english, L"鼠标中键", L"Middle mouse button");
        return choose(english, L"鼠标按钮 ", L"Mouse button ") + utf8_to_wide(button);
    }
    if (lower.starts_with("key.scancode."))
        return choose(english, L"扫描码 ", L"Scan code ") + utf8_to_wide(value.substr(13));
    if (!value.empty() && std::all_of(value.begin(), value.end(), [](char character) {
            return character >= '0' && character <= '9';
        }))
        return choose(english, L"按键码 ", L"Key code ") + utf8_to_wide(value);
    return short_value(value);
}

MigrationExplanation explain_migration(const PlannedOperation& operation,
                                       const MigrationProfile& profile,
                                       OverwriteStrategy strategy, bool english) {
    const auto& rule = profile.rules.at(operation.rule_index);
    MigrationExplanation result;
    result.introduction = introduction(operation, rule, english);
    result.scope = scope(operation, rule, english);
    result.difference = difference(operation, english);
    result.outcome = outcome(operation, rule, strategy, english);
    if (rule.kind == RuleKind::file && !operation.is_directory &&
        (operation.status == ScanStatus::found || operation.status == ScanStatus::conflict) &&
        (strategy != OverwriteStrategy::skip || operation.status == ScanStatus::found))
        result.outcome += choose(english, L" 整份文件复制，不逐项合并。",
                                L" The whole file is copied; individual settings are not merged.");
    if (rule.builtin == BuiltinKind::options && ascii_lower(rule.category) == "video" &&
        (operation.status == ScanStatus::found || operation.status == ScanStatus::conflict))
        result.outcome += choose(english, L" 画面效果可能因电脑而异。",
                                L" Display results may vary between computers.");
    result.technical = choose(english, L"迁出路径：", L"Source path: ") + operation.source_path.wstring() +
        L"\n" + choose(english, L"迁入路径：", L"Destination path: ") + operation.target_path.wstring();
    if (!operation.detail.empty()) result.technical += L"\n\n" + user_facing_detail(operation, english);
    return result;
}

std::wstring display_operation_name(const PlannedOperation& operation, bool english) {
    if (const auto* server = std::get_if<ServerEntry>(&operation.payload))
        return utf8_to_wide(server->name);
    if (const auto* world = std::get_if<WorldEntry>(&operation.payload))
        return world->world_name + L"  ·  " + world->folder_name;
    if (!english) return utf8_to_wide(operation.name);
    static const std::unordered_map<std::string, std::wstring> names = {
        {"minecraft_bindings", L"Key bindings"}, {"minecraft_audio", L"Audio settings"},
        {"minecraft_video", L"Video settings"}, {"minecraft_servers", L"Servers"},
        {"minecraft_screenshots", L"All screenshots"},
        {"sempervirens_xaero_data", L"Xaero maps and waypoints"},
        {"sempervirens_xaero_config", L"Xaero display preferences"},
        {"sempervirens_schematics", L"Litematica blueprints"},
        {"sempervirens_itemscroller", L"Item Scroller presets"},
        {"sempervirens_chesttracker", L"Chest Tracker data"},
        {"sempervirens_inventory_profiles", L"Inventory Profiles Next preferences"},
        {"sempervirens_litematica_config", L"Litematica preferences"},
        {"sempervirens_tweakeroo_config", L"Tweakeroo preferences"},
        {"sempervirens_minihud_config", L"MiniHUD preferences"},
        {"sempervirens_itemscroller_config", L"Item Scroller settings"},
        {"sempervirens_chesttracker_config", L"Chest Tracker settings"},
        {"sempervirens_litematica_settings", L"Litematica main settings"},
        {"sempervirens_malilib_settings", L"Malilib shortcuts"},
        {"sempervirens_tweakeroo_settings", L"Tweakeroo main settings"},
        {"sempervirens_tweakermore_settings", L"Tweakermore settings"},
        {"sempervirens_jade_data", L"Jade display and favorites"},
        {"sempervirens_rei_data", L"REI favorites and layout"},
        {"sempervirens_mouse_tweaks", L"Mouse Tweaks settings"},
        {"sempervirens_screenshot_behavior", L"Screenshot behavior"},
        {"sempervirens_ingame_ime", L"In-game input settings"},
        {"sempervirens_distant_horizons_cache", L"Distant Horizons map cache"}
    };
    if (const auto it = names.find(ascii_lower(operation.id)); it != names.end()) return it->second;
    return utf8_to_wide(operation.name);
}

std::wstring display_operation_group(const PlannedOperation& operation, bool english) {
    if (!english) return utf8_to_wide(operation.group);
    static const std::unordered_map<std::string, std::wstring> groups = {
        {"个人设置", L"Personal settings"}, {"服务器列表", L"Servers"},
        {"截图", L"Screenshots"}, {"单人世界", L"Single-player worlds"},
        {"新亭泪社区专属数据", L"Xintinglei personal data"}
    };
    if (const auto it = groups.find(operation.group); it != groups.end()) return it->second;
    return utf8_to_wide(operation.group);
}

std::wstring display_profile_name(std::string_view name, bool english) {
    if (english && name == "Sempervirens-新亭泪社区专用迁移器") return L"Xintinglei community profile";
    return utf8_to_wide(name);
}

} // namespace sempervirens
