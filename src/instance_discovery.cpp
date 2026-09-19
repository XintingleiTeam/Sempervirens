#include "instance_discovery.hpp"

#include "path_safety.hpp"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <array>
#include <stdexcept>
#include <vector>

namespace sempervirens {

static void check_cancelled(std::stop_token token) {
    if (token.stop_requested()) throw std::runtime_error("Scan cancelled");
}

static bool exists_without_link(const fs::path& path) {
    std::error_code error;
    if (!fs::exists(path, error) || error) return false;
    try { return !is_reparse_point(path); }
    catch (...) { return false; }
}

static std::vector<fs::path> directories(const fs::path& path, std::stop_token token) {
    std::vector<fs::path> result;
    if (!exists_without_link(path)) return result;
    std::error_code error;
    for (fs::directory_iterator it(path, fs::directory_options::skip_permission_denied, error), end;
         it != end && !error; it.increment(error)) {
        check_cancelled(token);
        if (it->is_directory(error) && !error && exists_without_link(it->path())) result.push_back(it->path());
        error.clear();
    }
    return result;
}

static int screenshot_count(const fs::path& path, std::stop_token token,
                            const ScanProgressCallback& progress, std::wstring_view name,
                            bool english) {
    if (!exists_without_link(path)) return 0;
    std::vector<fs::path> pending{path};
    int matching = 0;
    int examined = 0;
    while (!pending.empty()) {
        check_cancelled(token);
        const auto current = pending.back();
        pending.pop_back();
        std::error_code error;
        for (fs::directory_iterator it(current, fs::directory_options::skip_permission_denied, error), end;
             it != end && !error; it.increment(error)) {
            check_cancelled(token);
            if (!exists_without_link(it->path())) continue;
            if (it->is_directory(error) && !error) {
                pending.push_back(it->path());
            } else if (it->is_regular_file(error) && !error) {
                const auto extension = it->path().extension().wstring();
                if (CompareStringOrdinal(extension.c_str(), -1, L".png", -1, TRUE) == CSTR_EQUAL ||
                    CompareStringOrdinal(extension.c_str(), -1, L".jpg", -1, TRUE) == CSTR_EQUAL ||
                    CompareStringOrdinal(extension.c_str(), -1, L".jpeg", -1, TRUE) == CSTR_EQUAL)
                    ++matching;
                if (++examined % 64 == 0 && progress)
                    progress({std::nullopt, std::wstring(name) +
                        (english ? L" · " : L" · 已检查 ") + std::to_wstring(examined) +
                        (english ? L" files checked" : L" 个文件")});
            }
            error.clear();
        }
    }
    return matching;
}

static InstanceInfo inspect(const fs::path& path, std::wstring display_name, bool isolated,
                            std::stop_token token, const ScanProgressCallback& progress,
                            bool english) {
    check_cancelled(token);
    if (progress) progress({std::nullopt,
        (english ? L"Checking instance: " : L"正在检查实例：") + display_name});
    static constexpr std::array<std::wstring_view, 9> markers = {
        L"options.txt", L"servers.dat", L"screenshots", L"saves", L"resourcepacks",
        L"shaderpacks", L"config", L"mods", L"logs"
    };
    InstanceInfo info;
    info.root = full_path(path);
    info.display_name = std::move(display_name);
    info.version_isolated = isolated;
    for (const auto marker : markers) {
        check_cancelled(token);
        std::error_code error;
        if (fs::exists(path / fs::path(marker), error)) ++info.marker_count;
        if (error) info.access_note = english ? L"Some content could not be read" : L"部分内容无法读取";
    }
    info.usable = info.marker_count >= 1;
    info.has_options = exists_without_link(path / "options.txt");
    info.has_servers = exists_without_link(path / "servers.dat");
    info.screenshot_count = screenshot_count(path / "screenshots", token, progress, info.display_name, english);
    info.world_count = static_cast<int>(directories(path / "saves", token).size());
    return info;
}

InstanceSelection discover_instances(const fs::path& selected_path, std::wstring_view default_name,
                                     std::wstring_view direct_name, std::stop_token cancellation,
                                     const ScanProgressCallback& progress, bool english) {
    check_cancelled(cancellation);
    const auto root = full_path(selected_path);
    if (!fs::is_directory(root)) throw std::runtime_error("Selected instance folder does not exist");
    if (progress) progress({std::nullopt, english ? L"Finding instances" : L"正在查找实例"});
    InstanceSelection selection{root, {}};
    const auto filename = root.filename().wstring();
    const bool minecraft = CompareStringOrdinal(filename.c_str(), -1, L".minecraft", -1, TRUE) == CSTR_EQUAL;
    auto direct = inspect(root, minecraft ? std::wstring(default_name) : std::wstring(direct_name),
                          false, cancellation, progress, english);
    if (direct.usable) selection.candidates.push_back(direct);
    for (const auto& child : directories(root / "versions", cancellation)) {
        check_cancelled(cancellation);
        auto candidate = inspect(child, child.filename().wstring(), true, cancellation, progress, english);
        if (candidate.usable) selection.candidates.push_back(std::move(candidate));
    }
    if (selection.candidates.empty() && direct.marker_count > 0) selection.candidates.push_back(std::move(direct));
    std::stable_sort(selection.candidates.begin(), selection.candidates.end(),
                     [](const InstanceInfo& a, const InstanceInfo& b) {
        if (a.version_isolated != b.version_isolated) return !a.version_isolated;
        return CompareStringEx(LOCALE_NAME_USER_DEFAULT, NORM_IGNORECASE,
                               a.display_name.c_str(), -1, b.display_name.c_str(), -1,
                               nullptr, nullptr, 0) == CSTR_LESS_THAN;
    });
    return selection;
}

} // namespace sempervirens
