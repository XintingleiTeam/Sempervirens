#include "execution.hpp"
#include "atomic_file.hpp"
#include "path_safety.hpp"
#include "preferences.hpp"
#include "profile.hpp"
#include "presentation.hpp"
#include "scan.hpp"
#include "splash.hpp"
#include "text.hpp"
#include "update.hpp"
#include "win_compat.hpp"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>
#include <d2d1.h>
#include <d2d1helper.h>
#include <dwrite.h>
#include <dwmapi.h>
#include <imm.h>
#include <shobjidl.h>
#include <shlobj.h>
#include <shellapi.h>
#include <wincodec.h>
#include <knownfolders.h>
#include <oleacc.h>
#include "branding.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cwctype>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <variant>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using namespace sempervirens;

namespace {

constexpr UINT wm_worker = WM_APP + 1;
constexpr UINT wm_worker_progress = WM_APP + 2;
constexpr UINT wm_tray = WM_APP + 3;
constexpr UINT wm_update = WM_APP + 4;
constexpr UINT_PTR animation_timer = 1;
constexpr UINT tray_open_command = 1001;
constexpr UINT tray_exit_command = 1002;
constexpr wchar_t window_class[] = L"SempervirensNativeWindow";
constexpr wchar_t automated_test_window_class[] = L"SempervirensNativeAutomatedTestWindow";
constexpr wchar_t single_instance_smoke_window_class[] = L"SempervirensNativeSingleInstanceSmokeWindow";
constexpr wchar_t project_url[] = L"https://github.com/XintingleiTeam/sempervirens";
UINT registered_taskbar_created_message = 0;

class AppAccessible;

enum class Page { welcome, migration, settings, confirmation, result, error };
enum class SettingsSection { general, migration, about };
enum class UpdatePhase { idle, checking, available, downloading, ready, current, error };
enum class ContentScrollDrag { none, list, detail, error };
enum class Action { none, settings, back, source_browse, target_browse, source_drawer,
                    target_drawer, drawer_previous, drawer_next, scan, start, strategy_backup, strategy_replace,
                    strategy_skip, settings_general, settings_migration, settings_about,
                    language, language_zh, language_en, close_prompt, close_action,
                    project_link, update_check, update_download, update_restart, update_auto,
                    profile_browse, profile_reset, profile_xintinglei, profile_minecraft,
                    welcome_continue,
                    search, list_item, list_toggle, group_toggle, drawer_item,
                    confirm_start, report, backup, technical_toggle,
                    close_remember, close_tray, close_exit, close_cancel };
enum class Job { none, discover_source, discover_target, scan, migrate };

struct UpdateOutcome {
    UpdatePhase phase = UpdatePhase::error;
    std::optional<update::Manifest> manifest;
    std::vector<std::uint8_t> manifest_bytes;
    std::vector<std::uint8_t> signature;
    fs::path staging;
    std::wstring message;
};

bool shell_launch_succeeded(INT_PTR result) {
    return result > 32;
}

std::wstring project_open_failure_message(bool english) {
    return std::wstring(english ?
        L"The browser could not be opened. Visit the project address manually:\n" :
        L"无法打开浏览器，请手动访问项目地址：\n") + project_url;
}

std::wstring friendly_worker_error(Job job, std::string_view error, bool english) {
    if (error == "迁出和迁入实例不能相同，也不能互相包含。" ||
        error == "The source and destination cannot be the same folder or contain one another." ||
        error == "迁出和迁入实例必须都是现有文件夹。" ||
        error == "The source and destination must both be existing folders." ||
        error == "扫描后迁出或迁入实例发生了变化，请重新扫描后再迁移。" ||
        error == "The source or destination instance changed after scanning. Scan again before migrating." ||
        error == "迁移信息不完整，请返回迁移页面重新扫描。" ||
        error == "The migration information is incomplete. Return to the migration page and scan again.")
        return utf8_to_wide(error);
    if (job == Job::discover_source || job == Job::discover_target)
        return english ?
            L"The selected folder could not be read. Check that it exists and that you have access." :
            L"无法读取所选文件夹，请确认路径存在并检查访问权限。";
    if (job == Job::scan)
        return english ?
            L"The scan could not finish. Check access to both instances, then try again." :
            L"扫描未能完成，请确认两个实例均可访问后重试。";
    return english ?
        L"The migration stopped safely. Check the destination instance and available disk space, then scan again." :
        L"迁移已安全停止，请检查迁入实例和磁盘空间，然后重新扫描。";
}

struct WorkerOutcome {
    Job job = Job::none;
    std::variant<std::monostate, InstanceSelection, MigrationPlan, MigrationResult> value;
    std::string error;
    bool cancelled = false;
};

struct Hit {
    D2D1_RECT_F rectangle{};
    Action action = Action::none;
    int index = -1;
};

struct SearchEditState {
    std::wstring text;
    std::size_t caret = 0;
    std::optional<std::size_t> anchor;
};

template <typename T>
void release(T*& value) {
    if (value) { value->Release(); value = nullptr; }
}

D2D1_COLOR_F color(unsigned hex, float alpha = 1.0f) {
    return D2D1::ColorF(hex, alpha);
}

bool contains(const D2D1_RECT_F& rect, float x, float y) {
    return x >= rect.left && x < rect.right && y >= rect.top && y < rect.bottom;
}

bool search_whitespace(wchar_t character) {
    return (character >= L'\t' && character <= L'\r') || character == L' ' ||
           character == 0x0085 || character == 0x00A0 || character == 0x1680 ||
           (character >= 0x2000 && character <= 0x200A) ||
           character == 0x2028 || character == 0x2029 || character == 0x202F ||
           character == 0x205F || character == 0x3000;
}

std::wstring_view trim_search(std::wstring_view value) {
    while (!value.empty() && search_whitespace(value.front())) value.remove_prefix(1);
    while (!value.empty() && search_whitespace(value.back())) value.remove_suffix(1);
    return value;
}

bool current_culture_contains(std::wstring_view text, std::wstring_view query) {
    if (query.empty()) return true;
    if (text.empty() || text.size() > static_cast<std::size_t>(INT_MAX) ||
        query.size() > static_cast<std::size_t>(INT_MAX)) return false;
    return FindNLSStringEx(LOCALE_NAME_USER_DEFAULT,
        FIND_FROMSTART | LINGUISTIC_IGNORECASE,
        text.data(), static_cast<int>(text.size()),
        query.data(), static_cast<int>(query.size()), nullptr, nullptr, nullptr, 0) >= 0;
}

bool current_culture_equal(std::wstring_view left, std::wstring_view right) {
    if (left.size() > static_cast<std::size_t>(INT_MAX) ||
        right.size() > static_cast<std::size_t>(INT_MAX)) return false;
    return CompareStringEx(LOCALE_NAME_USER_DEFAULT, NORM_IGNORECASE,
        left.data(), static_cast<int>(left.size()), right.data(), static_cast<int>(right.size()),
        nullptr, nullptr, 0) == CSTR_EQUAL;
}

bool selectable(ScanStatus status) {
    return status == ScanStatus::found || status == ScanStatus::identical ||
           status == ScanStatus::conflict;
}

bool high_surrogate(wchar_t character) {
    return character >= 0xD800 && character <= 0xDBFF;
}

bool low_surrogate(wchar_t character) {
    return character >= 0xDC00 && character <= 0xDFFF;
}

D2D1_RECT_F rect(float x, float y, float width, float height) {
    return D2D1::RectF(x, y, x + width, y + height);
}

fs::path application_directory() {
    std::wstring buffer(MAX_PATH, L'\0');
    for (;;) {
        const auto length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0) throw std::runtime_error("Cannot find application directory");
        if (length < buffer.size()) { buffer.resize(length); return fs::path(buffer).parent_path(); }
        buffer.resize(buffer.size() * 2);
    }
}

fs::path application_path() {
    std::wstring buffer(MAX_PATH, L'\0');
    for (;;) {
        const auto length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0) throw std::runtime_error("Cannot find application path");
        if (length < buffer.size()) { buffer.resize(length); return fs::path(buffer); }
        buffer.resize(buffer.size() * 2);
    }
}

fs::path settings_path(bool current) {
    PWSTR roaming = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &roaming))) return {};
    const fs::path result = current ?
        fs::path(roaming) / "XintingleiTeam" / "Sempervirens" / "settings.json" :
        fs::path(roaming) / "Sempervirens" / "settings.json";
    CoTaskMemFree(roaming);
    return result;
}

fs::path preview_settings_path() {
    PWSTR roaming = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &roaming))) return {};
    const fs::path result = fs::path(roaming) / "SempervirensCppPreview" / "settings.json";
    CoTaskMemFree(roaming);
    return result;
}

std::optional<bool> installer_language() {
    wchar_t language[64]{};
    DWORD bytes = sizeof(language);
    auto result = RegGetValueW(HKEY_CURRENT_USER,
        L"Software\\XintingleiTeam\\Sempervirens", L"InstallerLanguage",
        RRF_RT_REG_SZ, nullptr, language, &bytes);
    if (result != ERROR_SUCCESS) {
        bytes = sizeof(language);
        result = RegGetValueW(HKEY_CURRENT_USER,
            L"Software\\XintingleiTeam\\SempervirensCppPreview", L"InstallerLanguage",
            RRF_RT_REG_SZ, nullptr, language, &bytes);
    }
    if (result != ERROR_SUCCESS) return std::nullopt;
    if (_wcsicmp(language, L"english") == 0) return true;
    if (_wcsicmp(language, L"chinesesimplified") == 0) return false;
    return std::nullopt;
}

std::optional<std::wstring> explorer_arguments(const fs::path& path) {
    std::error_code error;
    if (fs::is_regular_file(path, error)) return L"/select,\"" + path.wstring() + L"\"";
    error.clear();
    if (fs::is_directory(path, error)) return L"\"" + path.wstring() + L"\"";
    return std::nullopt;
}

bool open_in_explorer(HWND owner, const fs::path& path) {
    const auto arguments = explorer_arguments(path);
    if (!arguments) return false;
    const auto result = ShellExecuteW(owner, L"open", L"explorer.exe", arguments->c_str(),
                                      nullptr, SW_SHOWNORMAL);
    return reinterpret_cast<INT_PTR>(result) > 32;
}

class App {
public:
    explicit App(HINSTANCE instance, std::optional<fs::path> settings_override = {})
        : instance_(instance), settings_override_(std::move(settings_override)) {}
    ~App() {
        if (worker_.joinable()) { worker_.request_stop(); worker_.join(); }
        if (update_worker_.joinable()) { update_worker_.request_stop(); update_worker_.join(); }
        remove_tray();
        release(target_);
        release(brush_);
        release(brand_format_);
        release(title_format_);
        release(body_format_);
        release(button_format_);
        release(caption_format_);
        release(search_format_);
        release(control_label_format_);
        release(ellipsis_sign_);
        release(mono_single_format_);
        release(mono_format_);
        release(write_factory_);
        release(d2d_factory_);
        release(accessibility_);
    }

    bool initialize(HWND hwnd) {
        hwnd_ = hwnd;
        load_startup_language();
        try {
            profile_ = load_default_profile(application_directory());
        } catch (const std::exception&) {
            const std::wstring message = english_ ?
                L"Sempervirens could not start:\nThe default migration profile could not be read. Check the installation files or reinstall the app." :
                L"Sempervirens 无法启动：\n默认迁移规则无法读取。请检查安装文件或重新安装程序。";
            MessageBoxW(hwnd_, message.c_str(), L"Sempervirens", MB_OK | MB_ICONERROR);
            return false;
        }
        load_settings();
        if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &d2d_factory_))) return false;
        if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                       reinterpret_cast<IUnknown**>(&write_factory_)))) return false;
        if (FAILED(write_factory_->CreateTextFormat(L"Bahnschrift SemiBold", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
                 DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 32, L"zh-CN", &brand_format_))) return false;
        if (FAILED(write_factory_->CreateTextFormat(L"Microsoft YaHei UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
                 DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 18, L"zh-CN", &title_format_))) return false;
        if (FAILED(write_factory_->CreateTextFormat(L"Microsoft YaHei UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                 DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 14, L"zh-CN", &body_format_))) return false;
        if (FAILED(write_factory_->CreateTextFormat(L"Microsoft YaHei UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
                 DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 14, L"zh-CN", &button_format_))) return false;
        button_format_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        button_format_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        if (FAILED(write_factory_->CreateTextFormat(L"Microsoft YaHei UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                 DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 12, L"zh-CN", &caption_format_))) return false;
        if (FAILED(write_factory_->CreateTextFormat(L"Microsoft YaHei UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                 DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 13, L"zh-CN", &search_format_))) return false;
        search_format_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        if (FAILED(write_factory_->CreateTextFormat(L"Microsoft YaHei UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                 DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 14, L"zh-CN", &control_label_format_))) return false;
        control_label_format_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        if (FAILED(write_factory_->CreateTextFormat(L"Cascadia Mono", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                 DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 12, L"zh-CN", &mono_format_))) return false;
        if (FAILED(write_factory_->CreateTextFormat(L"Cascadia Mono", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                 DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 12, L"zh-CN", &mono_single_format_))) return false;
        mono_single_format_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
        if (FAILED(write_factory_->CreateEllipsisTrimmingSign(mono_single_format_, &ellipsis_sign_))) return false;
        if (FAILED(mono_single_format_->SetTrimming(&trimming, ellipsis_sign_))) return false;
        const BOOL dark = TRUE;
        DwmSetWindowAttribute(hwnd_, 20, &dark, sizeof(dark));
        const DWORD rounded = 2;
        DwmSetWindowAttribute(hwnd_, 33, &rounded, sizeof(rounded));
        const DWORD main_window_backdrop = 2;
        const auto backdrop_result = DwmSetWindowAttribute(hwnd_, 38, &main_window_backdrop,
                                                            sizeof(main_window_backdrop));
        if (SUCCEEDED(backdrop_result)) {
            DWORD applied_backdrop = 0;
            if (SUCCEEDED(DwmGetWindowAttribute(hwnd_, 38, &applied_backdrop,
                                                sizeof(applied_backdrop))))
                system_backdrop_active_ = applied_backdrop == main_window_backdrop;
        }
        BOOL animation = TRUE;
        SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &animation, 0);
        reduced_motion_ = animation == FALSE;
        DragAcceptFiles(hwnd_, TRUE);
        return true;
    }

    fs::path selected_settings_source() const {
        const auto current = settings_override_ ? *settings_override_ : settings_path(true);
        if (settings_override_) return current;
        if (!current.empty() && fs::exists(current)) return current;
        const auto preview = preview_settings_path();
        return !preview.empty() && fs::exists(preview) ? preview : settings_path(false);
    }

    void load_startup_language() {
        try {
            if (settings_override_) {
                if (fs::exists(*settings_override_))
                    if (const auto preferences = read_preferences(*settings_override_))
                        english_ = preferences->english;
                return;
            }
            const auto current = settings_path(true);
            if (!current.empty() && fs::exists(current)) {
                if (const auto preferences = read_preferences(current)) english_ = preferences->english;
                return;
            }
            const auto preview = preview_settings_path();
            if (!preview.empty() && fs::exists(preview)) {
                if (const auto preferences = read_preferences(preview)) english_ = preferences->english;
                return;
            }
            if (const auto installed = installer_language()) {
                english_ = *installed;
                return;
            }
            const auto legacy = settings_path(false);
            if (!legacy.empty() && fs::exists(legacy))
                if (const auto preferences = read_preferences(legacy)) english_ = preferences->english;
        } catch (...) { }
    }

    void load_settings() {
        try {
            const auto preview = settings_override_ ? *settings_override_ : settings_path(true);
            const auto old_preview = settings_override_ ? fs::path{} : preview_settings_path();
            const auto legacy = settings_override_ ? fs::path{} : settings_path(false);
            const bool first_launch = !settings_override_ &&
                (preview.empty() || !fs::exists(preview)) &&
                (old_preview.empty() || !fs::exists(old_preview)) &&
                (legacy.empty() || !fs::exists(legacy));
            const bool startup_english = english_;
            const auto source = selected_settings_source();
            if (source.empty() || !fs::exists(source)) {
                if (first_launch) begin_onboarding();
                return;
            }
            const auto preferences = read_preferences(source);
            if (!preferences) {
                status_ = tr(L"保存的设置无法读取，已使用默认值。", L"Saved settings could not be read; defaults are in use.");
                return;
            }
            english_ = preferences->english;
            prompt_on_close_ = preferences->prompt_on_close;
            close_to_tray_ = preferences->close_to_tray;
            auto_check_updates_ = preferences->auto_check_updates;
            use_minecraft_profile_ = preferences->use_minecraft_profile;
            if (preferences->migration_profile_path) {
                try {
                    profile_ = load_profile(*preferences->migration_profile_path);
                    profile_file_ = preferences->migration_profile_path;
                } catch (...) {
                    profile_file_.reset();
                    use_minecraft_profile_ = false;
                    status_ = tr(L"保存的规则文件不可用，已使用默认规则。",
                                 L"Saved rules are unavailable; default rules are in use.");
                    if (!first_launch) save_settings();
                }
            } else if (use_minecraft_profile_) profile_ = load_builtin_profile(BuiltInProfile::minecraft);
            if (first_launch) {
                if (installer_language()) english_ = startup_english;
                begin_onboarding();
            } else if (!settings_override_ && source != preview) save_settings();
        } catch (...) {
            english_ = false;
            prompt_on_close_ = true;
            close_to_tray_ = false;
            auto_check_updates_ = true;
            use_minecraft_profile_ = false;
            profile_file_.reset();
            status_ = tr(L"保存的设置无法读取，已使用默认值。", L"Saved settings could not be read; defaults are in use.");
            if (!settings_override_) {
                const auto preview = settings_path(true);
                if (preview.empty() || !fs::exists(preview)) begin_onboarding();
            }
        }
    }

    void begin_onboarding() {
        profile_ = load_builtin_profile(BuiltInProfile::minecraft);
        profile_file_.reset();
        use_minecraft_profile_ = true;
        onboarding_ = true;
        page_ = Page::welcome;
        status_.clear();
    }

    void save_settings() {
        try {
            const auto path = settings_override_ ? *settings_override_ : settings_path(true);
            if (path.empty()) return;
            write_preferences(path, {english_, prompt_on_close_, close_to_tray_, profile_file_,
                                      use_minecraft_profile_, auto_check_updates_});
        } catch (...) { status_ = tr(L"设置未能保存。", L"Settings could not be saved."); }
    }

    bool installed_update_layout() const {
        try {
            const auto root = update::installation_root(application_directory());
            return root != application_directory() &&
                   fs::is_regular_file(root / L"Sempervirens.exe") &&
                   fs::is_regular_file(root / L"SempervirensUpdater.exe");
        } catch (...) { return false; }
    }

    void publish_update(UpdateOutcome outcome) {
        {
            std::scoped_lock lock(update_mutex_);
            pending_update_outcome_ = std::move(outcome);
        }
        PostMessageW(hwnd_, wm_update, 0, 0);
    }

    void start_update_check(bool manual) {
        if (update_phase_ == UpdatePhase::checking || update_phase_ == UpdatePhase::downloading) return;
        if (update_worker_.joinable()) update_worker_.join();
        update_phase_ = UpdatePhase::checking;
        update_message_ = manual ? tr(L"正在检查更新…", L"Checking for updates…") : L"";
        update_manual_check_ = manual;
        update_progress_value_ = -1;
        update_worker_ = std::jthread([this](std::stop_token stop) {
            UpdateOutcome outcome;
            std::wstring last_error;
            for (const auto& source : update::stable_sources) {
                try {
                    auto manifest = update::download_https(source.manifest_url, 128 * 1024, stop);
                    auto signature = update::download_https(source.signature_url, 1024, stop);
                    const std::string_view text(reinterpret_cast<const char*>(manifest.data()), manifest.size());
                    if (!update::verify_manifest_signature(text, signature))
                        throw std::runtime_error("Update manifest signature is invalid");
                    auto parsed = update::parse_manifest(text, "stable");
                    outcome.phase = update::compare_versions(parsed.version, update::current_version) > 0 ?
                        UpdatePhase::available : UpdatePhase::current;
                    outcome.manifest = std::move(parsed);
                    outcome.manifest_bytes = std::move(manifest);
                    outcome.signature = std::move(signature);
                    publish_update(std::move(outcome));
                    return;
                } catch (const std::exception& error) {
                    last_error = utf8_to_wide(error.what());
                    if (stop.stop_requested()) return;
                }
            }
            outcome.phase = UpdatePhase::error;
            outcome.message = std::move(last_error);
            publish_update(std::move(outcome));
        });
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    void start_automatic_update_check() {
        if (auto_check_updates_ && installed_update_layout()) start_update_check(false);
    }

    void start_update_download() {
        if (update_phase_ != UpdatePhase::available || !update_manifest_ ||
            update_manifest_bytes_.empty() || update_signature_.empty() || !installed_update_layout()) return;
        if (update_worker_.joinable()) update_worker_.join();
        update_phase_ = UpdatePhase::downloading;
        update_message_ = tr(L"正在准备更新…", L"Preparing update…");
        update_progress_value_ = 0;
        const auto manifest = *update_manifest_;
        const auto manifest_bytes = update_manifest_bytes_;
        const auto signature = update_signature_;
        const auto current_app = application_path();
        update_expected_download_ = 1;
        for (const auto& package : manifest.packages) {
            if ((package.kind == "delta" && package.from_version == update::current_version) || package.kind == "full") {
                update_expected_download_ = std::max<std::uint64_t>(1, package.size);
                if (package.kind == "delta") break;
            }
        }
        update_worker_ = std::jthread([this, manifest, manifest_bytes, signature, current_app](std::stop_token stop) {
            UpdateOutcome outcome;
            try {
                const auto current_hash = update::sha256_file(current_app);
                const update::Package* selected = nullptr;
                for (const auto& package : manifest.packages) {
                    if (package.kind == "delta" && package.from_version == update::current_version &&
                        package.from_sha256 == current_hash) { selected = &package; break; }
                }
                if (!selected) for (const auto& package : manifest.packages)
                    if (package.kind == "full") { selected = &package; break; }
                if (!selected) throw std::runtime_error("No compatible update package was published");
                std::vector<std::uint8_t> bytes;
                std::string last_error;
                for (const auto& url : selected->urls) try {
                    bytes = update::download_https(utf8_to_wide(url), static_cast<std::size_t>(selected->size), stop,
                        [this](std::uint64_t current, std::uint64_t) {
                            update_progress_value_ = static_cast<int>(std::min<std::uint64_t>(99,
                                current * 100 / std::max<std::uint64_t>(1, update_expected_download_)));
                            PostMessageW(hwnd_, wm_update, 1, 0);
                        });
                    if (bytes.size() != selected->size ||
                        update::sha256_bytes(bytes.data(), bytes.size()) != selected->sha256)
                        throw std::runtime_error("Downloaded package failed verification");
                    break;
                } catch (const std::exception& error) {
                    bytes.clear(); last_error = error.what();
                    if (stop.stop_requested()) throw;
                }
                if (bytes.empty()) throw std::runtime_error(last_error.empty() ?
                    "All update mirrors failed" : last_error);
                const auto staging_root = update::update_data_root() / L"staging";
                const auto staging = staging_root / utf8_to_wide(manifest.version);
                if (!update::safe_version(manifest.version) || staging.parent_path() != staging_root)
                    throw std::runtime_error("Unsafe staging path");
                std::error_code cleanup_error;
                fs::remove_all(staging, cleanup_error);
                if (cleanup_error) throw std::runtime_error("Old update staging could not be cleared");
                fs::create_directories(staging);
                const auto output = staging / L"SempervirensApp.exe";
                if (selected->kind == "delta")
                    update::apply_delta(current_app, bytes, output, manifest.output_sha256,
                                        manifest.output_size, stop);
                else update::write_verified_file(bytes, output, manifest.output_sha256, manifest.output_size);
                write_file_atomically(staging / L"manifest.json", std::string_view(
                    reinterpret_cast<const char*>(manifest_bytes.data()), manifest_bytes.size()));
                write_file_atomically(staging / L"manifest.json.sig", std::string_view(
                    reinterpret_cast<const char*>(signature.data()), signature.size()));
                outcome.phase = UpdatePhase::ready;
                outcome.manifest = manifest;
                outcome.staging = staging;
            } catch (const std::exception& error) {
                outcome.phase = UpdatePhase::error;
                outcome.message = utf8_to_wide(error.what());
            }
            publish_update(std::move(outcome));
        });
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    void update_complete(WPARAM progress_only = 0) {
        if (progress_only) { InvalidateRect(hwnd_, nullptr, FALSE); return; }
        std::optional<UpdateOutcome> outcome;
        {
            std::scoped_lock lock(update_mutex_);
            outcome = std::move(pending_update_outcome_);
            pending_update_outcome_.reset();
        }
        if (update_worker_.joinable()) update_worker_.join();
        if (!outcome) return;
        update_phase_ = outcome->phase;
        update_manifest_ = std::move(outcome->manifest);
        if (!outcome->manifest_bytes.empty()) update_manifest_bytes_ = std::move(outcome->manifest_bytes);
        if (!outcome->signature.empty()) update_signature_ = std::move(outcome->signature);
        if (!outcome->staging.empty()) update_staging_ = std::move(outcome->staging);
        if (update_phase_ == UpdatePhase::available)
            update_message_ = tr(L"发现新版本 ", L"Version ") + utf8_to_wide(update_manifest_->version) +
                tr(L"，可在应用内更新。", L" is available.");
        else if (update_phase_ == UpdatePhase::current)
            update_message_ = update_manual_check_ ? tr(L"当前已是最新版本。", L"You're up to date.") : L"";
        else if (update_phase_ == UpdatePhase::ready) {
            update_progress_value_ = 100;
            update_message_ = tr(L"更新已准备好，重启后自动完成。", L"Update is ready and will finish after restart.");
        } else if (update_phase_ == UpdatePhase::error)
            update_message_ = tr(L"暂时无法获取更新，请稍后重试。", L"Updates are temporarily unavailable. Try again later.");
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    void restart_to_update() {
        if (update_phase_ != UpdatePhase::ready || !update_manifest_ || update_staging_.empty()) return;
        GUID guid{};
        if (FAILED(CoCreateGuid(&guid))) return;
        wchar_t guid_text[64]{};
        StringFromGUID2(guid, guid_text, 64);
        std::wstring token;
        for (const auto value : std::wstring_view(guid_text))
            if (iswalnum(value)) token.push_back(value);
        const auto root = update::installation_root(application_directory());
        const auto updater = root / L"SempervirensUpdater.exe";
        auto quote = [](const std::wstring& value) { return L"\"" + value + L"\""; };
        std::wstring command = quote(updater.wstring()) + L" --commit " + quote(update_staging_.wstring()) +
            L" " + quote(utf8_to_wide(update_manifest_->version)) + L" " +
            std::to_wstring(GetCurrentProcessId()) + L" " + token;
        STARTUPINFOW startup{sizeof(startup)};
        PROCESS_INFORMATION process{};
        if (CreateProcessW(updater.c_str(), command.data(), nullptr, nullptr, FALSE,
                           CREATE_NO_WINDOW, nullptr, root.c_str(), &startup, &process)) {
            CloseHandle(process.hThread); CloseHandle(process.hProcess);
            exit_now();
        } else update_message_ = tr(L"无法启动更新程序。", L"The updater could not be started.");
    }

    std::optional<fs::path> pick_folder() {
        IFileOpenDialog* dialog = nullptr;
        if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                    IID_PPV_ARGS(&dialog)))) return std::nullopt;
        DWORD options = 0;
        dialog->GetOptions(&options);
        dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
        const auto title = tr(L"选择 Minecraft 实例文件夹", L"Choose a Minecraft instance folder");
        dialog->SetTitle(title.c_str());
        std::optional<fs::path> selected;
        if (SUCCEEDED(dialog->Show(hwnd_))) {
            IShellItem* item = nullptr;
            if (SUCCEEDED(dialog->GetResult(&item))) {
                PWSTR path = nullptr;
                if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                    selected = fs::path(path);
                    CoTaskMemFree(path);
                }
                item->Release();
            }
        }
        dialog->Release();
        return selected;
    }

    std::optional<fs::path> pick_profile_file() {
        IFileOpenDialog* dialog = nullptr;
        if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                    IID_PPV_ARGS(&dialog)))) return std::nullopt;
        DWORD options = 0;
        dialog->GetOptions(&options);
        dialog->SetOptions(options | FOS_FILEMUSTEXIST | FOS_FORCEFILESYSTEM);
        const auto title = tr(L"选择迁移规则文件", L"Choose a migration profile");
        dialog->SetTitle(title.c_str());
        dialog->SetDefaultExtension(L"json");
        const COMDLG_FILTERSPEC filters[] = {{L"JSON", L"*.json"}};
        dialog->SetFileTypes(1, filters);
        std::optional<fs::path> selected;
        if (SUCCEEDED(dialog->Show(hwnd_))) {
            IShellItem* item = nullptr;
            if (SUCCEEDED(dialog->GetResult(&item))) {
                PWSTR path = nullptr;
                if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                    selected = fs::path(path);
                    CoTaskMemFree(path);
                }
                item->Release();
            }
        }
        dialog->Release();
        return selected;
    }

    void select_profile() {
        if (const auto path = pick_profile_file()) try {
            auto loaded = load_profile(*path);
            profile_ = std::move(loaded);
            profile_file_ = *path;
            use_minecraft_profile_ = false;
            plan_.reset();
            profile_changed_in_settings_ = true;
            status_ = onboarding_ ?
                tr(L"已选择自定义规则。", L"Custom rules selected.") :
                tr(L"迁移规则已切换，返回后会自动重新扫描。",
                   L"Migration rules changed. Returning will scan again automatically.");
            if (!onboarding_) save_settings();
        } catch (const std::exception&) {
            status_ = tr(L"无法读取这个迁移规则文件，请确认文件格式正确后重试。",
                         L"This migration profile could not be read. Check its format and try again.");
        }
    }

    void reset_profile() {
        try {
            profile_ = load_default_profile(application_directory());
            profile_file_.reset();
            use_minecraft_profile_ = false;
            plan_.reset();
            profile_changed_in_settings_ = true;
            status_ = tr(L"已恢复默认迁移规则，返回后会自动重新扫描。",
                         L"Default migration rules restored. Returning will scan again automatically.");
            if (!onboarding_) save_settings();
        } catch (const std::exception&) {
            status_ = tr(L"无法恢复默认迁移规则，请重新安装应用后重试。",
                         L"The default migration profile could not be restored. Reinstall the app and try again.");
        }
    }

    void select_builtin_profile(BuiltInProfile selected) {
        try {
            profile_ = load_builtin_profile(selected);
            profile_file_.reset();
            use_minecraft_profile_ = selected == BuiltInProfile::minecraft;
            plan_.reset();
            profile_changed_in_settings_ = true;
            status_ = onboarding_ ?
                (use_minecraft_profile_ ? tr(L"已选择仅原版规则。", L"Vanilla-only rules selected.") :
                                          tr(L"已选择新亭泪规则。", L"Xintinglei rules selected.")) :
                (use_minecraft_profile_ ?
                    tr(L"已切换到原版 Minecraft 规则，返回后会自动重新扫描。",
                       L"Minecraft rules selected. Returning will scan again automatically.") :
                    tr(L"已切换到新亭泪规则，返回后会自动重新扫描。",
                       L"Xintinglei rules selected. Returning will scan again automatically."));
            if (!onboarding_) save_settings();
        } catch (const std::exception&) {
            status_ = tr(L"内置迁移规则无法读取，请重新安装应用。",
                         L"Built-in migration rules could not be read. Reinstall the app.");
        }
    }

    void leave_settings() {
        const bool rescan = page_ == Page::settings && profile_changed_in_settings_;
        profile_changed_in_settings_ = false;
        page_ = Page::migration;
        start_page_animation(-1);
        if (rescan && source_path() && target_path() && profile_) start_job(Job::scan);
    }

    std::optional<fs::path> source_path() const {
        if (!source_selection_ || source_index_ < 0 ||
            source_index_ >= static_cast<int>(source_selection_->candidates.size())) return std::nullopt;
        return source_selection_->candidates[source_index_].root;
    }
    std::optional<fs::path> target_path() const {
        if (!target_selection_ || target_index_ < 0 ||
            target_index_ >= static_cast<int>(target_selection_->candidates.size())) return std::nullopt;
        return target_selection_->candidates[target_index_].root;
    }

    void start_discovery(bool source) {
        const auto path = pick_folder();
        if (!path) return;
        start_job(source ? Job::discover_source : Job::discover_target, *path);
    }

    void drop_files(HDROP drop) {
        POINT point{};
        const auto inside_client = DragQueryPoint(drop, &point) != FALSE;
        const auto count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
        std::optional<fs::path> folder;
        if (count >= 1) {
            const auto length = DragQueryFileW(drop, 0, nullptr, 0);
            if (length > 0 && length < 32768) {
                std::wstring value(length + 1, L'\0');
                value.resize(DragQueryFileW(drop, 0, value.data(), length + 1));
                folder = fs::path(value);
            }
        }
        DragFinish(drop);
        if (busy_ || page_ != Page::migration || !inside_client) return;
        RECT client{};
        GetClientRect(hwnd_, &client);
        const auto width = (client.right - client.left) / scale();
        const auto x = point.x / scale();
        const auto y = point.y / scale();
        const auto card_width = (width - 26 * 2 - 16) / 2;
        const bool source = contains(rect(26, 125, card_width, 176), x, y);
        const bool target = contains(rect(42 + card_width, 125, card_width, 176), x, y);
        if (!source && !target) return;
        std::error_code error;
        if (!folder || !fs::is_directory(*folder, error) || error) {
            status_ = tr(L"请拖入实例文件夹。", L"Drop an instance folder.");
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }
        source_drawer_open_ = false;
        target_drawer_open_ = false;
        start_job(source ? Job::discover_source : Job::discover_target, *folder);
    }

    bool discovery_started_for_smoke(bool source, const fs::path& folder = {}) const {
        return busy_ && job_ == (source ? Job::discover_source : Job::discover_target) &&
            (folder.empty() || active_discovery_folder_ == folder);
    }

    bool startup_english() const { return english_; }
    bool startup_motion_enabled() const { return !reduced_motion_; }
    int smoke_installer_language() const {
        const auto installed = installer_language();
        if (!installed || !*installed) return 2;
        const auto preview = settings_path(true);
        if (!preview.empty() && fs::exists(preview)) {
            const auto saved = read_preferences(preview);
            return saved && english_ == saved->english ? 0 : 3;
        }
        return english_ ? 0 : 4;
    }
    void set_single_instance_smoke(bool enabled) { single_instance_smoke_ = enabled; }
    void prepare_automated_test(bool preserve_language) {
        if (!preserve_language) english_ = false;
        if (onboarding_) {
            onboarding_ = false;
            page_ = Page::migration;
        }
        status_.clear();
    }
    LRESULT accessibility_result(WPARAM client_request);
    int smoke_accessibility();
    int smoke_accessibility_pages();
    int smoke_instance_card_behavior();
    int smoke_pointer_feedback();
    int smoke_drawer_scrollbar_and_deactivation();
    int smoke_session_end(bool destroy);
    int smoke_dpi_change();
    int smoke_content_scrollbars();

    int smoke_single_instance() {
        wchar_t executable[32768]{};
        const auto length = GetModuleFileNameW(nullptr, executable, std::size(executable));
        if (length == 0 || length >= std::size(executable)) return 2;
        const auto launch_second = [&]() {
            std::wstring command = L"\"" + std::wstring(executable, length) +
                                   L"\" --smoke-single-instance-child";
            STARTUPINFOW startup{};
            startup.cb = sizeof(startup);
            startup.dwFlags = STARTF_USESHOWWINDOW;
            startup.wShowWindow = SW_HIDE;
            PROCESS_INFORMATION child{};
            if (!CreateProcessW(executable, command.data(), nullptr, nullptr, FALSE,
                                CREATE_NO_WINDOW, nullptr, nullptr, &startup, &child)) return false;
            const auto wait = WaitForSingleObject(child.hProcess, 10000);
            if (wait == WAIT_TIMEOUT) {
                TerminateProcess(child.hProcess, 4);
                WaitForSingleObject(child.hProcess, 5000);
            }
            DWORD exit_code = 0;
            const bool exited = wait == WAIT_OBJECT_0 &&
                GetExitCodeProcess(child.hProcess, &exit_code) && exit_code == 0;
            CloseHandle(child.hThread);
            CloseHandle(child.hProcess);
            if (!exited) return false;
            MSG message{};
            while (PeekMessageW(&message, hwnd_, 0, 0, PM_REMOVE)) DispatchMessageW(&message);
            return true;
        };
        if (!launch_second()) return 3;
        if (single_instance_activations_ != 1 || !IsWindowVisible(hwnd_)) return 4;
        ShowWindow(hwnd_, SW_HIDE);
        set_startup_splash_active(true);
        if (!launch_second()) return 5;
        if (single_instance_activations_ != 2 || !pending_activation_after_splash_ ||
            IsWindowVisible(hwnd_)) return 6;
        set_startup_splash_active(false);
        const bool restored = IsWindowVisible(hwnd_) && !pending_activation_after_splash_;
        ShowWindow(hwnd_, SW_HIDE);
        return restored ? 0 : 7;
    }

    int smoke_tray_lifecycle() {
        prompt_on_close_ = false;
        close_to_tray_ = true;
        close_requested();
        if (!tray_added_ || IsWindowVisible(hwnd_)) return 2;
        remove_tray();
        tray_added_ = true;
        if (!registered_taskbar_created_message) return 3;
        SendMessageW(hwnd_, registered_taskbar_created_message, 0, 0);
        if (!tray_added_ || IsWindowVisible(hwnd_)) return 4;
        tray_message(WM_LBUTTONDBLCLK);
        const bool restored = !tray_added_ && IsWindowVisible(hwnd_);
        ShowWindow(hwnd_, SW_HIDE);
        return restored ? 0 : 5;
    }
    void set_startup_splash_active(bool active) {
        startup_splash_active_ = active;
        if (!active && pending_activation_after_splash_) {
            pending_activation_after_splash_ = false;
            restore_from_tray();
        }
    }

    void publish_progress(const ScanProgress& value) {
        bool notify = false;
        {
            std::scoped_lock lock(worker_mutex_);
            pending_progress_ = value;
            if (!progress_message_queued_) {
                progress_message_queued_ = true;
                notify = true;
            }
        }
        if (notify && !PostMessageW(hwnd_, wm_worker_progress, 0, 0)) {
            std::scoped_lock lock(worker_mutex_);
            progress_message_queued_ = false;
        }
    }

    int smoke_progress_coalescing() {
        busy_ = true;
        set_resizing(true);
        for (int index = 0; index < 10000; ++index)
            publish_progress({index % 100, index == 9999 ? L"Latest progress" : L"Scanning"});
        MSG message{};
        int queued = 0;
        while (PeekMessageW(&message, hwnd_, wm_worker_progress, wm_worker_progress, PM_REMOVE)) ++queued;
        worker_progress();
        set_resizing(false);
        busy_ = false;
        if (queued != 1) return 2;
        if (progress_percent_ != 99 || status_ != L"Latest progress") return 3;
        if (resizing_) return 4;
        return 0;
    }

    int smoke_scan_cancellation() {
        if (!profile_) return 2;
        const auto source_root = application_directory();
        const auto target_root = fs::temp_directory_path();
        InstanceInfo source;
        source.root = source_root;
        InstanceInfo target;
        target.root = target_root;
        source_selection_ = InstanceSelection{source_root, {source}};
        target_selection_ = InstanceSelection{target_root, {target}};
        source_index_ = target_index_ = 0;
        start_job(Job::scan);
        if (!busy_ || job_ != Job::scan) return 3;
        cancel_read_only_job();
        if (!scan_cancel_requested_ || !worker_.get_stop_token().stop_requested()) return 4;
        worker_.join();
        worker_complete();
        MSG message{};
        while (PeekMessageW(&message, hwnd_, wm_worker_progress, wm_worker_progress, PM_REMOVE))
            worker_progress();
        if (busy_ || job_ != Job::none || plan_ || progress_percent_) return 5;
        return status_ == tr(L"扫描已取消。", L"Scan cancelled.") ? 0 : 6;
    }

    int smoke_close_during_scan() {
        if (!profile_) return 2;
        const auto source_root = application_directory();
        const auto target_root = fs::temp_directory_path();
        InstanceInfo source;
        source.root = source_root;
        InstanceInfo target;
        target.root = target_root;
        source_selection_ = InstanceSelection{source_root, {source}};
        target_selection_ = InstanceSelection{target_root, {target}};
        source_index_ = target_index_ = 0;
        prompt_on_close_ = true;
        close_to_tray_ = true;
        start_job(Job::scan);
        if (!busy_ || job_ != Job::scan) return 3;
        SendMessageW(hwnd_, WM_CLOSE, 0, 0);
        if (!IsWindow(hwnd_) || close_dialog_open_ || !close_after_read_only_job_ ||
            !scan_cancel_requested_ || !worker_.get_stop_token().stop_requested() ||
            status_ != tr(L"正在取消扫描并退出…", L"Cancelling scan and closing…")) return 4;
        worker_.join();
        worker_complete();
        paint();
        if (!IsWindow(hwnd_) || busy_ || job_ != Job::none || close_after_read_only_job_ ||
            !close_dialog_open_ || status_ != tr(L"扫描已取消。", L"Scan cancelled.")) return 5;
        const auto cancel = std::find_if(hits_.begin(), hits_.end(), [](const Hit& hit) {
            return hit.action == Action::close_cancel;
        });
        if (cancel == hits_.end()) return 6;
        const auto cancel_bounds = cancel->rectangle;
        click((cancel_bounds.left + cancel_bounds.right) / 2,
              (cancel_bounds.top + cancel_bounds.bottom) / 2);
        if (close_dialog_open_ || !IsWindow(hwnd_)) return 7;

        prompt_on_close_ = false;
        close_to_tray_ = true;
        start_job(Job::scan);
        if (!busy_ || job_ != Job::scan) return 8;
        SendMessageW(hwnd_, WM_CLOSE, 0, 0);
        worker_.join();
        worker_complete();
        if (!IsWindow(hwnd_) || busy_ || close_dialog_open_ || !tray_added_ ||
            IsWindowVisible(hwnd_)) return 9;
        restore_from_tray();
        if (!IsWindowVisible(hwnd_) || tray_added_) return 10;

        close_to_tray_ = false;
        start_job(Job::scan);
        if (!busy_ || job_ != Job::scan) return 11;
        SendMessageW(hwnd_, WM_CLOSE, 0, 0);
        worker_.join();
        worker_complete();
        return !IsWindow(hwnd_) && !busy_ && job_ == Job::none &&
            !close_after_read_only_job_ ? 0 : 12;
    }

    int smoke_instance_choice() {
        if (!profile_) return 2;
        const auto source_root = application_directory();
        InstanceSelection multiple{source_root, {}};
        for (int index = 0; index < 6; ++index) {
            InstanceInfo candidate;
            candidate.root = source_root;
            candidate.display_name = L"Test instance " + std::to_wstring(index + 1);
            multiple.candidates.push_back(std::move(candidate));
        }
        WorkerOutcome source_outcome;
        source_outcome.job = Job::discover_source;
        source_outcome.value = std::move(multiple);
        pending_outcome_ = std::move(source_outcome);
        worker_complete();
        if (source_index_ != -1 || source_path() || !source_selection_ ||
            source_selection_->candidates.size() != 6 || busy_) return 3;
        const auto press = [&](Action action, int index = -1) {
            const auto hit = std::find_if(hits_.begin(), hits_.end(), [&](const Hit& item) {
                return item.action == action && (index < 0 || item.index == index);
            });
            if (hit == hits_.end()) return false;
            const auto bounds = hit->rectangle;
            click((bounds.left + bounds.right) / 2, (bounds.top + bounds.bottom) / 2);
            return true;
        };
        paint();
        const auto settings_hit = std::find_if(hits_.begin(), hits_.end(), [](const Hit& hit) {
            return hit.action == Action::settings;
        });
        if (settings_hit == hits_.end()) return 10;
        const auto settings_bounds = settings_hit->rectangle;
        if (!press(Action::source_drawer)) return 4;
        paint();
        if (std::any_of(hits_.begin(), hits_.end(), [](const Hit& hit) {
                return hit.action == Action::settings || hit.action == Action::scan;
            })) return 9;
        scroll(-WHEEL_DELTA);
        if (std::abs(source_drawer_first_ - 48.0f / 70.0f) > 0.001f) return 22;
        paint();
        scroll(WHEEL_DELTA);
        if (source_drawer_first_ != 0) return 23;
        paint();
        const auto focused_source_item = [&]() {
            return focus_index_ >= 0 && focus_index_ < static_cast<int>(hits_.size()) &&
                   hits_[focus_index_].action == Action::drawer_item ?
                   hits_[focus_index_].index : -1;
        };
        key_down(VK_DOWN);
        if (focused_source_item() != 0) return 13;
        key_down(VK_UP);
        if (focused_source_item() != 0) return 14;
        key_down(VK_END);
        if (std::abs(source_drawer_first_ - (6 - drawer_visible_rows_)) > 0.001f || focused_source_item() != 5) return 15;
        key_down(VK_HOME);
        if (source_drawer_first_ != 0 || focused_source_item() != 0) return 16;
        click((settings_bounds.left + settings_bounds.right) / 2,
              (settings_bounds.top + settings_bounds.bottom) / 2);
        if (source_drawer_open_ || page_ != Page::migration) return 11;
        paint();
        if (!press(Action::source_drawer)) return 12;
        paint();
        if (std::any_of(hits_.begin(), hits_.end(), [](const Hit& hit) {
                return hit.action == Action::drawer_previous || hit.action == Action::drawer_next;
            })) return 5;
        scroll(-WHEEL_DELTA);
        if (std::abs(source_drawer_first_ - 48.0f / 70.0f) > 0.001f) return 5;
        paint();
        key_down(VK_HOME);
        if (source_drawer_first_ != 0 || focused_source_item() != 0) return 17;
        for (int index = 0; index < 5; ++index) key_down(VK_DOWN);
        if (std::abs(source_drawer_first_ - (6 - drawer_visible_rows_)) > 0.001f || focused_source_item() != 5) return 18;
        key_down(VK_RETURN);
        if (source_index_ != 5 || source_drawer_open_) return 6;
        WorkerOutcome target_outcome;
        target_outcome.job = Job::discover_target;
        InstanceSelection target_multiple{fs::temp_directory_path(), {}};
        for (int index = 0; index < 6; ++index) {
            InstanceInfo target;
            target.root = target_multiple.selected_path;
            target.display_name = L"Test target " + std::to_wstring(index + 1);
            target_multiple.candidates.push_back(std::move(target));
        }
        target_outcome.value = std::move(target_multiple);
        pending_outcome_ = std::move(target_outcome);
        worker_complete();
        if (busy_ || target_index_ != -1) return 7;
        paint();
        if (!press(Action::target_drawer)) return 19;
        paint();
        key_down(VK_END);
        if (std::abs(target_drawer_first_ - (6 - drawer_visible_rows_)) > 0.001f || focus_index_ < 0 ||
            focus_index_ >= static_cast<int>(hits_.size()) ||
            hits_[focus_index_].action != Action::drawer_item ||
            hits_[focus_index_].index != -6) return 20;
        key_down(VK_RETURN);
        if (target_index_ != 5 || target_drawer_open_ || !busy_ || job_ != Job::scan) return 21;
        cancel_read_only_job();
        worker_.join();
        worker_complete();
        return !busy_ && !plan_ && source_index_ == 5 && target_index_ == 5 ? 0 : 8;
    }

    int smoke_migration_close_guard() {
        busy_ = true;
        job_ = Job::migrate;
        prompt_on_close_ = true;
        SendMessageW(hwnd_, WM_CLOSE, 0, 0);
        if (!IsWindow(hwnd_) || close_dialog_open_ ||
            status_ != tr(L"正在写入迁移文件，请等待完成后再关闭。",
                          L"Migration is writing files. Wait until it finishes before closing.")) return 2;
        SendMessageW(hwnd_, WM_COMMAND, tray_exit_command, 0);
        if (!IsWindow(hwnd_) || worker_.get_stop_token().stop_requested()) return 3;
        busy_ = false;
        job_ = Job::none;
        return 0;
    }

    int smoke_migration_error() {
        english_ = false;
        page_ = Page::confirmation;
        busy_ = true;
        job_ = Job::migrate;
        WorkerOutcome outcome;
        outcome.job = Job::migrate;
        outcome.error = "Test migration error";
        pending_outcome_ = std::move(outcome);
        worker_complete();
        if (busy_ || page_ != Page::error || migration_error_ !=
                L"迁移已安全停止，请检查迁入实例和磁盘空间，然后重新扫描。" ||
            migration_error_.find(L"Test migration error") != std::wstring::npos) return 2;
        migration_error_.clear();
        for (int index = 0; index < 30; ++index)
            migration_error_ += L"The destination file could not be written.\n";
        paint();
        if (error_total_height_ <= error_visible_height_) return 3;
        scroll(-WHEEL_DELTA);
        if (error_scroll_ <= 0) return 4;
        const auto back = std::find_if(hits_.begin(), hits_.end(), [](const Hit& hit) {
            return hit.action == Action::back;
        });
        if (back == hits_.end() || std::any_of(hits_.begin(), hits_.end(), [](const Hit& hit) {
                return hit.action == Action::confirm_start;
            })) return 5;
        const auto bounds = back->rectangle;
        click((bounds.left + bounds.right) / 2, (bounds.top + bounds.bottom) / 2);
        return page_ == Page::migration ? 0 : 6;
    }

    int smoke_real_migration_error() {
        const auto build_root = application_directory().parent_path();
        const auto fixture = build_root /
            (L"ui-migration-error-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
             std::to_wstring(GetTickCount64()));
        if (!is_within(build_root, fixture, true) ||
            !fixture.filename().wstring().starts_with(L"ui-migration-error-") || fs::exists(fixture)) return 2;
        int result = 0;
        try {
            const auto source = fixture / "source";
            const auto target = fixture / "target";
            fs::create_directories(source);
            fs::create_directories(target);
            {
                std::ofstream input(source / "file.txt", std::ios::binary);
                input << "isolated migration fixture";
                if (!input) throw std::runtime_error("Could not create migration fixture");
            }
            {
                std::ofstream blocked(target / "SempervirensReports", std::ios::binary);
                blocked << "keep this file";
                if (!blocked) throw std::runtime_error("Could not block fixture report folder");
            }
            MigrationProfile profile;
            profile.name = "UI migration failure fixture";
            profile.rules = {{"file", "test", "Test file", "", RuleKind::file,
                std::nullopt, "", "file.txt", "file.txt", true, true}};
            plan_ = build_plan(std::move(profile), source, target);
            if (!plan_->valid() || plan_->operations.empty()) result = 3;
            else {
                page_ = Page::confirmation;
                start_job(Job::migrate);
                worker_.join();
                worker_complete();
                if (page_ != Page::error || busy_ || migration_error_ != tr(
                        L"选中内容可能已写入，但迁移记录未能保存。请检查迁入实例和磁盘空间。",
                        L"Selected files may have been written, but the migration record could not be saved. Review the destination and disk space.") ||
                    !fs::is_regular_file(target / "file.txt")) result = 4;
            }
        } catch (const std::exception&) { result = 5; }
        std::error_code cleanup_error;
        if (is_within(build_root, fixture, true)) fs::remove_all(fixture, cleanup_error);
        return cleanup_error ? 6 : result;
    }

    int smoke_real_migration_success(bool english, bool keyboard = false) {
        const auto build_root = application_directory().parent_path();
        const auto fixture = build_root /
            (L"ui-migration-success-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
             std::to_wstring(GetTickCount64()));
        if (!is_within(build_root, fixture, true) ||
            !fixture.filename().wstring().starts_with(L"ui-migration-success-") ||
            fs::exists(fixture)) return 2;
        int result = 0;
        try {
            english_ = english;
            const auto source = fixture / "source";
            const auto target = fixture / "target";
            fs::create_directories(source);
            fs::create_directories(target);
            auto write_fixture = [](const fs::path& path, std::string_view contents) {
                std::ofstream output(path, std::ios::binary);
                output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
                if (!output) throw std::runtime_error("Could not write UI migration fixture");
            };
            auto read_fixture = [](const fs::path& path) {
                std::ifstream input(path, std::ios::binary);
                if (!input) throw std::runtime_error("Could not read UI migration fixture");
                return std::string(std::istreambuf_iterator<char>{input}, {});
            };
            write_fixture(source / "options.txt", "fov:70\n");
            write_fixture(target / "options.txt", "fov:70\n");
            write_fixture(source / "file.txt", "migrated content\n");
            write_fixture(target / "file.txt", "previous content\n");
            const auto profile_path = fixture / "rules.json";
            write_fixture(profile_path,
                R"({"schemaVersion":1,"name":"UI workflow fixture","rules":[{"id":"file","name":"Test file","kind":"file","source":"file.txt","target":"file.txt","defaultSelected":true}]})");
            profile_ = load_profile(profile_path);
            profile_file_ = profile_path;
            start_job(Job::discover_source, source);
            worker_.join();
            worker_complete();
            if (busy_ || source_path() != source || target_path()) result = 3;
            if (!result) {
                start_job(Job::discover_target, target);
                worker_.join();
                worker_complete();
                if (!busy_ || job_ != Job::scan || target_path() != target) result = 4;
            }
            if (!result) {
                worker_.join();
                worker_complete();
                if (busy_ || !plan_ || !plan_->valid() || plan_->operations.size() != 1 ||
                    plan_->operations[0].status != ScanStatus::conflict) result = 5;
            }
            const auto press = [&](Action action) {
                paint();
                const auto hit = std::find_if(hits_.begin(), hits_.end(), [&](const Hit& item) {
                    return item.action == action;
                });
                if (hit == hits_.end()) return false;
                const auto bounds = hit->rectangle;
                click((bounds.left + bounds.right) / 2, (bounds.top + bounds.bottom) / 2);
                return true;
            };
            const auto activate = [&](Action action) {
                if (!keyboard) return press(action);
                paint();
                focus_index_ = -1;
                const auto count = hits_.size();
                for (std::size_t index = 0; index < count; ++index) {
                    key_down(VK_TAB);
                    if (focus_index_ >= 0 && focus_index_ < static_cast<int>(hits_.size()) &&
                        hits_[focus_index_].action == action) {
                        key_down(action == Action::list_toggle ? VK_SPACE : VK_RETURN);
                        return true;
                    }
                }
                return false;
            };
            if (!result && (!activate(Action::list_toggle) || plan_->valid())) result = 12;
            if (!result) {
                paint();
                if (std::any_of(hits_.begin(), hits_.end(), [](const Hit& item) {
                        return item.action == Action::start;
                    })) result = 13;
            }
            if (!result && (!activate(Action::list_toggle) || !plan_->valid())) result = 14;
            if (!result && (!activate(Action::start) || page_ != Page::confirmation)) result = 6;
            if (!result && (!activate(Action::confirm_start) || !busy_ || job_ != Job::migrate)) result = 7;
            if (!result) {
                worker_.join();
                worker_complete();
                if (busy_ || page_ != Page::result || !result_ || result_->success_count != 1 ||
                    result_->failed_count != 0 || result_->files_copied != 1 ||
                    result_->files_backed_up != 1 || !result_->backup_root ||
                    read_fixture(target / "file.txt") != "migrated content\n" ||
                    read_fixture(source / "file.txt") != "migrated content\n" ||
                    read_fixture(*result_->backup_root / "file.txt") != "previous content\n" ||
                    !fs::is_regular_file(result_->log_path)) result = 8;
            }
            if (!result) {
                paint();
                if (std::none_of(hits_.begin(), hits_.end(), [](const Hit& item) {
                        return item.action == Action::report;
                    }) || std::none_of(hits_.begin(), hits_.end(), [](const Hit& item) {
                        return item.action == Action::backup;
                    })) result = 9;
            }
            if (!result) {
                fs::remove(result_->log_path);
                fs::remove_all(*result_->backup_root);
                paint();
                if (std::any_of(hits_.begin(), hits_.end(), [](const Hit& item) {
                        return item.action == Action::report || item.action == Action::backup;
                    })) result = 15;
            }
        } catch (const std::exception&) { result = 10; }
        if (worker_.joinable()) { worker_.request_stop(); worker_.join(); }
        std::error_code cleanup_error;
        if (is_within(build_root, fixture, true)) fs::remove_all(fixture, cleanup_error);
        return cleanup_error ? 11 : result;
    }

    int smoke_result_state() {
        result_ = MigrationResult{};
        result_->success_count = 2;
        result_->failed_count = 1;
        page_ = Page::result;
        paint();
        if (result_section_text() != L"请检查失败项目" ||
            std::any_of(hits_.begin(), hits_.end(), [](const Hit& item) {
                return item.action == Action::report || item.action == Action::backup;
            })) return 2;
        english_ = true;
        paint();
        if (result_section_text() != L"Review the failed items") return 3;
        result_->failed_count = 0;
        paint();
        if (result_section_text() != L"All done") return 4;
        english_ = false;
        paint();
        return result_section_text() == L"迁移已完成" ? 0 : 5;
    }

    int smoke_project_link() const {
        if (shell_launch_succeeded(32) || !shell_launch_succeeded(33)) return 2;
        const auto chinese = project_open_failure_message(false);
        const auto english = project_open_failure_message(true);
        if (chinese.find(L"无法打开浏览器") == std::wstring::npos ||
            english.find(L"The browser could not be opened") == std::wstring::npos ||
            chinese.find(project_url) == std::wstring::npos ||
            english.find(project_url) == std::wstring::npos) return 3;
        return 0;
    }

    int smoke_profile_rescan() {
        const auto build_root = application_directory().parent_path();
        const auto fixture = build_root /
            (L"ui-profile-rescan-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
             std::to_wstring(GetTickCount64()));
        if (!is_within(build_root, fixture, true) ||
            !fixture.filename().wstring().starts_with(L"ui-profile-rescan-") || fs::exists(fixture)) return 2;
        int result = 0;
        try {
            const auto source = fixture / "source";
            const auto target = fixture / "target";
            fs::create_directories(source);
            fs::create_directories(target);
            {
                std::ofstream output(source / "selected.txt", std::ios::binary);
                output << "selected profile data";
                if (!output) throw std::runtime_error("Could not create profile-rescan fixture");
            }
            InstanceInfo source_info;
            source_info.root = source;
            source_info.display_name = L"Source";
            InstanceInfo target_info;
            target_info.root = target;
            target_info.display_name = L"Target";
            source_selection_ = InstanceSelection{source, {source_info}};
            target_selection_ = InstanceSelection{target, {target_info}};
            source_index_ = target_index_ = 0;
            MigrationProfile selected;
            selected.name = "Selected rules";
            selected.rules = {{"selected", "test", "Selected file", "", RuleKind::file,
                std::nullopt, "", "selected.txt", "selected.txt", true, true}};
            profile_ = std::move(selected);
            plan_.reset();
            page_ = Page::settings;
            profile_changed_in_settings_ = true;
            leave_settings();
            if (page_ != Page::migration || !busy_ || job_ != Job::scan ||
                profile_changed_in_settings_) result = 3;
            if (!result) {
                worker_.join();
                worker_complete();
                if (busy_ || !plan_ || !plan_->valid() || plan_->operations.size() != 1 ||
                    plan_->operations[0].id != "selected") result = 4;
            }
            if (!result) {
                page_ = Page::settings;
                leave_settings();
                if (page_ != Page::migration || busy_) result = 5;
            }
        } catch (const std::exception&) { result = 6; }
        if (worker_.joinable()) { worker_.request_stop(); worker_.join(); }
        std::error_code cleanup_error;
        if (is_within(build_root, fixture, true)) fs::remove_all(fixture, cleanup_error);
        return cleanup_error ? 7 : result;
    }

    int smoke_result_open_arguments() {
        const auto build_root = application_directory().parent_path();
        const auto fixture = build_root /
            (L"ui-open-path-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
             std::to_wstring(GetTickCount64()));
        if (!is_within(build_root, fixture, true) ||
            !fixture.filename().wstring().starts_with(L"ui-open-path-") ||
            fs::exists(fixture)) return 2;
        int result = 0;
        try {
            const auto backup = fixture / L"\u5907\u4efd \u76ee\u5f55";
            const auto record = fixture / L"\u8fc1\u79fb \u8bb0\u5f55.txt";
            fs::create_directories(backup);
            std::ofstream output(record, std::ios::binary);
            output << "test";
            output.close();
            if (!output) result = 3;
            if (!result && explorer_arguments(record) !=
                    std::optional<std::wstring>(L"/select,\"" + record.wstring() + L"\"")) result = 4;
            if (!result && explorer_arguments(backup) !=
                    std::optional<std::wstring>(L"\"" + backup.wstring() + L"\"")) result = 5;
            if (!result && explorer_arguments(fixture / L"missing")) result = 6;
            if (!result) {
                result_ = MigrationResult{};
                result_->log_path = record;
                result_->backup_root = backup;
                page_ = Page::result;
                paint();
                const bool has_record = std::any_of(hits_.begin(), hits_.end(), [](const Hit& hit) {
                    return hit.action == Action::report;
                });
                const bool has_backup = std::any_of(hits_.begin(), hits_.end(), [](const Hit& hit) {
                    return hit.action == Action::backup;
                });
                if (!has_record || !has_backup) result = 7;
            }
            if (!result) {
                page_ = Page::migration;
                paint();
                const bool has_record = std::any_of(hits_.begin(), hits_.end(), [](const Hit& hit) {
                    return hit.action == Action::report;
                });
                const bool has_backup = std::any_of(hits_.begin(), hits_.end(), [](const Hit& hit) {
                    return hit.action == Action::backup;
                });
                if (!has_record || !has_backup) result = 10;
            }
            if (!result) {
                fs::remove(record);
                fs::remove_all(backup);
                paint();
                if (std::any_of(hits_.begin(), hits_.end(), [](const Hit& hit) {
                        return hit.action == Action::report || hit.action == Action::backup;
                    })) result = 11;
            }
        } catch (const std::exception&) { result = 8; }
        std::error_code cleanup_error;
        if (is_within(build_root, fixture, true)) fs::remove_all(fixture, cleanup_error);
        return cleanup_error ? 9 : result;
    }

    int smoke_settings_write() {
        if (!settings_override_ || fs::exists(*settings_override_) ||
            english_ || !prompt_on_close_ || close_to_tray_) return 2;
        const auto press = [&](Action action) {
            const auto hit = std::find_if(hits_.begin(), hits_.end(), [&](const Hit& item) {
                return item.action == action;
            });
            if (hit == hits_.end()) return false;
            const auto bounds = hit->rectangle;
            click((bounds.left + bounds.right) / 2, (bounds.top + bounds.bottom) / 2);
            return true;
        };
        begin_onboarding();
        paint();
        if (page_ != Page::welcome || !onboarding_ || !use_minecraft_profile_ || profile_file_ ||
            !press(Action::profile_xintinglei) || use_minecraft_profile_) return 21;
        paint();
        if (!press(Action::profile_minecraft) || !use_minecraft_profile_) return 22;
        paint();
        if (!press(Action::welcome_continue) || onboarding_ || page_ != Page::migration ||
            !fs::exists(*settings_override_)) return 23;
        page_animation_started_ = 0;
        page_ = Page::settings;
        paint();
        const auto prompt_hit = std::find_if(hits_.begin(), hits_.end(), [](const Hit& hit) {
            return hit.action == Action::close_prompt;
        });
        if (prompt_hit == hits_.end()) return 10;
        if (!press(Action::language)) return 3;
        if (!reduced_motion_ && language_animation_started_ == 0) return 24;
        if (language_animation_started_ != 0)
            language_animation_started_ = GetTickCount64() - language_animation_duration_ms / 2;
        paint();
        if (std::any_of(hits_.begin(), hits_.end(), [](const Hit& hit) {
                return hit.action == Action::back || hit.action == Action::close_prompt;
            })) return 9;
        key_down(VK_DOWN);
        if (focus_index_ != 0) return 13;
        key_down(VK_UP);
        if (focus_index_ != 2) return 14;
        if (!press(Action::language)) return 16;
        if (language_drawer_open_ || !prompt_on_close_ ||
            (!reduced_motion_ && (language_animation_started_ == 0 ||
                                  language_animation_to_ != 0.0f))) return 11;
        paint();
        if (!press(Action::language)) return 12;
        paint();
        if (!press(Action::language_en) || !english_) return 4;
        finish_language_animation();
        paint();
        if (!press(Action::close_prompt) || prompt_on_close_) return 5;
        if (!press(Action::close_action) || !close_to_tray_) return 17;
        if (!press(Action::settings_migration)) return 18;
        if (!reduced_motion_ && (settings_section_animation_started_ == 0 ||
                                 page_animation_started_ != 0)) return 25;
        paint();
        if (!press(Action::profile_minecraft) || !use_minecraft_profile_ || !profile_ ||
            profile_->name != "Sempervirens-Minecraft 原版") return 19;
        paint();
        if (!press(Action::profile_xintinglei) || use_minecraft_profile_ || !profile_ ||
            profile_->name != "Sempervirens-新亭泪社区专用迁移器") return 20;
        page_ = Page::migration;
        close_dialog_open_ = true;
        remember_close_choice_ = false;
        paint();
        if (!press(Action::close_remember) || !remember_close_choice_) return 6;
        if (!press(Action::close_tray) || !close_to_tray_ || prompt_on_close_) return 15;
        const auto custom_profile = settings_override_->parent_path() / "custom-profile.json";
        {
            std::ofstream output(custom_profile, std::ios::binary);
            output << R"({"schemaVersion":1,"name":"Isolated test rules","rules":[{"id":"test","name":"Test file","kind":"file","source":"file.txt","target":"file.txt"}]})";
            if (!output) return 7;
        }
        profile_ = load_profile(custom_profile);
        profile_file_ = custom_profile;
        use_minecraft_profile_ = false;
        save_settings();
        const auto saved = read_preferences(*settings_override_);
        return saved && saved->english && !saved->prompt_on_close && saved->close_to_tray &&
            saved->migration_profile_path == custom_profile && !saved->use_minecraft_profile ? 0 : 8;
    }

    int smoke_settings_read() {
        const auto custom_profile = settings_override_ ?
            settings_override_->parent_path() / "custom-profile.json" : fs::path{};
        if (!settings_override_ || !english_ || prompt_on_close_ || !close_to_tray_ ||
            profile_file_ != custom_profile || !profile_ || profile_->name != "Isolated test rules") return 2;
        page_ = Page::settings;
        paint();
        if (hits_.empty()) return 3;
        close_requested();
        return IsWindow(hwnd_) && !close_dialog_open_ ? 0 : 4;
    }

    int smoke_settings_invalid() const {
        if (!settings_override_ || profile_file_ || !profile_ ||
            profile_->name != "Sempervirens-新亭泪社区专用迁移器") return 2;
        const auto saved = read_preferences(*settings_override_);
        return saved && !saved->migration_profile_path ? 0 : 3;
    }

    void start_job(Job job, const fs::path& folder = {}) {
        if (busy_) return;
        if (worker_.joinable()) worker_.join();
        {
            std::scoped_lock lock(worker_mutex_);
            pending_progress_.reset();
            progress_message_queued_ = false;
        }
        busy_ = true;
        job_ = job;
        active_discovery_folder_ =
            job == Job::discover_source || job == Job::discover_target ?
                std::optional<fs::path>(folder) : std::nullopt;
        scan_cancel_requested_ = false;
        progress_percent_ = std::nullopt;
        status_ = tr(L"正在准备…", L"Preparing…");
        if (job == Job::scan) plan_.reset();
        const auto source = source_path();
        const auto target = target_path();
        const auto profile = profile_;
        const auto plan = plan_;
        const auto strategy = strategy_;
        const auto english = english_;
        worker_ = std::jthread([this, job, folder, source, target, profile, plan, strategy, english](std::stop_token token) {
            WorkerOutcome outcome;
            outcome.job = job;
            const auto progress = [this](const ScanProgress& value) { publish_progress(value); };
            try {
                if (job == Job::discover_source || job == Job::discover_target)
                    outcome.value = discover_instances(folder,
                        english ? L"Default Minecraft instance" : L"默认 Minecraft 实例",
                        english ? L"Current folder" : L"当前文件夹", token, progress, english);
                else if (job == Job::scan && profile && source && target)
                    outcome.value = build_plan(*profile, *source, *target, token, progress, english);
                else if (job == Job::migrate && plan)
                    outcome.value = execute_migration(*plan, strategy, token, progress, english);
                else throw std::runtime_error(english ?
                    "The migration information is incomplete. Return to the migration page and scan again." :
                    "迁移信息不完整，请返回迁移页面重新扫描。");
            } catch (const std::exception& error) {
                outcome.cancelled = token.stop_requested();
                if (!outcome.cancelled) outcome.error = error.what();
            }
            {
                std::scoped_lock lock(worker_mutex_);
                pending_outcome_ = std::move(outcome);
            }
            PostMessageW(hwnd_, wm_worker, 0, 0);
        });
        if (!resizing_) {
            fast_animation_timer_ = !progress_percent_ && !reduced_motion_;
            SetTimer(hwnd_, animation_timer, fast_animation_timer_ ? 16 : 80, nullptr);
        }
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    void cancel_read_only_job() {
        if (!busy_ || job_ == Job::migrate || scan_cancel_requested_) return;
        scan_cancel_requested_ = true;
        status_ = tr(L"正在取消扫描…", L"Cancelling scan…");
        if (worker_.joinable()) worker_.request_stop();
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    void cancel_read_only_job_and_close(bool force_exit = false) {
        if (!busy_ || job_ == Job::migrate) return;
        close_after_read_only_job_ = true;
        force_exit_after_read_only_job_ = force_exit;
        close_dialog_open_ = false;
        cancel_read_only_job();
        status_ = tr(L"正在取消扫描并退出…", L"Cancelling scan and closing…");
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    void worker_progress() {
        std::optional<ScanProgress> latest;
        {
            std::scoped_lock lock(worker_mutex_);
            latest = std::move(pending_progress_);
            pending_progress_.reset();
            progress_message_queued_ = false;
        }
        if (latest && !scan_cancel_requested_) {
            progress_percent_ = latest->percent;
            status_ = std::move(latest->message);
            const bool animate_indeterminate = !progress_percent_ && !reduced_motion_;
            if (!resizing_ && animate_indeterminate != fast_animation_timer_) {
                KillTimer(hwnd_, animation_timer);
                fast_animation_timer_ = animate_indeterminate;
                SetTimer(hwnd_, animation_timer, fast_animation_timer_ ? 16 : 80, nullptr);
            }
            if (!resizing_) InvalidateRect(hwnd_, nullptr, FALSE);
        }
    }

    void worker_complete() {
        std::optional<WorkerOutcome> outcome;
        {
            std::scoped_lock lock(worker_mutex_);
            outcome = std::move(pending_outcome_);
            pending_outcome_.reset();
            pending_progress_.reset();
            progress_message_queued_ = false;
        }
        if (worker_.joinable()) worker_.join();
        const bool cancellation_requested = scan_cancel_requested_;
        const bool close_after_read_only_job = close_after_read_only_job_;
        const bool force_exit_after_read_only_job = force_exit_after_read_only_job_;
        busy_ = false;
        job_ = Job::none;
        active_discovery_folder_.reset();
        scan_cancel_requested_ = false;
        close_after_read_only_job_ = false;
        force_exit_after_read_only_job_ = false;
        KillTimer(hwnd_, animation_timer);
        fast_animation_timer_ = false;
        if (!outcome) {
            if (close_after_read_only_job) {
                if (force_exit_after_read_only_job) exit_now();
                else close_requested();
            }
            return;
        }
        if (outcome->cancelled || cancellation_requested) {
            progress_percent_ = std::nullopt;
            status_ = tr(L"扫描已取消。", L"Scan cancelled.");
        } else if (!outcome->error.empty()) {
            if (outcome->job == Job::migrate) {
                migration_error_ = outcome->error == "Migration record could not be saved" ?
                    tr(L"选中内容可能已写入，但迁移记录未能保存。请检查迁入实例和磁盘空间。",
                       L"Selected files may have been written, but the migration record could not be saved. Review the destination and disk space.") :
                    friendly_worker_error(outcome->job, outcome->error, english_);
                error_scroll_ = 0;
                page_ = Page::error;
                start_page_animation(1);
                status_ = tr(L"迁移已中断，请检查迁入实例。",
                             L"Migration stopped. Review the destination instance.");
            } else status_ = friendly_worker_error(outcome->job, outcome->error, english_);
        } else if (auto* selection = std::get_if<InstanceSelection>(&outcome->value)) {
            const auto count = selection->candidates.size();
            if (outcome->job == Job::discover_source) {
                source_selection_ = std::move(*selection);
                source_index_ = count == 1 ? 0 : -1;
                source_drawer_first_ = 0;
            } else {
                target_selection_ = std::move(*selection);
                target_index_ = count == 1 ? 0 : -1;
                target_drawer_first_ = 0;
            }
            source_drawer_open_ = target_drawer_open_ = false;
            plan_.reset();
            if (count == 0) status_ = tr(L"此文件夹中没有发现可用实例。", L"No usable instance was found in this folder.");
            else if (count > 1) status_ = tr(L"发现多个实例，请在抽屉中选择一个。",
                                             L"Several instances found. Choose one from the drawer.");
            else status_ = tr(L"实例已选择，可以扫描内容。", L"Instance selected. You can scan contents.");
            if (!close_after_read_only_job && source_path() && target_path() && profile_)
                start_job(Job::scan);
        } else if (auto* plan = std::get_if<MigrationPlan>(&outcome->value)) {
            plan_ = std::move(*plan);
            selected_operation_ = plan_->operations.empty() ? -1 : 0;
            detail_scroll_ = 0;
            list_first_ = 0;
            status_ = plan_->error.empty() ? tr(L"扫描完成，请审阅迁移清单。", L"Scan complete. Review the migration list.") :
                                            utf8_to_wide(plan_->error);
        } else if (auto* result = std::get_if<MigrationResult>(&outcome->value)) {
            result_ = std::move(*result);
            page_ = Page::result;
            start_page_animation(1);
            status_ = tr(L"迁移结束，请查看结果。", L"Migration finished. Review the result.");
        }
        InvalidateRect(hwnd_, nullptr, FALSE);
        if (close_after_read_only_job) {
            if (force_exit_after_read_only_job) exit_now();
            else close_requested();
        }
    }

    void resize(UINT width, UINT height) {
        if (width == 0 || height == 0) return;
        if (target_) {
            const auto current = target_->GetPixelSize();
            if (current.width == width && current.height == height) return;
            if (FAILED(target_->Resize(D2D1::SizeU(width, height)))) {
                branding_.reset_target();
                release(brush_);
                release(target_);
            }
        }
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    void paint() {
        PAINTSTRUCT ps{};
        BeginPaint(hwnd_, &ps);
        if (ensure_target()) draw();
        EndPaint(hwnd_, &ps);
    }

    void prepare_visual_sample(bool english_sample) {
        if (!profile_) return;
        const auto source_root = fs::path(L"C:\\Minecraft\\旧实例");
        const auto target_root = fs::path(L"D:\\Games\\新实例");
        InstanceInfo old_instance;
        old_instance.root = source_root;
        old_instance.display_name = L"1.21.4 Fabric · 旧实例";
        InstanceInfo new_instance;
        new_instance.root = target_root;
        new_instance.display_name = L"1.21.4 Fabric · 新实例";
        source_selection_ = InstanceSelection{source_root, {old_instance}};
        target_selection_ = InstanceSelection{target_root, {new_instance}};
        source_index_ = target_index_ = 0;
        MigrationPlan sample{source_root, target_root, *profile_, {}, {}};
        const auto make = [&](std::size_t index, ScanStatus status) {
            const auto& rule = profile_->rules.at(index);
            PlannedOperation operation;
            operation.rule_index = index;
            operation.id = rule.id;
            operation.name = rule.name;
            operation.group = rule.group;
            operation.description = rule.description;
            operation.source_path = source_root / fs::path(utf8_to_wide(rule.source));
            operation.target_path = target_root / fs::path(utf8_to_wide(rule.target));
            operation.status = status;
            operation.selected = rule.default_selected;
            operation.is_builtin = rule.kind == RuleKind::builtin;
            operation.is_directory = rule.kind == RuleKind::directory;
            return operation;
        };
        auto bindings = make(0, ScanStatus::conflict);
        bindings.payload = OptionScanInfo{4, {{"key_key.forward", "key.keyboard.w", "key.keyboard.up"},
                                               {"key_key.jump", "key.keyboard.space", "key.keyboard.j"},
                                               {"key_key.inventory", "key.keyboard.e", "key.keyboard.i"}}};
        sample.operations.push_back(std::move(bindings));
        sample.operations.push_back(make(1, ScanStatus::identical));
        sample.operations.push_back(make(2, ScanStatus::found));
        auto servers = make(3, ScanStatus::conflict);
        servers.comparison = ServerComparisonInfo{"新亭泪", "常玩服务器", true};
        sample.operations.push_back(std::move(servers));
        sample.operations.push_back(make(4, ScanStatus::found));
        auto worlds = make(5, ScanStatus::conflict);
        worlds.name = "山谷之家";
        worlds.is_directory = true;
        worlds.comparison = FolderComparisonInfo{24, 2, 1, 21, true,
            {{fs::path(L"level.dat"), FolderDifferenceKind::changed},
             {fs::path(L"region\\r.1.0.mca"), FolderDifferenceKind::added}}};
        sample.operations.push_back(std::move(worlds));
        plan_ = std::move(sample);
        selected_operation_ = 0;
        status_ = english_sample ? L"4 items selected. Review them before migrating." :
            L"已选择 4 项内容，可继续审阅并开始迁移。";
    }

    void prepare_search_visual_sample() {
        search_ = L"\u641c\u7d22\uff1a\u670d\u52a1\u5668\u5730\u5740 play.example.cn \u4e0e\u4e16\u754c\u6587\u4ef6\u5939\u540d\u79f0\uff0c"
                  L"\u7ee7\u7eed\u8f93\u5165\u4ee5\u68c0\u67e5\u8d85\u957f\u67e5\u8be2\u5728\u7a97\u53e3\u7f29\u653e\u540e\u7684\u6a2a\u5411\u6eda\u52a8\u8fb9\u754c";
        search_anchor_ = 5;
        search_caret_ = search_.size();
        search_focused_ = true;
        search_scroll_ = 0;
    }

    void prepare_drawer_visual_sample() {
        prepare_visual_sample(false);
        if (!source_selection_) return;
        source_selection_->candidates.clear();
        for (int index = 0; index < 12; ++index) {
            InstanceInfo candidate;
            candidate.root = index == 3 ?
                fs::path(L"I:\\Minecraft\\XintingleiClient-V1.1.0\\.minecraft\\versions\\1.21.4-Fabric 0.18.4") :
                fs::path(L"C:\\Minecraft") / (L"实例-" + std::to_wstring(index + 1));
            candidate.display_name = index == 3 ? L"1.21.4 Fabric · 新亭泪整合实例" :
                L"Minecraft 实例 " + std::to_wstring(index + 1);
            source_selection_->candidates.push_back(std::move(candidate));
        }
        source_index_ = 3;
        source_drawer_first_ = 3;
        source_drawer_open_ = true;
        target_drawer_open_ = false;
    }

    void prepare_close_visual_sample() {
        prepare_visual_sample(false);
        close_dialog_open_ = true;
        remember_close_choice_ = false;
    }

    void prepare_settings_visual_sample(SettingsSection section) {
        settings_section_ = section;
        language_drawer_open_ = false;
    }

    bool capture(const fs::path& output_path, bool settings_page, bool populated, bool language_popup,
                 UINT width = 1120, UINT height = 800, bool english_sample = false,
                 bool error_sample = false, bool confirmation_sample = false,
                 bool scan_sample = false, bool indeterminate_sample = false,
                 bool result_sample = false, bool failed_result_sample = false,
                 bool post_result_sample = false, bool welcome_sample = false) {
        if (populated) prepare_visual_sample(english_sample);
        const auto previous_result = result_;
        const auto previous_status = status_;
        if (result_sample || post_result_sample) {
            MigrationResult sample;
            sample.success_count = failed_result_sample ? 2 : 3;
            sample.skipped_count = 1;
            sample.failed_count = failed_result_sample ? 1 : 0;
            sample.log_path = application_directory() / "Sempervirens.exe";
            if (post_result_sample) sample.backup_root = application_directory();
            result_ = std::move(sample);
            if (post_result_sample)
                status_ = english_sample ? L"Migration finished. Review the result." :
                                           L"迁移结束，请查看结果。";
        }
        if (scan_sample) {
            plan_.reset();
            busy_ = true;
            job_ = Job::scan;
            progress_percent_ = indeterminate_sample ? std::nullopt : std::optional<int>(42);
            status_ = indeterminate_sample ?
                tr(L"正在检查实例中的文件…", L"Checking instance files…") :
                tr(L"正在比较可迁移内容 · 42%", L"Comparing transferable data · 42%");
        }
        language_drawer_open_ = language_popup;
        IWICImagingFactory* imaging = nullptr;
        IWICBitmap* bitmap = nullptr;
        ID2D1RenderTarget* offscreen = nullptr;
        ID2D1SolidColorBrush* offscreen_brush = nullptr;
        IWICBitmapEncoder* encoder = nullptr;
        IWICStream* stream = nullptr;
        IWICBitmapFrameEncode* frame = nullptr;
        IPropertyBag2* properties = nullptr;
        bool okay = false;
        do {
            if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                        IID_PPV_ARGS(&imaging)))) break;
            if (FAILED(imaging->CreateBitmap(width, height, GUID_WICPixelFormat32bppPBGRA,
                                             WICBitmapCacheOnLoad, &bitmap))) break;
            const auto props = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_SOFTWARE,
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
            if (FAILED(d2d_factory_->CreateWicBitmapRenderTarget(bitmap, props, &offscreen))) break;
            offscreen->SetDpi(96, 96);
            if (FAILED(offscreen->CreateSolidColorBrush(color(0xFFFFFF), &offscreen_brush))) break;
            const auto previous_page = page_;
            const auto previous_english = english_;
            const auto previous_error = migration_error_;
            auto* previous_brush = brush_;
            auto* previous_canvas = canvas_;
            page_ = welcome_sample ? Page::welcome :
                    error_sample ? Page::error : confirmation_sample ? Page::confirmation :
                    result_sample ? Page::result :
                    settings_page ? Page::settings : Page::migration;
            if (english_sample || error_sample || result_sample) english_ = english_sample;
            if (error_sample) migration_error_ = tr(
                L"无法写入迁入实例中的文件。请确认 Minecraft 与启动器已经关闭，并检查目标目录是否可写。",
                L"A file in the destination instance could not be written. Close Minecraft and its launcher, then check folder permissions.");
            brush_ = offscreen_brush;
            canvas_ = offscreen;
            offscreen->BeginDraw();
            draw_contents(static_cast<float>(width), static_cast<float>(height));
            const auto draw_result = offscreen->EndDraw();
            canvas_ = previous_canvas;
            brush_ = previous_brush;
            english_ = previous_english;
            migration_error_ = previous_error;
            page_ = previous_page;
            if (FAILED(draw_result)) break;
            if (FAILED(imaging->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder))) break;
            if (FAILED(imaging->CreateStream(&stream))) break;
            if (FAILED(stream->InitializeFromFilename(output_path.c_str(), GENERIC_WRITE))) break;
            if (FAILED(encoder->Initialize(stream, WICBitmapEncoderNoCache))) break;
            if (FAILED(encoder->CreateNewFrame(&frame, &properties))) break;
            if (FAILED(frame->Initialize(properties))) break;
            if (FAILED(frame->SetSize(width, height))) break;
            WICPixelFormatGUID format = GUID_WICPixelFormat32bppPBGRA;
            if (FAILED(frame->SetPixelFormat(&format))) break;
            if (FAILED(frame->WriteSource(bitmap, nullptr))) break;
            if (FAILED(frame->Commit()) || FAILED(encoder->Commit())) break;
            okay = true;
        } while (false);
        release(properties); release(frame); release(stream); release(encoder);
        release(offscreen_brush); release(offscreen); release(bitmap); release(imaging);
        result_ = previous_result;
        status_ = previous_status;
        return okay;
    }

    void timer() {
        bool animation_active = false;
        if (scroll_animation_started_ != 0) {
            if (GetTickCount64() - scroll_animation_started_ >= 130) finish_scroll_animation();
            else animation_active = true;
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
        if (drawer_animation_started_ != 0) {
            const auto elapsed = GetTickCount64() - drawer_animation_started_;
            if (elapsed >= drawer_animation_duration_ms) {
                drawer_animation_started_ = 0;
                drawer_animation_from_ = drawer_animation_to_;
                if (drawer_animation_to_ <= 0.0f) drawer_animation_visible_ = false;
            } else animation_active = true;
        }
        if (page_animation_started_ != 0) {
            if (GetTickCount64() - page_animation_started_ >= page_animation_duration_ms)
                page_animation_started_ = 0;
            else animation_active = true;
        }
        if (settings_section_animation_started_ != 0) {
            if (GetTickCount64() - settings_section_animation_started_ >=
                settings_section_animation_duration_ms)
                settings_section_animation_started_ = 0;
            else animation_active = true;
        }
        if (language_animation_started_ != 0) {
            const auto elapsed = GetTickCount64() - language_animation_started_;
            if (elapsed >= language_animation_duration_ms) {
                language_animation_started_ = 0;
                language_animation_from_ = language_animation_to_;
                if (language_animation_to_ <= 0.0f) language_animation_visible_ = false;
            } else animation_active = true;
        }
        if (!resizing_ && (animation_active || busy_)) InvalidateRect(hwnd_, nullptr, FALSE);
        if (animation_active) return;
        if (busy_) {
            const bool animate_indeterminate = !progress_percent_ && !reduced_motion_;
            if (animate_indeterminate != fast_animation_timer_) {
                KillTimer(hwnd_, animation_timer);
                fast_animation_timer_ = animate_indeterminate;
                SetTimer(hwnd_, animation_timer, fast_animation_timer_ ? 16 : 80, nullptr);
            }
            return;
        }
        fast_animation_timer_ = false;
        KillTimer(hwnd_, animation_timer);
    }

    void set_resizing(bool resizing) {
        if (resizing_ == resizing) return;
        resizing_ = resizing;
        if (resizing_) {
            finish_scroll_animation();
            finish_drawer_animation();
            page_animation_started_ = 0;
            settings_section_animation_started_ = 0;
            finish_language_animation();
            KillTimer(hwnd_, animation_timer);
        }
        else {
            if (busy_) {
                fast_animation_timer_ = !progress_percent_ && !reduced_motion_;
                SetTimer(hwnd_, animation_timer, fast_animation_timer_ ? 16 : 80, nullptr);
            }
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
    }

    static float eased_progress(float progress) {
        progress = std::clamp(progress, 0.0f, 1.0f);
        return progress * progress * (3.0f - 2.0f * progress);
    }

    float drawer_reveal(bool source) const {
        if (drawer_animation_started_ != 0 && drawer_animation_source_ == source &&
            !reduced_motion_ && !resizing_) {
            const auto elapsed = GetTickCount64() - drawer_animation_started_;
            const auto linear = std::clamp(static_cast<float>(elapsed) /
                                           static_cast<float>(drawer_animation_duration_ms), 0.0f, 1.0f);
            return drawer_animation_from_ +
                (drawer_animation_to_ - drawer_animation_from_) * eased_progress(linear);
        }
        if (drawer_animation_visible_ && drawer_animation_source_ == source)
            return drawer_animation_to_;
        return source ? (source_drawer_open_ ? 1.0f : 0.0f) :
                        (target_drawer_open_ ? 1.0f : 0.0f);
    }

    void start_drawer_animation(bool source, bool opening) {
        const auto current = drawer_reveal(source);
        drawer_animation_source_ = source;
        drawer_animation_from_ = current;
        drawer_animation_to_ = opening ? 1.0f : 0.0f;
        drawer_animation_visible_ = opening || current > 0.0f;
        if (reduced_motion_ || resizing_ || std::abs(drawer_animation_to_ - current) < 0.001f) {
            drawer_animation_started_ = 0;
            drawer_animation_from_ = drawer_animation_to_;
            if (!opening) drawer_animation_visible_ = false;
            return;
        }
        drawer_animation_started_ = GetTickCount64();
        fast_animation_timer_ = true;
        SetTimer(hwnd_, animation_timer, 15, nullptr);
    }

    void finish_drawer_animation() {
        drawer_animation_started_ = 0;
        drawer_animation_from_ = drawer_animation_to_ =
            (drawer_animation_source_ ? source_drawer_open_ : target_drawer_open_) ? 1.0f : 0.0f;
        drawer_animation_visible_ = drawer_animation_to_ > 0.0f;
    }

    void start_page_animation(int direction) {
        page_animation_direction_ = direction < 0 ? -1.0f : 1.0f;
        if (reduced_motion_ || resizing_) {
            page_animation_started_ = 0;
            return;
        }
        page_animation_started_ = GetTickCount64();
        fast_animation_timer_ = true;
        SetTimer(hwnd_, animation_timer, 15, nullptr);
    }

    float page_reveal() const {
        if (page_animation_started_ == 0 || reduced_motion_ || resizing_) return 1.0f;
        const auto elapsed = GetTickCount64() - page_animation_started_;
        return eased_progress(std::clamp(static_cast<float>(elapsed) /
                              static_cast<float>(page_animation_duration_ms), 0.0f, 1.0f));
    }

    void start_settings_section_animation(int direction) {
        settings_section_animation_direction_ = direction < 0 ? -1.0f : 1.0f;
        if (reduced_motion_ || resizing_) {
            settings_section_animation_started_ = 0;
            return;
        }
        settings_section_animation_started_ = GetTickCount64();
        fast_animation_timer_ = true;
        SetTimer(hwnd_, animation_timer, 15, nullptr);
    }

    float settings_section_reveal() const {
        if (settings_section_animation_started_ == 0 || reduced_motion_ || resizing_) return 1.0f;
        const auto elapsed = GetTickCount64() - settings_section_animation_started_;
        return eased_progress(std::clamp(static_cast<float>(elapsed) /
            static_cast<float>(settings_section_animation_duration_ms), 0.0f, 1.0f));
    }

    float language_reveal() const {
        if (language_animation_started_ != 0 && !reduced_motion_ && !resizing_) {
            const auto elapsed = GetTickCount64() - language_animation_started_;
            const auto linear = std::clamp(static_cast<float>(elapsed) /
                static_cast<float>(language_animation_duration_ms), 0.0f, 1.0f);
            return language_animation_from_ +
                (language_animation_to_ - language_animation_from_) * eased_progress(linear);
        }
        if (language_animation_visible_) return language_animation_to_;
        return language_drawer_open_ ? 1.0f : 0.0f;
    }

    void start_language_animation(bool opening) {
        const auto current = language_reveal();
        language_animation_from_ = current;
        language_animation_to_ = opening ? 1.0f : 0.0f;
        language_animation_visible_ = opening || current > 0.0f;
        if (reduced_motion_ || resizing_ || std::abs(language_animation_to_ - current) < 0.001f) {
            language_animation_started_ = 0;
            language_animation_from_ = language_animation_to_;
            if (!opening) language_animation_visible_ = false;
            return;
        }
        language_animation_started_ = GetTickCount64();
        fast_animation_timer_ = true;
        SetTimer(hwnd_, animation_timer, 15, nullptr);
    }

    void finish_language_animation() {
        language_animation_started_ = 0;
        language_animation_from_ = language_animation_to_ = language_drawer_open_ ? 1.0f : 0.0f;
        language_animation_visible_ = language_drawer_open_;
    }

    int smoke_live_resize() {
        RECT initial_window{};
        RECT initial_client{};
        MONITORINFO monitor{sizeof(monitor)};
        if (!GetWindowRect(hwnd_, &initial_window) || !GetClientRect(hwnd_, &initial_client) ||
            !GetMonitorInfoW(MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST), &monitor)) return 14;
        const auto initial_dpi = win_compat::window_dpi(hwnd_);
        const auto work_width = monitor.rcWork.right - monitor.rcWork.left;
        const auto work_height = monitor.rcWork.bottom - monitor.rcWork.top;
        const auto outer_width = initial_window.right - initial_window.left;
        const auto outer_height = initial_window.bottom - initial_window.top;
        if (outer_width < MulDiv(960, initial_dpi, 96) ||
            outer_height < MulDiv(720, initial_dpi, 96) ||
            outer_width > std::max<LONG>(work_width, MulDiv(960, initial_dpi, 96)) ||
            outer_height > std::max<LONG>(work_height, MulDiv(720, initial_dpi, 96)) ||
            initial_window.left != monitor.rcWork.left + std::max<LONG>(0, (work_width - outer_width) / 2) ||
            initial_window.top != monitor.rcWork.top + std::max<LONG>(0, (work_height - outer_height) / 2))
            return 15;
        RECT preferred_client{0, 0, MulDiv(1120, initial_dpi, 96), MulDiv(800, initial_dpi, 96)};
        if (!win_compat::adjust_window_rect(&preferred_client, GetWindowLongW(hwnd_, GWL_STYLE), FALSE,
                                            GetWindowLongW(hwnd_, GWL_EXSTYLE), initial_dpi)) return 16;
        if (work_width >= preferred_client.right - preferred_client.left &&
            work_height >= preferred_client.bottom - preferred_client.top &&
            (initial_client.right != MulDiv(1120, initial_dpi, 96) ||
             initial_client.bottom != MulDiv(800, initial_dpi, 96))) return 17;
        const auto dpi = win_compat::window_dpi(hwnd_);
        if (!SetWindowPos(hwnd_, nullptr, 0, 0, MulDiv(960, dpi, 96), MulDiv(720, dpi, 96),
                          SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE)) return 7;
        RECT window{};
        RECT client{};
        if (!GetWindowRect(hwnd_, &window) || !GetClientRect(hwnd_, &client) ||
            window.right - window.left != MulDiv(960, dpi, 96) ||
            window.bottom - window.top != MulDiv(720, dpi, 96)) return 8;
        const auto inside = [&](Action action) {
            return std::any_of(hits_.begin(), hits_.end(), [&](const Hit& hit) {
                return hit.action == action && hit.rectangle.left >= 0 && hit.rectangle.top >= 0 &&
                    hit.rectangle.right <= client.right / scale() &&
                    hit.rectangle.bottom <= client.bottom / scale();
            });
        };
        const auto bounds_for = [&](Action action) -> std::optional<D2D1_RECT_F> {
            const auto hit = std::find_if(hits_.begin(), hits_.end(), [&](const Hit& item) {
                return item.action == action;
            });
            if (hit == hits_.end()) return std::nullopt;
            return hit->rectangle;
        };
        const auto has_size = [](const std::optional<D2D1_RECT_F>& bounds,
                                 float expected_width, float expected_height) {
            return bounds && std::fabs((bounds->right - bounds->left) - expected_width) < 0.01f &&
                   std::fabs((bounds->bottom - bounds->top) - expected_height) < 0.01f;
        };
        page_ = Page::migration;
        paint();
        if (!inside(Action::settings) || !inside(Action::strategy_backup)) return 9;
        page_ = Page::settings;
        paint();
        if (!inside(Action::back) || !inside(Action::language) ||
            !inside(Action::settings_migration) || !inside(Action::settings_about)) return 10;
        const auto language = bounds_for(Action::language);
        const auto close_prompt = bounds_for(Action::close_prompt);
        const auto close_action = bounds_for(Action::close_action);
        if (!has_size(language, 178, 43) || !has_size(close_prompt, 178, 43) ||
            !has_size(close_action, 178, 43) ||
            std::fabs(language->right - close_prompt->right) >= 0.01f ||
            std::fabs(language->right - close_action->right) >= 0.01f) return 19;
        settings_section_ = SettingsSection::migration;
        paint();
        if (!inside(Action::profile_minecraft) || !inside(Action::profile_xintinglei) ||
            !inside(Action::profile_browse)) return 11;
        if (!has_size(bounds_for(Action::profile_minecraft), 178, 43) ||
            !has_size(bounds_for(Action::profile_xintinglei), 178, 43) ||
            !has_size(bounds_for(Action::profile_browse), 178, 43)) return 20;
        settings_section_ = SettingsSection::about;
        paint();
        if (!inside(Action::project_link)) return 18;
        if (!has_size(bounds_for(Action::project_link), 178, 43)) return 21;
        page_ = Page::confirmation;
        paint();
        if (!inside(Action::back) || !inside(Action::confirm_start)) return 12;
        if (!has_size(bounds_for(Action::confirm_start), 182, 52)) return 22;
        page_ = Page::error;
        paint();
        if (!inside(Action::back)) return 13;
        if (!has_size(bounds_for(Action::back), 175, 48)) return 23;
        result_ = MigrationResult{};
        result_->log_path = application_directory() / L"Sempervirens.exe";
        result_->backup_root = application_directory();
        page_ = Page::result;
        paint();
        const auto report = bounds_for(Action::report);
        const auto backup = bounds_for(Action::backup);
        if (!has_size(report, 167, 48) || !has_size(backup, 167, 48) ||
            std::fabs(report->bottom - backup->bottom) >= 0.01f ||
            std::fabs((backup->left - report->right) - 14.0f) >= 0.01f) return 24;
        close_dialog_open_ = true;
        english_ = true;
        paint();
        const auto close_cancel = bounds_for(Action::close_cancel);
        const auto close_exit = bounds_for(Action::close_exit);
        const auto close_tray = bounds_for(Action::close_tray);
        if (!close_cancel || !close_exit || !close_tray ||
            std::fabs((close_cancel->right - close_cancel->left) -
                      (close_exit->right - close_exit->left)) >= 0.01f ||
            std::fabs((close_cancel->right - close_cancel->left) -
                      (close_tray->right - close_tray->left)) >= 0.01f ||
            std::fabs((close_cancel->bottom - close_cancel->top) - 49.0f) >= 0.01f ||
            std::fabs((close_exit->bottom - close_exit->top) - 49.0f) >= 0.01f ||
            std::fabs((close_tray->bottom - close_tray->top) - 49.0f) >= 0.01f ||
            std::fabs((close_exit->left - close_cancel->right) - 15.0f) >= 0.01f ||
            std::fabs((close_tray->left - close_exit->right) - 15.0f) >= 0.01f) return 25;
        english_ = false;
        close_dialog_open_ = false;
        prepare_visual_sample(false);
        result_ = MigrationResult{};
        result_->log_path = application_directory() / L"Sempervirens.exe";
        result_->backup_root = application_directory();
        page_ = Page::migration;
        paint();
        const auto footer_report = bounds_for(Action::report);
        const auto footer_backup = bounds_for(Action::backup);
        const auto footer_scan = bounds_for(Action::scan);
        const auto footer_start = bounds_for(Action::start);
        if (!has_size(footer_report, 142, 45) || !has_size(footer_backup, 142, 45) ||
            !has_size(footer_scan, 142, 45) || !has_size(footer_start, 142, 45) ||
            std::fabs((footer_backup->left - footer_report->right) - 12.0f) >= 0.01f ||
            std::fabs((footer_scan->left - footer_backup->right) - 12.0f) >= 0.01f ||
            std::fabs((footer_start->left - footer_scan->right) - 12.0f) >= 0.01f) return 26;
        SendMessageW(hwnd_, WM_ENTERSIZEMOVE, 0, 0);
        for (int step = 0; step < 120; ++step) {
            const auto phase = step < 60 ? step : 119 - step;
            RECT desired{0, 0, 960 + phase * 5, 720 + phase * 2};
            const auto dpi = win_compat::window_dpi(hwnd_);
            if (!win_compat::adjust_window_rect(&desired, GetWindowLongW(hwnd_, GWL_STYLE), FALSE,
                                                GetWindowLongW(hwnd_, GWL_EXSTYLE), dpi)) return 2;
            if (!SetWindowPos(hwnd_, nullptr, 0, 0, desired.right - desired.left,
                              desired.bottom - desired.top,
                              SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE)) return 3;
            InvalidateRect(hwnd_, nullptr, FALSE);
            paint();
            RECT client{};
            if (!GetClientRect(hwnd_, &client) || !target_ || !brush_) return 4;
            const auto pixels = target_->GetPixelSize();
            if (pixels.width != static_cast<UINT>(client.right) ||
                pixels.height != static_cast<UINT>(client.bottom)) return 5;
        }
        SendMessageW(hwnd_, WM_EXITSIZEMOVE, 0, 0);
        return !resizing_ && target_ ? 0 : 6;
    }

    int smoke_window_material(bool required) const {
        DWORD backdrop = 0;
        const auto result = DwmGetWindowAttribute(hwnd_, 38, &backdrop, sizeof(backdrop));
        if (FAILED(result)) return required ? 3 : 0;
        return system_backdrop_active_ && backdrop == 2 ? 0 : 2;
    }

    int smoke_search_input() {
        if (!profile_) return 2;
        MigrationPlan sample{{}, {}, *profile_, {}, {}};
        PlannedOperation ordinary;
        ordinary.name = "Caf\xC3\xA9 \xE8\xAE\xBE\xE7\xBD\xAE";
        ordinary.group = "\xE4\xB8\xAA\xE4\xBA\xBA\xE8\xAE\xBE\xE7\xBD\xAE";
        ordinary.detail = "controller \xF0\x9F\x8E\xAE";
        sample.operations.push_back(std::move(ordinary));
        PlannedOperation server;
        server.name = "Servers";
        server.payload = ServerEntry{"\xE6\x98\x9F\xE6\xB2\xB3\xE6\x9C\x8D", "play.example.cn", "", 0, {}};
        sample.operations.push_back(std::move(server));
        PlannedOperation world;
        world.name = "Worlds";
        world.payload = WorldEntry{L"\u6d4b\u8bd5\u4e16\u754c\u6587\u4ef6\u5939", L"\u5c71\u8c37\u4e4b\u5bb6"};
        sample.operations.push_back(std::move(world));
        plan_ = std::move(sample);
        page_ = Page::migration;
        search_focused_ = true;
        search_undo_.clear();
        search_redo_.clear();

        search_ = L"\u3000 PLAY.example.CN \u3000";
        auto filtered = filtered_operations();
        if (filtered.size() != 1 || filtered[0] != 1) return 3;
        search_ = L"\u3000\t\r\n ";
        if (filtered_operations().size() != 3) return 4;
        search_ = L"caf\u00c9";
        filtered = filtered_operations();
        if (filtered.size() != 1 || filtered[0] != 0) return 5;
        search_ = L"\u4e2a\u4eba\u8bbe\u7f6e";
        if (!filtered_operations().empty()) return 11;
        search_ = L"\u6d4b\u8bd5\u4e16\u754c\u6587\u4ef6\u5939";
        filtered = filtered_operations();
        if (filtered.size() != 1 || filtered[0] != 2) return 6;

        search_.clear();
        character(0xD83C);
        if (!search_.empty() || !pending_search_high_surrogate_) return 7;
        character(0xDFAE);
        filtered = filtered_operations();
        if (search_.size() != 2 || pending_search_high_surrogate_ ||
            filtered.size() != 1 || filtered[0] != 0) return 8;
        character(L'\b');
        if (!search_.empty() || filtered_operations().size() != 3) return 9;

        search_ = L"A\xD83C\xDFAE" L"B";
        search_caret_ = search_.size();
        move_search_caret(previous_search_position(search_caret_), false);
        character(L'\b');
        if (search_ != L"AB" || search_caret_ != 1) return 12;
        search_anchor_ = 0;
        search_caret_ = search_.size();
        character(L'X');
        if (search_ != L"X" || search_caret_ != 1 || search_anchor_) return 13;
        search_ = L"ABC";
        search_caret_ = 1;
        if (!handle_search_key(VK_DELETE) || search_ != L"AC" || search_caret_ != 1) return 14;
        if (!undo_search_edit() || search_ != L"ABC" || search_caret_ != 1) return 15;
        if (!redo_search_edit() || search_ != L"AC" || search_caret_ != 1) return 16;

        search_ = L"alpha beta";
        search_caret_ = search_.size();
        search_anchor_.reset();
        search_scroll_ = 0;
        search_undo_.clear();
        search_redo_.clear();
        paint();
        if (!search_bounds_) return 17;
        const auto bounds = *search_bounds_;
        const auto y = (bounds.top + bounds.bottom) / 2.0f;
        const auto start_x = bounds.left + 15.0f;
        const auto end_x = start_x + measure_search_prefix(5) + 1.0f;
        mouse_down(start_x, y);
        mouse_move(end_x, y);
        mouse_up(end_x, y);
        const auto [drag_start, drag_end] = search_selection();
        if (drag_start != 0 || drag_end != 5 || search_.substr(drag_start, drag_end) != L"alpha" ||
            search_mouse_selecting_ || GetCapture() == hwnd_) return 18;
        const auto beta_x = start_x + measure_search_prefix(8);
        mouse_double_click(beta_x, y);
        const auto [word_start, word_end] = search_selection();
        if (search_.substr(word_start, word_end - word_start) != L"beta") return 19;
        mouse_down(bounds.left - 20.0f, y);
        character(L'x');
        return !search_focused_ && search_ == L"alpha beta" ? 0 : 10;
    }

    int smoke_group_toggle() {
        english_ = false;
        prepare_visual_sample(false);
        if (!plan_ || plan_->operations.size() < 6) return 2;
        for (auto& operation : plan_->operations) operation.selected = false;
        search_.clear();
        search_caret_ = 0;
        const auto all_members = filtered_group_members(0);
        if (all_members != std::vector<int>({0, 1, 2})) return 3;
        paint();
        if (std::any_of(hits_.begin(), hits_.end(), [](const Hit& hit) {
                return hit.action == Action::group_toggle;
            })) return 8;
        toggle_filtered_group(0);
        if (!plan_->operations[0].selected || !plan_->operations[1].selected ||
            !plan_->operations[2].selected || plan_->operations[3].selected) return 4;
        toggle_filtered_group(1);
        if (plan_->operations[0].selected || plan_->operations[1].selected ||
            plan_->operations[2].selected) return 5;
        search_ = L"\u952e\u4f4d";
        search_caret_ = search_.size();
        const auto visible_members = filtered_group_members(0);
        if (visible_members != std::vector<int>({0})) return 6;
        paint();
        if (std::any_of(hits_.begin(), hits_.end(), [](const Hit& hit) {
                return hit.action == Action::group_toggle;
            })) return 9;
        toggle_filtered_group(0);
        return plan_->operations[0].selected && !plan_->operations[1].selected &&
            !plan_->operations[2].selected && !plan_->operations[3].selected ? 0 : 7;
    }

    std::optional<Hit> hit_at(float x, float y) const {
        const auto hit = std::find_if(hits_.rbegin(), hits_.rend(), [&](const Hit& item) {
            return contains(item.rectangle, x, y);
        });
        return hit == hits_.rend() ? std::nullopt : std::optional<Hit>(*hit);
    }

    bool pointer_matches(Action action, int index, bool pressed) const {
        return (pressed ? pressed_action_ : hovered_action_) == action &&
               (pressed ? pressed_index_ : hovered_index_) == index;
    }

    void mouse_move(float x, float y) {
        if (!tracking_mouse_) {
            TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE, hwnd_, 0};
            tracking_mouse_ = TrackMouseEvent(&tracking) != FALSE;
        }
        if (drawer_scroll_dragging_) {
            SetCursor(LoadCursorW(nullptr, IDC_HAND));
            scroll_drawer_to_pointer(y);
            return;
        }
        if (content_scroll_drag_ != ContentScrollDrag::none) {
            SetCursor(LoadCursorW(nullptr, IDC_HAND));
            scroll_content_to_pointer(y);
            return;
        }
        if (search_mouse_selecting_ && search_bounds_) {
            SetCursor(LoadCursorW(nullptr, IDC_IBEAM));
            search_caret_ = search_position_from_x(
                std::max(0.0f, x - search_bounds_->left - 15) + search_scroll_);
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }
        const auto target = hit_at(x, y);
        const auto action = target ? target->action : Action::none;
        const auto index = target ? target->index : -1;
        SetCursor(LoadCursorW(nullptr, action == Action::search ? IDC_IBEAM :
                                      action == Action::none ? IDC_ARROW : IDC_HAND));
        if (action == hovered_action_ && index == hovered_index_) return;
        hovered_action_ = action;
        hovered_index_ = index;
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    void mouse_leave() {
        tracking_mouse_ = false;
        hovered_action_ = Action::none;
        hovered_index_ = -1;
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    void capture_changed() {
        pressed_action_ = Action::none;
        pressed_index_ = -1;
        drawer_scroll_dragging_ = false;
        content_scroll_drag_ = ContentScrollDrag::none;
        search_mouse_selecting_ = false;
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    void deactivate() {
        const bool had_overlay = language_drawer_open_ || source_drawer_open_ || target_drawer_open_;
        language_drawer_open_ = false;
        source_drawer_open_ = false;
        target_drawer_open_ = false;
        finish_drawer_animation();
        finish_language_animation();
        focus_index_ = -1;
        mouse_leave();
        capture_changed();
        if (GetCapture() == hwnd_) ReleaseCapture();
        if (had_overlay) InvalidateRect(hwnd_, nullptr, FALSE);
    }

    void scroll_drawer_to_pointer(float y) {
        finish_scroll_animation();
        if (!drawer_scroll_track_) return;
        const auto& selection = source_drawer_open_ ? source_selection_ : target_selection_;
        if (!selection || selection->candidates.size() <= drawer_visible_rows_) return;
        auto& first = source_drawer_open_ ? source_drawer_first_ : target_drawer_first_;
        const auto maximum = std::max(0.0f, static_cast<float>(selection->candidates.size()) - drawer_visible_rows_);
        const auto track_height = drawer_scroll_track_->bottom - drawer_scroll_track_->top;
        const auto thumb_height = std::min(track_height, std::max(24.0f,
            track_height * drawer_visible_rows_ / static_cast<float>(selection->candidates.size())));
        const auto travel = std::max(1.0f, track_height - thumb_height);
        const auto position = std::clamp(y - drawer_scroll_track_->top - thumb_height / 2.0f,
                                         0.0f, travel);
        const auto next = std::clamp(position * maximum / travel, 0.0f, maximum);
        if (first == next) return;
        first = next;
        focus_index_ = -1;
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    void scroll_content_to_pointer(float y) {
        finish_scroll_animation();
        if (content_scroll_drag_ == ContentScrollDrag::list && list_scroll_track_ &&
            list_scroll_total_ > list_scroll_visible_) {
            const auto track_height = list_scroll_track_->bottom - list_scroll_track_->top;
            const auto thumb_height = std::min(track_height, std::max(27.0f,
                track_height * static_cast<float>(list_scroll_visible_) /
                static_cast<float>(list_scroll_total_)));
            const auto travel = std::max(1.0f, track_height - thumb_height);
            const auto maximum = list_scroll_total_ - list_scroll_visible_;
            const auto position = std::clamp(y - list_scroll_track_->top - thumb_height / 2.0f,
                                             0.0f, travel);
            list_first_ = std::clamp(position * maximum / travel, 0.0f, maximum);
        } else if (content_scroll_drag_ == ContentScrollDrag::detail && detail_scroll_track_ &&
                   detail_total_height_ > detail_scroll_visible_height_) {
            const auto track_height = detail_scroll_track_->bottom - detail_scroll_track_->top;
            const auto thumb_height = std::min(track_height, std::max(26.0f,
                track_height * detail_scroll_visible_height_ / detail_total_height_));
            const auto travel = std::max(1.0f, track_height - thumb_height);
            const auto maximum = detail_total_height_ - detail_scroll_visible_height_;
            const auto position = std::clamp(y - detail_scroll_track_->top - thumb_height / 2.0f,
                                             0.0f, travel);
            detail_scroll_ = std::clamp(position * maximum / travel, 0.0f, maximum);
        } else if (content_scroll_drag_ == ContentScrollDrag::error && error_scroll_track_ &&
                   error_total_height_ > error_visible_height_) {
            const auto track_height = error_scroll_track_->bottom - error_scroll_track_->top;
            const auto thumb_height = std::max(25.0f,
                track_height * track_height / error_total_height_);
            const auto travel = std::max(1.0f, track_height - thumb_height);
            const auto maximum = error_total_height_ - error_visible_height_;
            const auto position = std::clamp(y - error_scroll_track_->top - thumb_height / 2.0f,
                                             0.0f, travel);
            error_scroll_ = std::clamp(position * maximum / travel, 0.0f, maximum);
        }
        focus_index_ = -1;
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    void mouse_down(float x, float y) {
        if (search_focused_ && (!search_bounds_ || !contains(*search_bounds_, x, y))) {
            search_focused_ = false;
            search_mouse_selecting_ = false;
            pending_search_high_surrogate_.reset();
            search_anchor_.reset();
            if (GetCapture() == hwnd_) ReleaseCapture();
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
        if (drawer_scroll_track_ && contains(*drawer_scroll_track_, x, y) &&
            (source_drawer_open_ || target_drawer_open_)) {
            drawer_scroll_dragging_ = true;
            pressed_action_ = Action::none;
            pressed_index_ = -1;
            SetCapture(hwnd_);
            scroll_drawer_to_pointer(y);
            return;
        }
        if (source_drawer_open_ || target_drawer_open_) {
            const auto overlay_target = hit_at(x, y);
            const bool belongs_to_drawer = overlay_target &&
                (overlay_target->action == Action::source_drawer ||
                 overlay_target->action == Action::target_drawer ||
                 overlay_target->action == Action::drawer_item ||
                 overlay_target->action == Action::drawer_previous ||
                 overlay_target->action == Action::drawer_next);
            if (!belongs_to_drawer) {
                const bool source = source_drawer_open_;
                start_drawer_animation(source, false);
                source_drawer_open_ = false;
                target_drawer_open_ = false;
                focus_index_ = -1;
                InvalidateRect(hwnd_, nullptr, FALSE);
                return;
            }
        }
        if (language_drawer_open_) {
            const auto overlay_target = hit_at(x, y);
            const bool belongs_to_language = overlay_target &&
                (overlay_target->action == Action::language ||
                 overlay_target->action == Action::language_zh ||
                 overlay_target->action == Action::language_en);
            if (!belongs_to_language) {
                start_language_animation(false);
                language_drawer_open_ = false;
                focus_index_ = -1;
                InvalidateRect(hwnd_, nullptr, FALSE);
                return;
            }
        }
        if (list_scroll_track_ && contains(*list_scroll_track_, x, y)) {
            content_scroll_drag_ = ContentScrollDrag::list;
            pressed_action_ = Action::none;
            pressed_index_ = -1;
            SetCapture(hwnd_);
            scroll_content_to_pointer(y);
            return;
        }
        if (detail_scroll_track_ && contains(*detail_scroll_track_, x, y)) {
            content_scroll_drag_ = ContentScrollDrag::detail;
            pressed_action_ = Action::none;
            pressed_index_ = -1;
            SetCapture(hwnd_);
            scroll_content_to_pointer(y);
            return;
        }
        if (error_scroll_track_ && contains(*error_scroll_track_, x, y)) {
            content_scroll_drag_ = ContentScrollDrag::error;
            pressed_action_ = Action::none;
            pressed_index_ = -1;
            SetCapture(hwnd_);
            scroll_content_to_pointer(y);
            return;
        }
        const auto target = hit_at(x, y);
        if (!target) return;
        if (target->action == Action::search) {
            search_focused_ = true;
            pending_search_high_surrogate_.reset();
            const auto position = search_position_from_x(
                std::max(0.0f, x - target->rectangle.left - 15) + search_scroll_);
            if ((GetKeyState(VK_SHIFT) & 0x8000) != 0) {
                if (!search_anchor_) search_anchor_ = search_caret_;
            } else search_anchor_ = position;
            search_caret_ = position;
            search_mouse_selecting_ = true;
            pressed_action_ = Action::none;
            pressed_index_ = -1;
            SetCapture(hwnd_);
            focus_index_ = -1;
            for (std::size_t index = 0; index < hits_.size(); ++index)
                if (hits_[index].action == Action::search) {
                    NotifyWinEvent(EVENT_OBJECT_FOCUS, hwnd_, OBJID_CLIENT,
                                   static_cast<LONG>(index + 1));
                    break;
                }
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }
        pressed_action_ = target->action;
        pressed_index_ = target->index;
        SetCapture(hwnd_);
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    void mouse_up(float x, float y) {
        if (drawer_scroll_dragging_) {
            drawer_scroll_dragging_ = false;
            if (GetCapture() == hwnd_) ReleaseCapture();
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }
        if (content_scroll_drag_ != ContentScrollDrag::none) {
            content_scroll_drag_ = ContentScrollDrag::none;
            if (GetCapture() == hwnd_) ReleaseCapture();
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }
        if (search_mouse_selecting_) {
            search_mouse_selecting_ = false;
            if (search_anchor_ && *search_anchor_ == search_caret_) search_anchor_.reset();
            if (GetCapture() == hwnd_) ReleaseCapture();
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }
        const auto pressed_action = pressed_action_;
        const auto pressed_index = pressed_index_;
        pressed_action_ = Action::none;
        pressed_index_ = -1;
        if (GetCapture() == hwnd_) ReleaseCapture();
        const auto target = hit_at(x, y);
        if (target && target->action == pressed_action && target->index == pressed_index)
            click(x, y);
        else InvalidateRect(hwnd_, nullptr, FALSE);
    }

    void click(float x, float y) {
        for (auto it = hits_.rbegin(); it != hits_.rend(); ++it) {
            if (!contains(it->rectangle, x, y)) continue;
            const auto activated_action = it->action;
            const auto accessible_child = static_cast<LONG>(
                std::distance(hits_.begin(), std::prev(it.base())) + 1);
            if (it->action != Action::search) {
                search_focused_ = false;
                pending_search_high_surrogate_.reset();
                search_anchor_.reset();
            }
            switch (it->action) {
            case Action::settings:
                if (!busy_) {
                    profile_changed_in_settings_ = false;
                    settings_section_ = SettingsSection::general;
                    page_ = Page::settings;
                    start_page_animation(1);
                }
                break;
            case Action::back:
                if (!busy_) {
                    if (page_ == Page::settings) leave_settings();
                    else {
                        page_ = Page::migration;
                        start_page_animation(-1);
                    }
                }
                break;
            case Action::source_browse: if (!busy_) start_discovery(true); break;
            case Action::target_browse: if (!busy_) start_discovery(false); break;
            case Action::source_drawer:
                if (!busy_) {
                    if (!source_selection_ || source_selection_->candidates.empty())
                        start_discovery(true);
                    else {
                        const bool opening = !source_drawer_open_;
                        start_drawer_animation(true, opening);
                        source_drawer_open_ = opening;
                        target_drawer_open_ = false;
                        focus_index_ = -1;
                    }
                }
                break;
            case Action::target_drawer:
                if (!busy_) {
                    if (!target_selection_ || target_selection_->candidates.empty())
                        start_discovery(false);
                    else {
                        const bool opening = !target_drawer_open_;
                        start_drawer_animation(false, opening);
                        target_drawer_open_ = opening;
                        source_drawer_open_ = false;
                        focus_index_ = -1;
                    }
                }
                break;
            case Action::drawer_item:
                if (busy_) break;
                if (it->index >= 0) {
                    start_drawer_animation(true, false);
                    source_index_ = it->index;
                    source_drawer_open_ = false;
                } else {
                    start_drawer_animation(false, false);
                    target_index_ = -it->index - 1;
                    target_drawer_open_ = false;
                }
                plan_.reset();
                status_ = tr(L"实例已切换，请重新扫描。", L"Instance changed. Scan again.");
                if (source_path() && target_path() && profile_) start_job(Job::scan);
                break;
            case Action::drawer_previous:
            case Action::drawer_next: {
                if (busy_) break;
                const bool source = it->index == 0;
                auto& first = source ? source_drawer_first_ : target_drawer_first_;
                const auto& selection = source ? source_selection_ : target_selection_;
                if (selection) {
                    const auto maximum = std::max(0.0f, static_cast<float>(selection->candidates.size()) - drawer_visible_rows_);
                    first = std::clamp(first + (it->action == Action::drawer_next ? drawer_visible_rows_ : -drawer_visible_rows_), 0.0f, maximum);
                }
                break;
            }
            case Action::list_item: selected_operation_ = it->index; detail_scroll_ = 0; break;
            case Action::search:
                search_focused_ = true;
                pending_search_high_surrogate_.reset();
                search_anchor_.reset();
                search_caret_ = search_position_from_x(
                    std::max(0.0f, x - it->rectangle.left - 15) + search_scroll_);
                focus_index_ = -1;
                break;
            case Action::list_toggle:
                if (plan_ && it->index >= 0 && it->index < static_cast<int>(plan_->operations.size())) {
                    auto& operation = plan_->operations[it->index];
                    if (selectable(operation.status)) operation.selected = !operation.selected;
                    selected_operation_ = it->index;
                    detail_scroll_ = 0;
                }
                break;
            case Action::group_toggle:
                toggle_filtered_group(it->index);
                selected_operation_ = it->index;
                detail_scroll_ = 0;
                break;
            case Action::scan:
                if (busy_) cancel_read_only_job();
                else if (source_path() && target_path() && profile_) start_job(Job::scan);
                break;
            case Action::start:
                if (!busy_ && plan_ && plan_->valid()) {
                    page_ = Page::confirmation;
                    start_page_animation(1);
                }
                break;
            case Action::technical_toggle:
                technical_expanded_ = !technical_expanded_;
                detail_scroll_ = 0;
                break;
            case Action::confirm_start: if (!busy_ && plan_ && plan_->valid()) start_job(Job::migrate); break;
            case Action::strategy_backup: if (!busy_) strategy_ = OverwriteStrategy::backup_and_replace; break;
            case Action::strategy_replace: if (!busy_) strategy_ = OverwriteStrategy::replace; break;
            case Action::strategy_skip: if (!busy_) strategy_ = OverwriteStrategy::skip; break;
            case Action::settings_general:
                if (settings_section_ != SettingsSection::general) {
                    start_settings_section_animation(-1);
                    settings_section_ = SettingsSection::general;
                }
                break;
            case Action::settings_migration:
                if (settings_section_ != SettingsSection::migration) {
                    start_settings_section_animation(settings_section_ == SettingsSection::about ? -1 : 1);
                    settings_section_ = SettingsSection::migration;
                }
                break;
            case Action::settings_about:
                if (settings_section_ != SettingsSection::about) {
                    start_settings_section_animation(1);
                    settings_section_ = SettingsSection::about;
                }
                break;
            case Action::language: {
                const bool opening = !language_drawer_open_;
                start_language_animation(opening);
                language_drawer_open_ = opening;
                focus_index_ = -1;
                break;
            }
            case Action::language_zh:
                english_ = false;
                start_language_animation(false);
                language_drawer_open_ = false;
                save_settings();
                break;
            case Action::language_en:
                english_ = true;
                start_language_animation(false);
                language_drawer_open_ = false;
                save_settings();
                break;
            case Action::close_prompt: prompt_on_close_ = !prompt_on_close_; save_settings(); break;
            case Action::close_action: close_to_tray_ = !close_to_tray_; save_settings(); break;
            case Action::project_link: {
                const auto result = reinterpret_cast<INT_PTR>(
                    ShellExecuteW(hwnd_, L"open", project_url, nullptr, nullptr, SW_SHOWNORMAL));
                if (!shell_launch_succeeded(result)) {
                    const auto message = project_open_failure_message(english_);
                    MessageBoxW(hwnd_, message.c_str(), L"Sempervirens", MB_OK | MB_ICONINFORMATION);
                }
                break;
            }
            case Action::update_check: start_update_check(true); break;
            case Action::update_download: start_update_download(); break;
            case Action::update_restart: restart_to_update(); break;
            case Action::update_auto:
                auto_check_updates_ = !auto_check_updates_;
                save_settings();
                break;
            case Action::profile_browse: if (!busy_) select_profile(); break;
            case Action::profile_reset: if (!busy_) reset_profile(); break;
            case Action::profile_xintinglei: if (!busy_) select_builtin_profile(BuiltInProfile::xintinglei); break;
            case Action::profile_minecraft: if (!busy_) select_builtin_profile(BuiltInProfile::minecraft); break;
            case Action::welcome_continue:
                if (onboarding_ && profile_) {
                    onboarding_ = false;
                    profile_changed_in_settings_ = false;
                    status_ = tr(L"规则已保存。请选择迁出和迁入实例。",
                                 L"Rules saved. Choose source and destination instances.");
                    save_settings();
                    page_ = Page::migration;
                    start_page_animation(1);
                }
                break;
            case Action::report:
                if (result_) open_in_explorer(hwnd_, result_->log_path);
                break;
            case Action::backup:
                if (result_ && result_->backup_root)
                    open_in_explorer(hwnd_, *result_->backup_root);
                break;
            case Action::close_remember:
                remember_close_choice_ = !remember_close_choice_;
                break;
            case Action::close_tray:
                close_dialog_open_ = false;
                InvalidateRect(hwnd_, nullptr, FALSE);
                UpdateWindow(hwnd_);
                NotifyWinEvent(EVENT_SYSTEM_DIALOGEND, hwnd_, OBJID_WINDOW, CHILDID_SELF);
                if (remember_close_choice_) {
                    prompt_on_close_ = false;
                    close_to_tray_ = true;
                    save_settings();
                }
                minimize_to_tray();
                break;
            case Action::close_exit:
                close_dialog_open_ = false;
                NotifyWinEvent(EVENT_SYSTEM_DIALOGEND, hwnd_, OBJID_WINDOW, CHILDID_SELF);
                if (remember_close_choice_) {
                    prompt_on_close_ = false;
                    close_to_tray_ = false;
                    save_settings();
                }
                exit_now();
                break;
            case Action::close_cancel:
                close_dialog_open_ = false;
                InvalidateRect(hwnd_, nullptr, FALSE);
                UpdateWindow(hwnd_);
                NotifyWinEvent(EVENT_SYSTEM_DIALOGEND, hwnd_, OBJID_WINDOW, CHILDID_SELF);
                break;
            default: break;
            }
            if (activated_action == Action::search)
                NotifyWinEvent(EVENT_OBJECT_FOCUS, hwnd_, OBJID_CLIENT, accessible_child);
            if (accessibility_ && activated_action != Action::close_tray &&
                activated_action != Action::close_exit && activated_action != Action::close_cancel)
                NotifyWinEvent(EVENT_OBJECT_STATECHANGE, hwnd_, OBJID_CLIENT, accessible_child);
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }
        if (!close_dialog_open_) {
            if (source_drawer_open_ || target_drawer_open_)
                start_drawer_animation(source_drawer_open_, false);
            if (language_drawer_open_) start_language_animation(false);
            source_drawer_open_ = target_drawer_open_ = language_drawer_open_ = false;
        }
        search_focused_ = false;
        pending_search_high_surrogate_.reset();
        search_anchor_.reset();
        focus_index_ = -1;
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    void mouse_double_click(float x, float y) {
        const auto target = hit_at(x, y);
        if (!target) return;
        if (target->action != Action::search) {
            mouse_down(x, y);
            return;
        }
        if (search_.empty()) return;
        search_focused_ = true;
        pending_search_high_surrogate_.reset();
        auto start = search_position_from_x(
            std::max(0.0f, x - target->rectangle.left - 15) + search_scroll_);
        if (start == search_.size() && start > 0) start = previous_search_position(start);
        const bool whitespace = start < search_.size() && search_whitespace(search_[start]);
        auto end = start;
        while (start > 0) {
            const auto previous = previous_search_position(start);
            if (search_whitespace(search_[previous]) != whitespace) break;
            start = previous;
        }
        while (end < search_.size() && search_whitespace(search_[end]) == whitespace)
            end = next_search_position(end);
        search_anchor_ = start;
        search_caret_ = end;
        focus_index_ = -1;
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    SearchEditState current_search_state() const {
        return {search_, search_caret_, search_anchor_};
    }

    void remember_search_edit() {
        const auto state = current_search_state();
        if (search_undo_.empty() || search_undo_.back().text != state.text ||
            search_undo_.back().caret != state.caret || search_undo_.back().anchor != state.anchor) {
            search_undo_.push_back(state);
            if (search_undo_.size() > 64) search_undo_.erase(search_undo_.begin());
        }
        search_redo_.clear();
    }

    void restore_search_state(const SearchEditState& state) {
        search_ = state.text;
        search_caret_ = std::min(state.caret, search_.size());
        search_anchor_ = state.anchor && *state.anchor <= search_.size() ? state.anchor : std::nullopt;
        pending_search_high_surrogate_.reset();
        search_changed();
    }

    bool undo_search_edit() {
        if (search_undo_.empty()) return false;
        search_redo_.push_back(current_search_state());
        const auto state = std::move(search_undo_.back());
        search_undo_.pop_back();
        restore_search_state(state);
        return true;
    }

    bool redo_search_edit() {
        if (search_redo_.empty()) return false;
        search_undo_.push_back(current_search_state());
        const auto state = std::move(search_redo_.back());
        search_redo_.pop_back();
        restore_search_state(state);
        return true;
    }

    void character(wchar_t character) {
        if (!search_focused_ || page_ != Page::migration) return;
        if (character == L'\b') {
            if (pending_search_high_surrogate_) pending_search_high_surrogate_.reset();
            else {
                const auto [start, end] = search_selection();
                if (start == end && search_caret_ == 0) return;
                remember_search_edit();
                if (!delete_search_selection()) {
                    const auto previous = previous_search_position(search_caret_);
                    search_.erase(previous, search_caret_ - previous);
                    search_caret_ = previous;
                }
            }
        } else if (high_surrogate(character)) {
            pending_search_high_surrogate_ = character;
            return;
        } else if (low_surrogate(character)) {
            if (!pending_search_high_surrogate_) return;
            const wchar_t pair[] = {*pending_search_high_surrogate_, character};
            pending_search_high_surrogate_.reset();
            remember_search_edit();
            replace_search_selection(std::wstring_view(pair, 2));
        } else if (character >= L' ' && character != 0x7F) {
            pending_search_high_surrogate_.reset();
            remember_search_edit();
            replace_search_selection(std::wstring_view(&character, 1));
        } else return;
        search_changed();
    }

    std::vector<int> filtered_operations() const {
        std::vector<int> result;
        if (!plan_) return result;
        const auto needle = trim_search(search_);
        for (std::size_t index = 0; index < plan_->operations.size(); ++index) {
            const auto& operation = plan_->operations[index];
            auto text = display_operation_name(operation, english_) + L" ";
            if (const auto* server = std::get_if<ServerEntry>(&operation.payload))
                text += L" " + utf8_to_wide(server->address);
            if (const auto* world = std::get_if<WorldEntry>(&operation.payload))
                text += L" " + world->folder_name;
            text += L" " + utf8_to_wide(operation.detail);
            if (current_culture_contains(text, needle))
                result.push_back(static_cast<int>(index));
        }
        return result;
    }

    std::vector<int> filtered_group_members(int operation_index) const {
        std::vector<int> result;
        if (!plan_ || operation_index < 0 ||
            operation_index >= static_cast<int>(plan_->operations.size())) return result;
        const auto group = display_operation_group(plan_->operations[operation_index], english_);
        for (const auto index : filtered_operations()) {
            if (current_culture_equal(
                    display_operation_group(plan_->operations[index], english_), group))
                result.push_back(index);
        }
        return result;
    }

    void toggle_filtered_group(int operation_index) {
        if (!plan_) return;
        const auto members = filtered_group_members(operation_index);
        const bool any_selected = std::any_of(members.begin(), members.end(), [&](int index) {
            const auto& operation = plan_->operations[index];
            return selectable(operation.status) && operation.selected;
        });
        for (const auto index : members) {
            auto& operation = plan_->operations[index];
            if (selectable(operation.status)) operation.selected = !any_selected;
        }
    }

    std::size_t previous_search_position(std::size_t position) const {
        position = std::min(position, search_.size());
        if (position >= 2 && low_surrogate(search_[position - 1]) &&
            high_surrogate(search_[position - 2])) return position - 2;
        return position > 0 ? position - 1 : 0;
    }

    std::size_t next_search_position(std::size_t position) const {
        position = std::min(position, search_.size());
        if (position + 1 < search_.size() && high_surrogate(search_[position]) &&
            low_surrogate(search_[position + 1])) return position + 2;
        return std::min(position + 1, search_.size());
    }

    std::pair<std::size_t, std::size_t> search_selection() const {
        if (!search_anchor_) return {search_caret_, search_caret_};
        return std::minmax(*search_anchor_, search_caret_);
    }

    bool delete_search_selection() {
        const auto [start, end] = search_selection();
        if (start == end) return false;
        search_.erase(start, end - start);
        search_caret_ = start;
        search_anchor_.reset();
        return true;
    }

    void replace_search_selection(std::wstring_view value) {
        const auto [start, end] = search_selection();
        std::wstring printable;
        printable.reserve(value.size());
        for (const auto character : value)
            if (character >= L' ' && character != 0x7F) printable.push_back(character);
        const auto retained = search_.size() - (end - start);
        const auto available = retained < 32767 ? 32767 - retained : 0;
        if (printable.size() > available) {
            printable.resize(available);
            if (!printable.empty() && high_surrogate(printable.back())) printable.pop_back();
        }
        search_.replace(start, end - start, printable);
        search_caret_ = start + printable.size();
        search_anchor_.reset();
    }

    void search_changed() {
        search_caret_ = std::min(search_caret_, search_.size());
        list_first_ = 0;
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    void move_search_caret(std::size_t position, bool extend) {
        position = std::min(position, search_.size());
        if (position > 0 && position < search_.size() && low_surrogate(search_[position]) &&
            high_surrogate(search_[position - 1])) --position;
        if (extend) {
            if (!search_anchor_) search_anchor_ = search_caret_;
        } else search_anchor_.reset();
        search_caret_ = position;
    }

    std::size_t previous_search_word(std::size_t position) const {
        auto next = std::min(position, search_.size());
        while (next > 0) {
            const auto previous = previous_search_position(next);
            if (!search_whitespace(search_[previous])) break;
            next = previous;
        }
        while (next > 0) {
            const auto previous = previous_search_position(next);
            if (search_whitespace(search_[previous])) break;
            next = previous;
        }
        return next;
    }

    std::size_t next_search_word(std::size_t position) const {
        auto next = std::min(position, search_.size());
        while (next < search_.size() && !search_whitespace(search_[next]))
            next = next_search_position(next);
        while (next < search_.size() && search_whitespace(search_[next]))
            next = next_search_position(next);
        return next;
    }

    bool copy_search_selection() {
        const auto [start, end] = search_selection();
        if (start == end || !OpenClipboard(hwnd_)) return false;
        const auto value = search_.substr(start, end - start);
        const auto memory = GlobalAlloc(GMEM_MOVEABLE, (value.size() + 1) * sizeof(wchar_t));
        if (!memory) { CloseClipboard(); return false; }
        auto* destination = static_cast<wchar_t*>(GlobalLock(memory));
        if (!destination) {
            GlobalFree(memory);
            CloseClipboard();
            return false;
        }
        std::copy(value.begin(), value.end(), destination);
        destination[value.size()] = L'\0';
        GlobalUnlock(memory);
        const bool transferred = EmptyClipboard() && SetClipboardData(CF_UNICODETEXT, memory);
        if (!transferred) GlobalFree(memory);
        CloseClipboard();
        return transferred;
    }

    bool paste_search_clipboard() {
        if (!OpenClipboard(hwnd_)) return false;
        std::wstring value;
        if (const auto memory = GetClipboardData(CF_UNICODETEXT)) {
            if (const auto* source = static_cast<const wchar_t*>(GlobalLock(memory))) {
                const auto capacity = GlobalSize(memory) / sizeof(wchar_t);
                value.assign(source, wcsnlen(source, capacity));
                GlobalUnlock(memory);
            }
        }
        CloseClipboard();
        if (value.empty()) return false;
        remember_search_edit();
        replace_search_selection(value);
        search_changed();
        return true;
    }

    bool handle_search_key(WPARAM key) {
        if (!search_focused_ || page_ != Page::migration) return false;
        const bool control = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        const auto [selection_start, selection_end] = search_selection();
        if (control && key == L'A') {
            search_anchor_ = 0;
            search_caret_ = search_.size();
            return true;
        }
        if (control && key == L'Z') { undo_search_edit(); return true; }
        if (control && key == L'Y') { redo_search_edit(); return true; }
        if (control && key == L'C') { copy_search_selection(); return true; }
        if (control && key == L'X') {
            if (copy_search_selection()) {
                remember_search_edit();
                if (delete_search_selection()) search_changed();
            }
            return true;
        }
        if (control && key == L'V') { paste_search_clipboard(); return true; }
        if (key == VK_LEFT || key == VK_RIGHT) {
            std::size_t next = search_caret_;
            if (!shift && selection_start != selection_end)
                next = key == VK_LEFT ? selection_start : selection_end;
            else if (key == VK_LEFT)
                next = control ? previous_search_word(search_caret_) : previous_search_position(search_caret_);
            else next = control ? next_search_word(search_caret_) : next_search_position(search_caret_);
            move_search_caret(next, shift);
            return true;
        }
        if (key == VK_HOME || key == VK_END) {
            move_search_caret(key == VK_HOME ? 0 : search_.size(), shift);
            return true;
        }
        if (key == VK_DELETE) {
            const auto [start, end] = search_selection();
            if (start != end || search_caret_ < search_.size()) {
                remember_search_edit();
                if (!delete_search_selection())
                    search_.erase(search_caret_, next_search_position(search_caret_) - search_caret_);
                search_changed();
            }
            return true;
        }
        if (control && key == VK_BACK) {
            const auto [start, end] = search_selection();
            if (start != end || search_caret_ > 0) {
                remember_search_edit();
                if (!delete_search_selection()) {
                    const auto previous = previous_search_word(search_caret_);
                    search_.erase(previous, search_caret_ - previous);
                    search_caret_ = previous;
                }
                search_changed();
            }
            return true;
        }
        return key == VK_UP || key == VK_DOWN || key == VK_RETURN;
    }

    float measure_search_prefix(std::size_t length) const {
        length = std::min(length, search_.size());
        if (length == 0) return 0;
        IDWriteTextLayout* layout = nullptr;
        if (FAILED(write_factory_->CreateTextLayout(search_.c_str(), static_cast<UINT32>(length),
                search_format_, 32767, 39, &layout))) return 0;
        DWRITE_TEXT_METRICS metrics{};
        const auto okay = SUCCEEDED(layout->GetMetrics(&metrics));
        layout->Release();
        return okay ? metrics.widthIncludingTrailingWhitespace : 0;
    }

    std::size_t search_position_from_x(float x) const {
        if (search_.empty() || x <= 0) return 0;
        IDWriteTextLayout* layout = nullptr;
        if (FAILED(write_factory_->CreateTextLayout(search_.c_str(), static_cast<UINT32>(search_.size()),
                search_format_, 32767, 39, &layout))) return search_.size();
        BOOL trailing = FALSE;
        BOOL inside = FALSE;
        DWRITE_HIT_TEST_METRICS metrics{};
        const auto okay = SUCCEEDED(layout->HitTestPoint(x, 19, &trailing, &inside, &metrics));
        layout->Release();
        if (!okay) return search_.size();
        auto position = static_cast<std::size_t>(metrics.textPosition) +
            (trailing ? static_cast<std::size_t>(metrics.length) : 0);
        position = std::min(position, search_.size());
        if (position > 0 && position < search_.size() && low_surrogate(search_[position]) &&
            high_surrogate(search_[position - 1])) --position;
        return position;
    }

    void position_ime() {
        if (!search_focused_ || page_ != Page::migration || !search_bounds_) return;
        const auto text_width = measure_search_prefix(search_caret_);
        const auto caret_x = MulDiv(static_cast<int>(search_bounds_->left + 15 +
            std::max(0.0f, text_width - search_scroll_)), dpi_, 96);
        const auto text_y = MulDiv(static_cast<int>(search_bounds_->bottom), dpi_, 96);
        const auto candidate_y = MulDiv(372, dpi_, 96);
        const auto context = ImmGetContext(hwnd_);
        if (!context) return;
        COMPOSITIONFORM composition{};
        composition.dwStyle = CFS_POINT;
        composition.ptCurrentPos = {caret_x, text_y};
        ImmSetCompositionWindow(context, &composition);
        CANDIDATEFORM candidate{};
        candidate.dwIndex = 0;
        candidate.dwStyle = CFS_CANDIDATEPOS;
        candidate.ptCurrentPos = {caret_x, candidate_y};
        ImmSetCandidateWindow(context, &candidate);
        ImmReleaseContext(hwnd_, context);
    }

    void close_requested() {
        if (busy_) {
            if (job_ != Job::migrate) cancel_read_only_job_and_close();
            else {
                status_ = tr(L"正在写入迁移文件，请等待完成后再关闭。",
                             L"Migration is writing files. Wait until it finishes before closing.");
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
            return;
        }
        if (prompt_on_close_) {
            close_dialog_open_ = true;
            remember_close_choice_ = false;
            focus_index_ = 3;
            InvalidateRect(hwnd_, nullptr, FALSE);
            UpdateWindow(hwnd_);
            NotifyWinEvent(EVENT_SYSTEM_DIALOGSTART, hwnd_, OBJID_WINDOW, CHILDID_SELF);
            NotifyWinEvent(EVENT_OBJECT_FOCUS, hwnd_, OBJID_CLIENT, focus_index_ + 1);
        } else if (close_to_tray_) minimize_to_tray();
        else exit_now();
    }

    bool query_end_session() {
        if (!busy_) return true;
        if (job_ != Job::migrate) cancel_read_only_job_and_close();
        else {
            status_ = tr(L"正在写入迁移文件，暂时无法注销或关机。",
                         L"Migration is writing files, so sign-out or shutdown is temporarily blocked.");
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
        return false;
    }

    void end_session(bool ending) {
        if (!ending) return;
        remove_tray();
        DestroyWindow(hwnd_);
    }

    void exit_now() {
        if (busy_) {
            if (job_ != Job::migrate) cancel_read_only_job_and_close(true);
            else {
                if (tray_added_) restore_from_tray();
                close_dialog_open_ = false;
                status_ = tr(L"正在写入迁移文件，请等待完成后再关闭。",
                             L"Migration is writing files. Wait until it finishes before closing.");
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
            return;
        }
        if (worker_.joinable()) worker_.request_stop();
        remove_tray();
        DestroyWindow(hwnd_);
    }

    void minimize_to_tray() {
        if (!tray_added_) tray_added_ = add_tray_icon();
        if (tray_added_) ShowWindow(hwnd_, SW_HIDE);
        else status_ = tr(L"无法创建托盘图标，窗口将保持打开。", L"Tray icon could not be created; the window stays open.");
    }

    bool add_tray_icon() {
        NOTIFYICONDATAW data{};
        data.cbSize = sizeof(data);
        data.hWnd = hwnd_;
        data.uID = 1;
        data.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
        data.uCallbackMessage = wm_tray;
        data.hIcon = LoadIconW(instance_, MAKEINTRESOURCEW(1));
        wcscpy_s(data.szTip, L"Sempervirens");
        return Shell_NotifyIconW(NIM_ADD, &data) != FALSE ||
               Shell_NotifyIconW(NIM_MODIFY, &data) != FALSE;
    }

    void taskbar_created() {
        if (!tray_added_) return;
        if (add_tray_icon()) return;
        tray_added_ = false;
        status_ = tr(L"托盘图标未能恢复，窗口已重新打开。",
                     L"The tray icon could not be restored, so the window was reopened.");
        ShowWindow(hwnd_, SW_RESTORE);
        SetForegroundWindow(hwnd_);
    }

    void remove_tray() {
        if (!tray_added_) return;
        NOTIFYICONDATAW data{};
        data.cbSize = sizeof(data);
        data.hWnd = hwnd_;
        data.uID = 1;
        Shell_NotifyIconW(NIM_DELETE, &data);
        tray_added_ = false;
    }

    void restore_from_tray() {
        ShowWindow(hwnd_, SW_RESTORE);
        SetForegroundWindow(hwnd_);
        remove_tray();
    }

    void tray_message(LPARAM event) {
        if (event == WM_LBUTTONDBLCLK) {
            if (single_instance_smoke_) ++single_instance_activations_;
            if (startup_splash_active_) pending_activation_after_splash_ = true;
            else restore_from_tray();
        }
        else if (event == WM_RBUTTONUP || event == WM_CONTEXTMENU) {
            HMENU menu = CreatePopupMenu();
            if (!menu) return;
            AppendMenuW(menu, MF_STRING, tray_open_command, tr(L"打开 Sempervirens", L"Open Sempervirens").c_str());
            AppendMenuW(menu, MF_STRING, tray_exit_command, tr(L"退出 Sempervirens", L"Exit Sempervirens").c_str());
            POINT cursor{};
            GetCursorPos(&cursor);
            SetForegroundWindow(hwnd_);
            TrackPopupMenu(menu, TPM_RIGHTBUTTON, cursor.x, cursor.y, 0, hwnd_, nullptr);
            DestroyMenu(menu);
        }
    }

    void tray_command(UINT id) {
        if (id == tray_open_command) restore_from_tray();
        else if (id == tray_exit_command) exit_now();
    }

    void finish_scroll_animation() {
        scroll_animation_started_ = 0;
        animated_scroll_value_ = nullptr;
    }

    float displayed_scroll(float& value) const {
        if (animated_scroll_value_ != &value || scroll_animation_started_ == 0 || value != scroll_animation_to_)
            return value;
        const float t = std::clamp(static_cast<float>(GetTickCount64() - scroll_animation_started_) / 130.0f, 0.0f, 1.0f);
        const float remaining = 1.0f - t;
        return scroll_animation_from_ + (value - scroll_animation_from_) * (1 - remaining * remaining * remaining);
    }

    void scroll_by(float& value, float distance, float maximum) {
        const auto current = displayed_scroll(value);
        const auto next = std::clamp(value + distance, 0.0f, std::max(0.0f, maximum));
        if (value == next) return;
        value = next;
        finish_scroll_animation();
        if (!reduced_motion_ && !resizing_) {
            animated_scroll_value_ = &value;
            scroll_animation_from_ = current;
            scroll_animation_to_ = next;
            scroll_animation_started_ = GetTickCount64();
            fast_animation_timer_ = true;
            SetTimer(hwnd_, animation_timer, 16, nullptr);
        }
        focus_index_ = -1;
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    void scroll(int delta) {
        POINT cursor{};
        GetCursorPos(&cursor);
        ScreenToClient(hwnd_, &cursor);
        scroll_at(delta, cursor.x / scale());
    }

    void scroll_at(int delta, float pointer_x) {
        if (delta == 0 || close_dialog_open_ || language_drawer_open_) return;
        const float pixels = -static_cast<float>(delta) * 48.0f / WHEEL_DELTA;
        if (source_drawer_open_ || target_drawer_open_) {
            const bool source = source_drawer_open_;
            const auto& selection = source ? source_selection_ : target_selection_;
            if (!selection) return;
            auto& first = source ? source_drawer_first_ : target_drawer_first_;
            const auto maximum = std::max(0.0f, static_cast<float>(selection->candidates.size()) - drawer_visible_rows_);
            scroll_by(first, pixels / 70.0f, maximum);
            return;
        }
        if (page_ == Page::error) {
            scroll_by(error_scroll_, pixels, error_total_height_ - error_visible_height_);
            return;
        }
        if (!plan_ || page_ != Page::migration) return;
        if (pointer_x >= (target_ ? target_->GetSize().width * 0.54f : 600.0f)) {
            scroll_by(detail_scroll_, pixels, detail_total_height_ - detail_scroll_visible_height_);
        }
        else {
            const auto total = static_cast<int>(filtered_operations().size());
            scroll_by(list_first_, pixels / 62.0f, total - list_scroll_visible_);
        }
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    void key_down(WPARAM key) {
        finish_scroll_animation();
        if ((source_drawer_open_ || target_drawer_open_) &&
            (key == VK_UP || key == VK_DOWN || key == VK_HOME || key == VK_END))
            finish_drawer_animation();
        if (search_focused_ && handle_search_key(key)) {
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }
        if (key == VK_TAB && !hits_.empty()) {
            search_focused_ = false;
            pending_search_high_surrogate_.reset();
            search_anchor_.reset();
            const auto count = static_cast<int>(hits_.size());
            const auto direction = (GetKeyState(VK_SHIFT) & 0x8000) ? -1 : 1;
            focus_index_ = focus_index_ < 0 ? (direction > 0 ? 0 : count - 1) :
                (focus_index_ + direction + count) % count;
            NotifyWinEvent(EVENT_OBJECT_FOCUS, hwnd_, OBJID_CLIENT, focus_index_ + 1);
        } else if ((key == VK_RETURN || key == VK_SPACE) &&
                   focus_index_ >= 0 && focus_index_ < static_cast<int>(hits_.size())) {
            const auto bounds = hits_[focus_index_].rectangle;
            click((bounds.left + bounds.right) / 2, (bounds.top + bounds.bottom) / 2);
            focus_index_ = -1;
        } else if (key == VK_ESCAPE) {
            if (search_focused_) {
                search_focused_ = false;
                pending_search_high_surrogate_.reset();
                search_anchor_.reset();
                InvalidateRect(hwnd_, nullptr, FALSE);
                return;
            }
            if (close_dialog_open_) {
                close_dialog_open_ = false;
                InvalidateRect(hwnd_, nullptr, FALSE);
                UpdateWindow(hwnd_);
                NotifyWinEvent(EVENT_SYSTEM_DIALOGEND, hwnd_, OBJID_WINDOW, CHILDID_SELF);
            }
            else if (language_drawer_open_ || source_drawer_open_ || target_drawer_open_) {
                if (source_drawer_open_ || target_drawer_open_)
                    start_drawer_animation(source_drawer_open_, false);
                if (language_drawer_open_) start_language_animation(false);
                language_drawer_open_ = source_drawer_open_ = target_drawer_open_ = false;
            }
            else if (page_ != Page::migration && !busy_) {
                if (page_ == Page::settings) leave_settings();
                else {
                    page_ = Page::migration;
                    start_page_animation(-1);
                }
            }
            focus_index_ = -1;
        } else if ((source_drawer_open_ || target_drawer_open_) &&
                   (key == VK_UP || key == VK_DOWN || key == VK_HOME || key == VK_END)) {
            const bool source = source_drawer_open_;
            const auto& selection = source ? source_selection_ : target_selection_;
            if (!selection || selection->candidates.empty()) return;
            const auto total = static_cast<int>(selection->candidates.size());
            int current = -1;
            if (focus_index_ >= 0 && focus_index_ < static_cast<int>(hits_.size()) &&
                hits_[focus_index_].action == Action::drawer_item)
                current = source ? hits_[focus_index_].index : -hits_[focus_index_].index - 1;
            const auto next = key == VK_HOME ? 0 : key == VK_END ? total - 1 :
                current < 0 ? (key == VK_DOWN ? 0 : total - 1) :
                std::clamp(current + (key == VK_DOWN ? 1 : -1), 0, total - 1);
            auto& first = source ? source_drawer_first_ : target_drawer_first_;
            first = std::clamp(first, 0.0f, std::max(0.0f, total - drawer_visible_rows_));
            if (next < first) first = next;
            if (next + 1 > first + drawer_visible_rows_) first = next + 1 - drawer_visible_rows_;
            focus_index_ = -1;
            InvalidateRect(hwnd_, nullptr, FALSE);
            paint();
            for (std::size_t index = 0; index < hits_.size(); ++index) {
                const auto& hit = hits_[index];
                const auto item = source ? hit.index : -hit.index - 1;
                if (hit.action == Action::drawer_item && item == next) {
                    focus_index_ = static_cast<int>(index);
                    NotifyWinEvent(EVENT_OBJECT_FOCUS, hwnd_, OBJID_CLIENT, focus_index_ + 1);
                    break;
                }
            }
        } else if (close_dialog_open_ || language_drawer_open_) {
            if ((key == VK_UP || key == VK_DOWN) && !hits_.empty()) {
                const auto count = static_cast<int>(hits_.size());
                const auto direction = key == VK_DOWN ? 1 : -1;
                focus_index_ = focus_index_ < 0 ? (direction > 0 ? 0 : count - 1) :
                    (focus_index_ + direction + count) % count;
                NotifyWinEvent(EVENT_OBJECT_FOCUS, hwnd_, OBJID_CLIENT, focus_index_ + 1);
            }
        } else if (page_ == Page::migration && plan_ && !plan_->operations.empty() &&
                   (key == VK_UP || key == VK_DOWN)) {
            const auto filtered = filtered_operations();
            if (filtered.empty()) return;
            const auto current = std::find(filtered.begin(), filtered.end(), selected_operation_);
            const auto position = current == filtered.end() ? 0 :
                static_cast<int>(std::distance(filtered.begin(), current));
            const auto next = std::clamp(position + (key == VK_DOWN ? 1 : -1), 0,
                                         static_cast<int>(filtered.size()) - 1);
            selected_operation_ = filtered[next];
            detail_scroll_ = 0;
            const float visible = std::max(1.0f, list_scroll_visible_);
            if (next < list_first_) list_first_ = next;
            if (next + 1 > list_first_ + visible) list_first_ = next - visible + 1;
        }
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    float scale() const { return dpi_ / 96.0f; }
    void set_dpi(UINT dpi) {
        dpi_ = dpi;
        if (target_) target_->SetDpi(static_cast<float>(dpi), static_cast<float>(dpi));
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

private:
    friend class AppAccessible;
    bool ensure_target() {
        if (target_) return true;
        RECT pixels{};
        GetClientRect(hwnd_, &pixels);
        const auto props = D2D1::HwndRenderTargetProperties(hwnd_,
            D2D1::SizeU(std::max(1L, pixels.right), std::max(1L, pixels.bottom)));
        if (FAILED(d2d_factory_->CreateHwndRenderTarget(D2D1::RenderTargetProperties(), props, &target_))) return false;
        target_->SetDpi(static_cast<float>(dpi_), static_cast<float>(dpi_));
        return SUCCEEDED(target_->CreateSolidColorBrush(color(0xFFFFFF), &brush_));
    }

    void fill(const D2D1_RECT_F& bounds, unsigned value, float alpha = 1) {
        brush_->SetColor(color(value, alpha));
        canvas_->FillRectangle(bounds, brush_);
    }
    void panel(const D2D1_RECT_F& bounds, unsigned value = 0x171B20, float radius = 17) {
        brush_->SetColor(color(value));
        const auto rounded = D2D1::RoundedRect(bounds, radius, radius);
        canvas_->FillRoundedRectangle(rounded, brush_);
        brush_->SetColor(color(0xFFFFFF, 0.12f));
        canvas_->DrawRoundedRectangle(rounded, brush_, 1);
    }
    void label(const std::wstring& value, const D2D1_RECT_F& bounds, IDWriteTextFormat* format,
               unsigned ink = 0xF7F9FA) {
        brush_->SetColor(color(ink));
        canvas_->DrawTextW(value.c_str(), static_cast<UINT32>(value.size()), format, bounds, brush_,
                           D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }
    float wrapped_text(const std::wstring& value, float x, float y, float width,
                       IDWriteTextFormat* format, unsigned ink = 0xAAB6C1) {
        if (value.empty()) return 0;
        IDWriteTextLayout* layout = nullptr;
        if (FAILED(write_factory_->CreateTextLayout(value.c_str(), static_cast<UINT32>(value.size()),
                                                     format, std::max(1.0f, width), 12000, &layout))) return 0;
        DWRITE_TEXT_METRICS metrics{};
        layout->GetMetrics(&metrics);
        brush_->SetColor(color(ink));
        canvas_->DrawTextLayout(D2D1::Point2F(x, y), layout, brush_, D2D1_DRAW_TEXT_OPTIONS_CLIP);
        layout->Release();
        return metrics.height;
    }

    float wrapped_text_height(const std::wstring& value, float width, IDWriteTextFormat* format) {
        if (value.empty()) return 0;
        IDWriteTextLayout* layout = nullptr;
        if (FAILED(write_factory_->CreateTextLayout(value.c_str(), static_cast<UINT32>(value.size()),
                                                     format, std::max(1.0f, width), 12000, &layout))) return 0;
        DWRITE_TEXT_METRICS metrics{};
        layout->GetMetrics(&metrics);
        layout->Release();
        return metrics.height;
    }

    void draw_detail(const PlannedOperation& operation, float x, float top, float width, float height) {
        if (!profile_) return;
        const auto explanation = explain_migration(operation, *profile_, strategy_, english_);
        canvas_->PushAxisAlignedClip(rect(x, top, width, height), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        const auto status = operation.status == ScanStatus::conflict ? tr(L"有差异", L"Different") :
            operation.status == ScanStatus::found ? tr(L"可迁移", L"Available") :
            operation.status == ScanStatus::identical ? tr(L"相同", L"Identical") :
            operation.status == ScanStatus::not_found ? tr(L"未找到", L"Missing") :
            operation.status == ScanStatus::config_error ? tr(L"无法读取", L"Read error") :
            tr(L"无内容", L"Empty");
        const auto status_color = operation.status == ScanStatus::conflict ? 0xD7A77D :
            operation.status == ScanStatus::found ? 0x8FBACB :
            operation.status == ScanStatus::identical ? 0x8FB7A0 :
            operation.status == ScanStatus::config_error ? 0xD18A8A : 0x8999A6;
        const float content_width = std::max(1.0f, width - 24.0f);
        const auto title_height = std::max(31.0f, wrapped_text_height(display_operation_name(operation, english_),
            std::max(100.0f, content_width - 122), title_format_));
        const auto section_height = [&](const std::wstring& heading, const std::wstring& body) {
            if (body.empty()) return 0.0f;
            return wrapped_text_height(heading, content_width - 5, caption_format_) + 6 +
                   wrapped_text_height(body, content_width - 5, body_format_) + 18;
        };
        detail_scroll_visible_height_ = height;
        detail_total_height_ = 5 + title_height + 37 +
            section_height(tr(L"差异（目标 → 迁出）", L"Changes (destination → source)"), explanation.difference) +
            section_height(tr(L"处理方式", L"What happens"), explanation.outcome) + 12;
        if (technical_expanded_) detail_total_height_ +=
            section_height(tr(L"技术详情", L"Technical details"), explanation.technical);
        detail_scroll_ = std::clamp(detail_scroll_, 0.0f, std::max(0.0f, detail_total_height_ - height));
        const auto displayed = displayed_scroll(detail_scroll_);
        float y = top + 5 - displayed;
        wrapped_text(display_operation_name(operation, english_), x + 5, y + 3,
                     std::max(100.0f, content_width - 122), title_format_, 0xF7F9FA);
        const auto status_bounds = rect(x + content_width - 104, y, 104, 31);
        panel(status_bounds, 0x20262D, 8);
        label(status, status_bounds, button_format_, status_color);
        y += title_height + 17;
        divider(x + 5, y, x + content_width);
        y += 20;
        auto section = [&](std::wstring_view heading, const std::wstring& body) {
            if (body.empty()) return;
            y += wrapped_text(std::wstring(heading), x + 5, y, content_width - 5,
                              caption_format_, 0xF7F9FA) + 6;
            y += wrapped_text(body, x + 5, y, content_width - 5, body_format_, 0xAAB6C1) + 18;
        };
        section(tr(L"差异（目标 → 迁出）", L"Changes (destination → source)"), explanation.difference);
        section(tr(L"处理方式", L"What happens"), explanation.outcome);
        if (technical_expanded_) section(tr(L"技术详情", L"Technical details"), explanation.technical);
        detail_content_bottom_ = y;
        detail_viewport_bottom_ = top + height;
        canvas_->PopAxisAlignedClip();
        if (detail_total_height_ > height + 2) {
            detail_scroll_track_ = rect(x + width - 18, top + 3, 14, std::max(1.0f, height - 6));
            detail_scroll_visible_height_ = height;
            const auto track_height = std::max(1.0f, height - 6);
            const auto thumb_height = std::min(track_height,
                std::max(26.0f, track_height * height / detail_total_height_));
            const auto available = track_height - thumb_height;
            panel(rect(x + width - 11, top + 3, 4, track_height), 0x27313A, 2);
            panel(rect(x + width - 11, top + 3 + available *
                (displayed / std::max(1.0f, detail_total_height_ - height)), 4, thumb_height),
                0x9FB3BF, 2);
        }
    }
    void button(const D2D1_RECT_F& bounds, const std::wstring& value, Action action,
                bool prominent = false, bool enabled = true, int index = -1) {
        const bool hovered = enabled && pointer_matches(action, index, false);
        const bool pressed = enabled && pointer_matches(action, index, true);
        const auto surface = !enabled ? 0x191D22 : prominent ?
            (pressed ? 0xB8C7CD : hovered ? 0xE5EEF2 : 0xD6E3E9) :
            (pressed ? 0x171C22 : hovered ? 0x2A323B : 0x20262D);
        panel(bounds, surface, 11);
        const auto ink = prominent ? 0x0B0D10 : enabled ? 0xF7F9FA : 0x71808B;
        label(value, bounds, button_format_, ink);
        if (enabled) hits_.push_back({bounds, action, index});
    }
    void divider(float x1, float y, float x2) {
        brush_->SetColor(color(0xFFFFFF, 0.11f));
        canvas_->DrawLine(D2D1::Point2F(x1, y), D2D1::Point2F(x2, y), brush_, 1);
    }
    std::wstring tr(std::wstring_view chinese, std::wstring_view english) const {
        return std::wstring(english_ ? english : chinese);
    }

    void draw_progress_bar(const D2D1_RECT_F& bounds) {
        const auto width = bounds.right - bounds.left;
        brush_->SetColor(color(0x2B343C));
        canvas_->FillRoundedRectangle(D2D1::RoundedRect(bounds, 3, 3), brush_);
        float left = bounds.left;
        float length = 0;
        if (progress_percent_) length = width * std::clamp(*progress_percent_ / 100.0f, 0.0f, 1.0f);
        else {
            length = width * 0.18f;
            if (!reduced_motion_ && !resizing_) {
                const auto cycle = std::fmod(static_cast<float>(GetTickCount64()) / 1350.0f, 2.0f);
                const auto linear = cycle <= 1.0f ? cycle : 2.0f - cycle;
                left += (width - length) * eased_progress(linear);
            }
        }
        if (length > 0) {
            brush_->SetColor(color(0xD6E3E9));
            canvas_->FillRoundedRectangle(D2D1::RoundedRect(
                rect(left, bounds.top, length, bounds.bottom - bounds.top), 3, 3), brush_);
        }
    }

    void draw_handoff(float center_x, float center_y) {
        // A quiet source-to-destination cue: no tile, hit target or hover state.
        const bool instances_selected = source_path().has_value() && target_path().has_value();
        brush_->SetColor(color(instances_selected ? 0xC2CED6 : 0x8999A6));
        canvas_->DrawLine(D2D1::Point2F(center_x - 9, center_y),
                          D2D1::Point2F(center_x + 9, center_y), brush_, 1.6f);
        canvas_->DrawLine(D2D1::Point2F(center_x + 4, center_y - 5),
                          D2D1::Point2F(center_x + 9, center_y), brush_, 1.6f);
        canvas_->DrawLine(D2D1::Point2F(center_x + 9, center_y),
                          D2D1::Point2F(center_x + 4, center_y + 5), brush_, 1.6f);
    }

    void draw_chevron(float center_x, float center_y, bool up) {
        const float direction = up ? -1.0f : 1.0f;
        brush_->SetColor(color(0xD6E3E9));
        canvas_->DrawLine(D2D1::Point2F(center_x - 5.0f, center_y - 2.5f * direction),
                          D2D1::Point2F(center_x, center_y + 2.5f * direction), brush_, 1.8f);
        canvas_->DrawLine(D2D1::Point2F(center_x, center_y + 2.5f * direction),
                          D2D1::Point2F(center_x + 5.0f, center_y - 2.5f * direction), brush_, 1.8f);
    }

    void draw_migration(float width, float height) {
        const float margin = 26;
        branding_.draw(canvas_, rect(margin, 12, 212, 104));
        label(tr(L"选择实例，确认内容，再把熟悉的游戏环境带到新地方。",
                 L"Choose instances, review contents, then bring your setup along."),
              rect(margin, 126, width - 250, 24), body_format_, 0xAAB6C1);
        button(rect(width - margin - 120, 34, 120, 44), tr(L"设置", L"Settings"),
               Action::settings, false, !busy_);

        const float gap = 32;
        const float card_width = (width - margin * 2 - gap) / 2;
        const float left = margin;
        const float right = left + card_width + gap;
        const float card_top = 160;
        const float card_height = 176;
        panel(rect(left, card_top, card_width, card_height));
        panel(rect(right, card_top, card_width, card_height));
        label(tr(L"迁出实例", L"Source instance"), rect(left + 20, card_top + 17, card_width - 160, 30), title_format_);
        label(tr(L"只读取，不改动", L"Read only"), rect(left + 20, card_top + 50, card_width - 160, 25), caption_format_, 0xAAB6C1);
        label(tr(L"迁入实例", L"Destination instance"), rect(right + 20, card_top + 17, card_width - 160, 30), title_format_);
        label(tr(L"选中的内容会写入这里", L"Selected data is written here"),
              rect(right + 20, card_top + 50, card_width - 160, 25), caption_format_, 0xAAB6C1);
        button(rect(left + card_width - 114, card_top + 18, 94, 42), tr(L"浏览", L"Browse"),
               Action::source_browse, false, !busy_);
        button(rect(right + card_width - 114, card_top + 18, 94, 42), tr(L"浏览", L"Browse"),
               Action::target_browse, false, !busy_);
        const auto source_card = rect(left + 18, card_top + 91, card_width - 36, 65);
        const auto target_card = rect(right + 18, card_top + 91, card_width - 36, 65);
        panel(source_card, pointer_matches(Action::source_drawer, -1, true) ? 0x182027 :
                           pointer_matches(Action::source_drawer, -1, false) ? 0x141A20 : 0x101419, 9);
        panel(target_card, pointer_matches(Action::target_drawer, -1, true) ? 0x182027 :
                           pointer_matches(Action::target_drawer, -1, false) ? 0x141A20 : 0x101419, 9);
        if (source_path()) {
            const auto& info = source_selection_->candidates[source_index_];
            label(info.display_name, rect(left + 30, card_top + 99, card_width - 85, 27), body_format_);
            label(info.root.wstring(), rect(left + 30, card_top + 128, card_width - 85, 22), mono_single_format_, 0xAAB6C1);
            if (!source_selection_->candidates.empty()) {
                draw_chevron(left + card_width - 35, card_top + 124, source_drawer_open_);
                if (!busy_) hits_.push_back({source_card, Action::source_drawer});
            }
        } else if (source_selection_ && source_selection_->candidates.size() > 1) {
            label(std::to_wstring(source_selection_->candidates.size()) +
                  tr(L" 个实例 · 点此选择", L" instances · Choose one"),
                  rect(left + 30, card_top + 112, card_width - 85, 28), body_format_);
            draw_chevron(left + card_width - 35, card_top + 124, false);
            if (!busy_) hits_.push_back({source_card, Action::source_drawer});
        } else {
            label(source_selection_ ? tr(L"未发现实例，请重新浏览", L"No instance found. Browse again") :
                                      tr(L"拖入实例文件夹，或点击浏览", L"Drop an instance folder, or browse"),
                  rect(left + 30, card_top + 112, card_width - 60, 28), body_format_, 0xAAB6C1);
            if (!busy_) hits_.push_back({source_card, Action::source_drawer});
        }
        if (target_path()) {
            const auto& info = target_selection_->candidates[target_index_];
            label(info.display_name, rect(right + 30, card_top + 99, card_width - 85, 27), body_format_);
            label(info.root.wstring(), rect(right + 30, card_top + 128, card_width - 85, 22), mono_single_format_, 0xAAB6C1);
            if (!target_selection_->candidates.empty()) {
                draw_chevron(right + card_width - 35, card_top + 124, target_drawer_open_);
                if (!busy_) hits_.push_back({target_card, Action::target_drawer});
            }
        } else if (target_selection_ && target_selection_->candidates.size() > 1) {
            label(std::to_wstring(target_selection_->candidates.size()) +
                  tr(L" 个实例 · 点此选择", L" instances · Choose one"),
                  rect(right + 30, card_top + 112, card_width - 85, 28), body_format_);
            draw_chevron(right + card_width - 35, card_top + 124, false);
            if (!busy_) hits_.push_back({target_card, Action::target_drawer});
        } else {
            label(target_selection_ ? tr(L"未发现实例，请重新浏览", L"No instance found. Browse again") :
                                      tr(L"拖入实例文件夹，或点击浏览", L"Drop an instance folder, or browse"),
                  rect(right + 30, card_top + 112, card_width - 60, 28), body_format_, 0xAAB6C1);
            if (!busy_) hits_.push_back({target_card, Action::target_drawer});
        }
        draw_handoff(width / 2, card_top + 123);

        const float content_top = 352;
        const float footer_top = height - 125;
        const float content_height = std::max(190.0f, footer_top - content_top - 14);
        panel(rect(margin, content_top, width - margin * 2, content_height));
        const float split = width * 0.54f;
        label(tr(L"迁移清单", L"Migration list"), rect(margin + 20, content_top + 17, 200, 31), title_format_);
        const auto search_rect = rect(margin + 218, content_top + 15, split - margin - 234, 39);
        search_bounds_ = search_rect;
        panel(search_rect, 0x101419, 9);
        if (search_focused_) {
            brush_->SetColor(color(0xD6E3E9));
            canvas_->DrawRoundedRectangle(D2D1::RoundedRect(search_rect, 9, 9), brush_, 1.5f);
        }
        const auto search_text_rect = rect(search_rect.left + 15, search_rect.top,
                                           search_rect.right - search_rect.left - 30,
                                           search_rect.bottom - search_rect.top);
        const auto caret_width = measure_search_prefix(search_caret_);
        const auto visible_width = search_text_rect.right - search_text_rect.left;
        if (search_focused_) {
            if (caret_width - search_scroll_ > visible_width - 2)
                search_scroll_ = caret_width - visible_width + 2;
            if (caret_width - search_scroll_ < 0) search_scroll_ = caret_width;
        }
        search_scroll_ = std::max(0.0f, search_scroll_);
        canvas_->PushAxisAlignedClip(search_text_rect, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        if (search_.empty()) {
            if (!search_focused_)
                label(tr(L"搜索内容", L"Search contents"), search_text_rect, search_format_, 0x71808B);
        } else {
            const auto [selection_start, selection_end] = search_selection();
            if (search_focused_ && selection_start != selection_end) {
                const auto selection_left = search_text_rect.left +
                    measure_search_prefix(selection_start) - search_scroll_;
                const auto selection_right = search_text_rect.left +
                    measure_search_prefix(selection_end) - search_scroll_;
                fill(rect(selection_left, search_text_rect.top + 8,
                          std::max(1.0f, selection_right - selection_left), 23), 0x315E78);
            }
            label(search_, rect(search_text_rect.left - search_scroll_, search_text_rect.top,
                                std::max(32767.0f, measure_search_prefix(search_.size()) + 8),
                                search_text_rect.bottom - search_text_rect.top), search_format_);
        }
        if (search_focused_) {
            const auto caret_x = search_text_rect.left + caret_width - search_scroll_;
            brush_->SetColor(color(0xF7F9FA));
            canvas_->DrawLine(D2D1::Point2F(caret_x, search_text_rect.top + 8),
                              D2D1::Point2F(caret_x, search_text_rect.bottom - 8), brush_, 1.2f);
        }
        canvas_->PopAxisAlignedClip();
        hits_.push_back({search_rect, Action::search});
        label(tr(L"内容说明", L"Details"), rect(split + 16, content_top + 17, 180, 31), title_format_);
        brush_->SetColor(color(0xFFFFFF, 0.12f));
        canvas_->DrawLine(D2D1::Point2F(split, content_top + 58),
                          D2D1::Point2F(split, content_top + content_height - 20), brush_, 1);
        divider(margin + 18, content_top + 61, width - margin - 18);
        if (plan_) {
            const auto filtered = filtered_operations();
            const auto list_viewport = rect(margin + 14, content_top + 70,
                split - margin - 29, content_height - 89);
            const float visible = (list_viewport.bottom - list_viewport.top) / 62.0f;
            list_scroll_visible_ = visible;
            list_scroll_total_ = static_cast<int>(filtered.size());
            list_first_ = std::clamp(list_first_, 0.0f, std::max(0.0f, filtered.size() - visible));
            const auto displayed = displayed_scroll(list_first_);
            const int first = static_cast<int>(std::floor(displayed));
            const int last = std::min(static_cast<int>(filtered.size()), static_cast<int>(std::ceil(displayed + visible)));
            canvas_->PushAxisAlignedClip(list_viewport, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
            for (int position = first; position < last; ++position) {
                const auto index = filtered[position];
                const auto& operation = plan_->operations[index];
                const auto group_name = display_operation_group(operation, english_);
                const auto y = content_top + 70 + (position - displayed) * 62;
                const auto row = rect(margin + 15, y, split - margin - 31, 57);
                const bool selected = index == selected_operation_;
                if (selected) {
                    panel(row, 0x20272E, 9);
                    fill(rect(row.left, row.top + 10, 3, row.bottom - row.top - 20), 0x8FBACB);
                } else if (pointer_matches(Action::list_item, index, false)) panel(row, 0x1A2026, 9);
                else fill(row, 0x171B20);
                brush_->SetColor(color(operation.selected ? 0xD6E3E9 : 0x71808B));
                canvas_->DrawRoundedRectangle(D2D1::RoundedRect(rect(row.left + 13, y + 18, 20, 20), 5, 5), brush_, 1.5f);
                if (operation.selected) canvas_->FillRoundedRectangle(
                    D2D1::RoundedRect(rect(row.left + 18, y + 23, 10, 10), 2, 2), brush_);
                label(display_operation_name(operation, english_),
                      rect(row.left + 47, y + 7, row.right - row.left - 165, 26), body_format_);
                label(group_name, rect(row.left + 47, y + 32, row.right - row.left - 165, 19),
                      caption_format_, 0xAAB6C1);
                const auto status = operation.status == ScanStatus::conflict ? tr(L"有差异", L"Different") :
                    operation.status == ScanStatus::found ? tr(L"可迁移", L"Available") :
                    operation.status == ScanStatus::identical ? tr(L"相同", L"Identical") :
                    operation.status == ScanStatus::not_found ? tr(L"未找到", L"Missing") :
                    operation.status == ScanStatus::config_error ? tr(L"无法读取", L"Read error") :
                    tr(L"无内容", L"Empty");
                const auto status_color = operation.status == ScanStatus::conflict ? 0xD7A77D :
                    operation.status == ScanStatus::found ? 0x8FBACB :
                    operation.status == ScanStatus::identical ? 0x8FB7A0 :
                    operation.status == ScanStatus::config_error ? 0xD18A8A : 0x8999A6;
                const auto status_bounds = rect(row.right - 104, y + 15, 92, 28);
                panel(status_bounds, 0x20262D, 8);
                label(status, status_bounds, button_format_, status_color);
                const auto clipped = [&](D2D1_RECT_F bounds, Action action) {
                    bounds.top = std::max(bounds.top, list_viewport.top);
                    bounds.bottom = std::min(bounds.bottom, list_viewport.bottom);
                    if (bounds.bottom > bounds.top) hits_.push_back({bounds, action, index});
                };
                clipped(row, Action::list_item);
                clipped(rect(row.left + 6, y + 10, 34, 34), Action::list_toggle);
            }
            canvas_->PopAxisAlignedClip();
            if (filtered.empty()) label(tr(L"没有匹配的内容。", L"No matching content."),
                                        rect(margin + 22, content_top + 92, split - margin - 43, 50),
                                        body_format_, 0xAAB6C1);
            if (static_cast<int>(filtered.size()) > visible) {
                const auto list_height = content_height - 89;
                list_scroll_track_ = rect(split - 24, content_top + 73, 15, list_height);
                list_scroll_visible_ = visible;
                list_scroll_total_ = static_cast<int>(filtered.size());
                const auto thumb_height = std::min(list_height,
                    std::max(27.0f, list_height * visible / filtered.size()));
                const auto travel = list_height - thumb_height;
                panel(rect(split - 17, content_top + 73, 4, list_height), 0x27313A, 2);
                panel(rect(split - 17, content_top + 73 + travel *
                    (displayed / (filtered.size() - visible)), 4, thumb_height), 0x9FB3BF, 2);
            }
            if (selected_operation_ >= 0 && selected_operation_ < static_cast<int>(plan_->operations.size())) {
                const auto& operation = plan_->operations[selected_operation_];
                const auto detail_x = split + 20;
                const auto detail_width = width - detail_x - margin - 16;
                button(rect(width - margin - 184, content_top + 15, 160, 38),
                       technical_expanded_ ? tr(L"收起技术详情", L"Hide technical details") :
                                             tr(L"查看技术详情", L"Technical details"), Action::technical_toggle);
                draw_detail(operation, detail_x, content_top + 72, detail_width, content_height - 89);
            } else label(tr(L"选择一项内容，查看它会带走什么。", L"Select an item to see what it will bring."),
                         rect(split + 20, content_top + 90, width - split - margin - 40, 80), body_format_, 0xAAB6C1);
        } else {
            label(tr(L"选择两个实例后，扫描可迁移内容。", L"Choose both instances, then scan for transferable data."),
                  rect(margin + 22, content_top + 90, split - margin - 40, 80), body_format_, 0xAAB6C1);
            label(tr(L"选中一项内容，可在这里查看它的用途和差异。",
                     L"Select an item to review its purpose and differences."),
                  rect(split + 20, content_top + 90, width - split - margin - 40, 80), body_format_, 0xAAB6C1);
        }

        divider(margin, footer_top - 1, width - margin);
        label(tr(L"遇到同名内容时", L"Existing content"),
              rect(margin, footer_top + 15, 155, 28), body_format_, 0xAAB6C1);
        const auto small_width = 122.0f;
        button(rect(margin + 164, footer_top + 9, small_width, 40), tr(L"备份后覆盖", L"Back up first"),
               Action::strategy_backup, strategy_ == OverwriteStrategy::backup_and_replace, !busy_);
        button(rect(margin + 294, footer_top + 9, small_width, 40), tr(L"直接覆盖", L"Replace"),
               Action::strategy_replace, strategy_ == OverwriteStrategy::replace, !busy_);
        button(rect(margin + 424, footer_top + 9, small_width, 40), tr(L"保留目标", L"Keep target"),
               Action::strategy_skip, strategy_ == OverwriteStrategy::skip, !busy_);
        if (busy_) draw_progress_bar(rect(margin, footer_top + 54, width - margin * 2, 6));
        std::error_code result_path_error;
        const bool has_report = result_ && fs::is_regular_file(result_->log_path, result_path_error);
        result_path_error.clear();
        const bool has_backup = result_ && result_->backup_root &&
            fs::is_directory(*result_->backup_root, result_path_error);
        constexpr float footer_action_width = 142;
        constexpr float footer_action_gap = 12;
        const int footer_action_count = 2 + (has_report ? 1 : 0) + (has_backup ? 1 : 0);
        const float footer_actions_width = footer_action_count * footer_action_width +
            (footer_action_count - 1) * footer_action_gap;
        if (!status_.empty()) label(status_, rect(margin, height - 63,
                                    width - margin * 2 - footer_actions_width - 24, 51),
                                    caption_format_, 0xAAB6C1);
        const bool can_cancel_scan = busy_ && job_ != Job::migrate && !scan_cancel_requested_;
        float footer_action_x = width - margin - footer_actions_width;
        if (has_report)
            button(rect(footer_action_x, height - 61, footer_action_width, 45),
                   tr(L"查看本次记录", L"Open record"), Action::report, false, !busy_);
        if (has_report) footer_action_x += footer_action_width + footer_action_gap;
        if (has_backup)
            button(rect(footer_action_x, height - 61, footer_action_width, 45),
                   tr(L"查看备份", L"Open backups"), Action::backup, false, !busy_);
        if (has_backup) footer_action_x += footer_action_width + footer_action_gap;
        button(rect(footer_action_x, height - 61, footer_action_width, 45),
               busy_ && job_ != Job::migrate ? tr(L"取消扫描", L"Cancel scan") :
                                               tr(L"扫描内容", L"Scan contents"),
               Action::scan, false,
               can_cancel_scan || (!busy_ && source_path() && target_path() && profile_));
        footer_action_x += footer_action_width + footer_action_gap;
        button(rect(footer_action_x, height - 61, footer_action_width, 45), tr(L"开始迁移", L"Start migration"),
               Action::start, plan_ && plan_->valid(), !busy_ && plan_ && plan_->valid());
        const bool animated_source_drawer = drawer_animation_visible_ && drawer_animation_source_;
        const bool animated_target_drawer = drawer_animation_visible_ && !drawer_animation_source_;
        if ((source_drawer_open_ || animated_source_drawer) && source_selection_) {
            const bool interactive = source_drawer_open_;
            if (interactive) hits_.clear();
            if (interactive && !busy_) {
                hits_.push_back({source_card, Action::source_drawer});
                hits_.push_back({target_card, Action::target_drawer});
            }
            draw_instance_drawer(left + 18, card_top + 158,
                                 card_width - 36, source_selection_->candidates, true,
                                 drawer_reveal(true), interactive);
        } else if ((target_drawer_open_ || animated_target_drawer) && target_selection_) {
            const bool interactive = target_drawer_open_;
            if (interactive) hits_.clear();
            if (interactive && !busy_) {
                hits_.push_back({source_card, Action::source_drawer});
                hits_.push_back({target_card, Action::target_drawer});
            }
            draw_instance_drawer(right + 18, card_top + 158,
                                 card_width - 36, target_selection_->candidates, false,
                                 drawer_reveal(false), interactive);
        }
    }

    void draw_instance_drawer(float x, float y, float width, const std::vector<InstanceInfo>& items,
                              bool source, float reveal, bool interactive) {
        if (items.empty()) return;
        const auto previous_hit_count = hits_.size();
        constexpr float row_pitch = 70.0f;
        constexpr float row_height = 66.0f;
        const auto full_height = std::min(12 + std::min(5.0f, static_cast<float>(items.size())) * row_pitch,
            std::max(82.0f, canvas_->GetSize().height - y - 12));
        drawer_visible_rows_ = (full_height - 12) / row_pitch;
        const bool paged = items.size() > drawer_visible_rows_;
        auto& offset = source ? source_drawer_first_ : target_drawer_first_;
        offset = std::clamp(offset, 0.0f, std::max(0.0f, items.size() - drawer_visible_rows_));
        const auto displayed = displayed_scroll(offset);
        const int first = static_cast<int>(std::floor(displayed));
        const int last = std::min(static_cast<int>(items.size()), static_cast<int>(std::ceil(displayed + drawer_visible_rows_)));
        const auto visible_height = std::max(1.0f, full_height * reveal);
        canvas_->PushAxisAlignedClip(rect(x, y, width, visible_height), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        panel(rect(x, y - (1.0f - reveal) * 8.0f, width, full_height), 0x20262D, 10);
        const auto item_viewport = rect(x + 6, y + 6, width - 12, std::max(1.0f, visible_height - 12));
        canvas_->PushAxisAlignedClip(item_viewport, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        for (int index = first; index < last; ++index) {
            const auto row = rect(x + 7, y + 6 + (index - displayed) * row_pitch,
                                  width - (paged ? 34.0f : 14.0f), row_height);
            const auto action_index = source ? static_cast<int>(index) : -static_cast<int>(index) - 1;
            if (pointer_matches(Action::drawer_item, action_index, true)) panel(row, 0x151C23, 7);
            else if (pointer_matches(Action::drawer_item, action_index, false)) panel(row, 0x202830, 7);
            label(items[index].display_name, rect(row.left + 12, row.top + 5, row.right - row.left - 31, 27), body_format_);
            label(items[index].root.wstring(), rect(row.left + 12, row.top + 32,
                  row.right - row.left - 31, 31), mono_format_, 0xAAB6C1);
            auto hit = row;
            hit.top = std::max(hit.top, item_viewport.top);
            hit.bottom = std::min(hit.bottom, item_viewport.bottom);
            if (hit.bottom > hit.top) hits_.push_back({hit, Action::drawer_item, action_index});
        }
        canvas_->PopAxisAlignedClip();
        if (paged) {
            const auto track_height = std::max(24.0f, full_height - 24.0f);
            const auto scroll_track = rect(x + width - 24, y + 12, 14, track_height);
            if (interactive) drawer_scroll_track_ = scroll_track;
            const auto thumb_height = std::min(track_height, std::max(24.0f,
                track_height * drawer_visible_rows_ / static_cast<float>(items.size())));
            const auto maximum = std::max(0.01f, items.size() - drawer_visible_rows_);
            const auto thumb_top = scroll_track.top +
                (track_height - thumb_height) * displayed / maximum;
            panel(rect(x + width - 18, scroll_track.top, 4, track_height), 0x2B343E, 2);
            panel(rect(x + width - 18, thumb_top, 4, thumb_height), 0x8EA2B0, 2);
        }
        canvas_->PopAxisAlignedClip();
        if (!interactive) hits_.resize(previous_hit_count);
    }

    void draw_welcome(float width, float height) {
        const float content_width = std::min(780.0f, width - 80.0f);
        const float left = (width - content_width) / 2.0f;
        branding_.draw(canvas_, rect(left, 8, 196, 96));
        label(tr(L"选择首次使用的迁移规则", L"Choose your migration rules"),
              rect(left, 111, content_width, 35), title_format_);
        label(tr(L"之后仍可在设置中更改。默认仅迁移原版 Minecraft 内容。",
                 L"You can change this later in Settings. Vanilla Minecraft content is selected by default."),
              rect(left, 150, content_width, 48), body_format_, 0xAAB6C1);

        const auto option = [&](float top, const std::wstring& name, const std::wstring& description,
                                Action action, bool selected) {
            const auto bounds = rect(left, top, content_width, 94);
            panel(bounds, selected ? 0x202A31 : 0x171B20, 15);
            if (selected) fill(rect(bounds.left, bounds.top + 18, 3, 58), 0x8FBACB);
            label(name, rect(bounds.left + 22, bounds.top + 17, content_width - 215, 29), body_format_);
            label(description, rect(bounds.left + 22, bounds.top + 50, content_width - 215, 27),
                  caption_format_, 0xAAB6C1);
            button(rect(bounds.right - 156, bounds.top + 25, 132, 44),
                   selected ? tr(L"已选择", L"Selected") : tr(L"选择", L"Select"),
                   action, selected);
        };
        option(215, tr(L"仅原版 Minecraft", L"Vanilla Minecraft only"),
               tr(L"设置、服务器、截图和单人世界", L"Settings, servers, screenshots, and single-player worlds"),
               Action::profile_minecraft, !profile_file_ && use_minecraft_profile_);
        option(321, tr(L"新亭泪整合实例", L"Xintinglei modpacks"),
               tr(L"包含原版内容和新亭泪整合包数据", L"Vanilla content plus Xintinglei modpack data"),
               Action::profile_xintinglei, !profile_file_ && !use_minecraft_profile_);
        option(427, tr(L"自定义规则文件", L"Custom rules file"),
               profile_file_ && profile_ ? utf8_to_wide(profile_->name) :
                   tr(L"从本地 JSON 文件加载", L"Load a local JSON file"),
               Action::profile_browse, profile_file_.has_value());

        if (!status_.empty())
            label(status_, rect(left, 540, content_width - 220, 29), caption_format_, 0xAAB6C1);
        label(tr(L"首次选择只决定迁移范围，不会立即读取或修改任何实例。",
                 L"This choice only sets the migration scope. No instance is read or changed yet."),
              rect(left, height - 89, content_width - 220, 42), caption_format_, 0x71808B);
        button(rect(left + content_width - 186, height - 97, 186, 52),
               tr(L"继续使用", L"Continue"), Action::welcome_continue, true, profile_.has_value());
    }

    void draw_settings(float width, float height) {
        const float sidebar_width = 232;
        const float main_x = sidebar_width + 34;
        const float main_width = width - main_x - 34;
        fill(rect(0, 0, sidebar_width, height), 0x0F1216);
        fill(rect(sidebar_width - 1, 0, 1, height), 0x2A3036);
        button(rect(20, 24, 150, 42), tr(L"←  返回应用", L"←  Back to app"), Action::back);
        label(tr(L"设置", L"Settings"), rect(22, 94, 186, 37), title_format_);
        label(tr(L"偏好", L"PREFERENCES"), rect(22, 143, 186, 22), caption_format_, 0x71808B);

        const auto nav = [&](float top, const std::wstring& text, Action action, SettingsSection section) {
            const auto bounds = rect(14, top, sidebar_width - 28, 43);
            const bool selected = settings_section_ == section;
            if (selected) {
                panel(bounds, 0x252B31, 9);
                fill(rect(bounds.left, bounds.top + 10, 3, 23), 0x8FBACB);
            } else if (pointer_matches(action, -1, false)) panel(bounds, 0x181D22, 9);
            label(text, rect(bounds.left + 18, bounds.top, bounds.right - bounds.left - 30, 43),
                  control_label_format_, selected ? 0xF7F9FA : 0xAAB6C1);
            hits_.push_back({bounds, action});
        };
        nav(173, tr(L"常规", L"General"), Action::settings_general, SettingsSection::general);
        nav(222, tr(L"迁移规则", L"Migration rules"), Action::settings_migration, SettingsSection::migration);
        nav(271, tr(L"关于与鸣谢", L"About & credits"), Action::settings_about, SettingsSection::about);

        const auto section_reveal = settings_section_reveal();
        D2D1_MATRIX_3X2_F settings_transform{};
        canvas_->GetTransform(&settings_transform);
        canvas_->SetTransform(D2D1::Matrix3x2F::Translation(
            0, (1.0f - section_reveal) * settings_section_animation_direction_ * 9.0f));
        const auto heading = settings_section_ == SettingsSection::general ? tr(L"常规", L"General") :
            settings_section_ == SettingsSection::migration ? tr(L"迁移规则", L"Migration rules") :
            tr(L"关于与鸣谢", L"About & credits");
        label(heading, rect(main_x, 48, main_width, 44), brand_format_);

        if (settings_section_ == SettingsSection::general) {
            label(tr(L"调整界面语言与关闭行为。", L"Control language and closing behavior."),
                  rect(main_x, 98, main_width, 30), body_format_, 0xAAB6C1);
            label(tr(L"应用", L"APPLICATION"), rect(main_x, 151, main_width, 22), caption_format_, 0x71808B);
            const auto card = rect(main_x, 181, main_width, 306);
            panel(card, 0x171B20, 14);
            label(tr(L"界面语言", L"Interface language"), rect(card.left + 20, card.top + 22, 260, 27), body_format_);
            label(tr(L"更改后立即生效", L"Changes apply immediately"),
                  rect(card.left + 20, card.top + 53, main_width - 250, 23), caption_format_, 0xAAB6C1);
            const auto language_button = rect(card.right - 198, card.top + 25, 178, 43);
            button(language_button, english_ ? L"English" : L"简体中文", Action::language);
            draw_chevron(language_button.right - 18,
                         (language_button.top + language_button.bottom) / 2.0f,
                         language_drawer_open_);
            divider(card.left + 20, card.top + 101, card.right - 20);
            label(tr(L"关闭时询问", L"Ask when closing"), rect(card.left + 20, card.top + 124, 260, 27), body_format_);
            wrapped_text(tr(L"关闭窗口时，询问是最小化到托盘还是退出应用。",
                            L"When closing the window, ask whether to minimize to the tray or exit the app."),
                         card.left + 20, card.top + 155, main_width - 250, caption_format_, 0xAAB6C1);
            button(rect(card.right - 198, card.top + 128, 178, 43),
                   prompt_on_close_ ? tr(L"已开启", L"On") : tr(L"已关闭", L"Off"),
                   Action::close_prompt, prompt_on_close_);
            divider(card.left + 20, card.top + 202, card.right - 20);
            label(tr(L"关闭方式", L"Close action"), rect(card.left + 20, card.top + 223, 260, 27), body_format_);
            wrapped_text(tr(L"关闭“关闭时询问”后，按此方式处理。",
                            L"Used when “Ask when closing” is turned off."),
                         card.left + 20, card.top + 254, main_width - 250, caption_format_, 0xAAB6C1);
            button(rect(card.right - 198, card.top + 227, 178, 43),
                   close_to_tray_ ? tr(L"最小化到托盘", L"Minimize to tray") : tr(L"直接退出", L"Exit app"),
                   Action::close_action);
        } else if (settings_section_ == SettingsSection::migration) {
            label(tr(L"管理扫描和迁移时使用的内容规则。", L"Manage the content rules used for scanning and migration."),
                  rect(main_x, 98, main_width, 30), body_format_, 0xAAB6C1);
            label(tr(L"规则文件", L"RULE FILE"), rect(main_x, 151, main_width, 22), caption_format_, 0x71808B);
            const auto card = rect(main_x, 181, main_width, 326);
            panel(card, 0x171B20, 14);
            label(tr(L"仅原版 Minecraft", L"Vanilla Minecraft only"), rect(card.left + 20, card.top + 22, 260, 27), body_format_);
            label(tr(L"设置、服务器、截图和单人世界", L"Settings, servers, screenshots, and single-player worlds"),
                  rect(card.left + 20, card.top + 53, main_width - 250, 23), caption_format_, 0xAAB6C1);
            button(rect(card.right - 198, card.top + 25, 178, 43),
                   profile_file_ || !use_minecraft_profile_ ? tr(L"使用", L"Use") : tr(L"使用中", L"Active"),
                   Action::profile_minecraft, !profile_file_ && use_minecraft_profile_, !busy_);
            divider(card.left + 20, card.top + 101, card.right - 20);
            label(tr(L"新亭泪整合实例", L"Xintinglei modpacks"), rect(card.left + 20, card.top + 122, 260, 27), body_format_);
            label(tr(L"包含原版内容和新亭泪整合包数据", L"Minecraft content plus Xintinglei modpack data"),
                  rect(card.left + 20, card.top + 153, main_width - 250, 23), caption_format_, 0xAAB6C1);
            button(rect(card.right - 198, card.top + 125, 178, 43),
                   profile_file_ || use_minecraft_profile_ ? tr(L"使用", L"Use") : tr(L"使用中", L"Active"),
                   Action::profile_xintinglei, !profile_file_ && !use_minecraft_profile_, !busy_);
            divider(card.left + 20, card.top + 202, card.right - 20);
            label(tr(L"自定义规则", L"Custom rules"), rect(card.left + 20, card.top + 223, 260, 27), body_format_);
            label(profile_file_ && profile_ ? utf8_to_wide(profile_->name) :
                  tr(L"从本地 JSON 文件加载", L"Load rules from a local JSON file"),
                  rect(card.left + 20, card.top + 254, main_width - 250, 23), caption_format_, 0xAAB6C1);
            button(rect(card.right - 198, card.top + 227, 178, 43),
                   profile_file_ ? tr(L"更换文件", L"Change file") : tr(L"选择文件", L"Choose file"),
                   Action::profile_browse, profile_file_.has_value(), !busy_);
        } else {
            label(tr(L"版本、更新与项目信息。", L"Version, updates, and project information."),
                  rect(main_x, 98, main_width, 30), body_format_, 0xAAB6C1);
            const auto card = rect(main_x, 151, main_width, 430);
            panel(card, 0x171B20, 14);
            branding_.draw(canvas_, rect(card.left + 24, card.top + 10, 176, 86));
            wrapped_text(tr(L"支持自定义规则迁移的 Minecraft 实例迁移器。",
                            L"A Minecraft instance migrator with support for custom rules."),
                         card.left + 224, card.top + 27, main_width - 248, body_format_, 0xAAB6C1);
            divider(card.left + 24, card.top + 105, card.right - 24);
            label(tr(L"应用更新", L"App updates"), rect(card.left + 24, card.top + 124, 160, 31), title_format_);
            label(tr(L"当前版本 0.1.0.0 · 正式渠道", L"Version 0.1.0.0 · Stable channel"),
                  rect(card.left + 24, card.top + 160, main_width - 250, 24), caption_format_, 0xAAB6C1);
            button(rect(card.right - 198, card.top + 121, 178, 43),
                   auto_check_updates_ ? tr(L"自动检查：开", L"Auto check: On") : tr(L"自动检查：关", L"Auto check: Off"),
                   Action::update_auto, auto_check_updates_);
            const auto message = !installed_update_layout() ?
                tr(L"安装正式版后可在这里直接更新。", L"Install the release build to enable in-app updates.") :
                update_message_.empty() ? tr(L"更新在后台下载，完成后重启切换版本。",
                                             L"Updates download in the background and apply on restart.") : update_message_;
            label(message, rect(card.left + 24, card.top + 194, main_width - 248, 28), caption_format_, 0xAAB6C1);
            const auto update_action = update_phase_ == UpdatePhase::available ? Action::update_download :
                update_phase_ == UpdatePhase::ready ? Action::update_restart : Action::update_check;
            const auto update_text = update_phase_ == UpdatePhase::checking ? tr(L"正在检查…", L"Checking…") :
                update_phase_ == UpdatePhase::downloading ? tr(L"正在下载…", L"Downloading…") :
                update_phase_ == UpdatePhase::available ? tr(L"下载更新", L"Download update") :
                update_phase_ == UpdatePhase::ready ? tr(L"重启并更新", L"Restart to update") :
                tr(L"检查更新", L"Check for updates");
            button(rect(card.right - 198, card.top + 184, 178, 43), update_text, update_action,
                   update_phase_ == UpdatePhase::ready,
                   installed_update_layout() && update_phase_ != UpdatePhase::checking &&
                   update_phase_ != UpdatePhase::downloading);
            if (update_phase_ == UpdatePhase::downloading) {
                const auto track = rect(card.left + 24, card.top + 233, main_width - 48, 5);
                fill(track, 0x303942);
                const auto progress = std::clamp(update_progress_value_.load(), 0, 100) / 100.0f;
                fill(rect(track.left, track.top, (track.right - track.left) * progress, 5), 0xB8D5E0);
            }
            divider(card.left + 24, card.top + 258, card.right - 24);
            label(tr(L"鸣谢", L"Credits"), rect(card.left + 24, card.top + 280, 140, 31), title_format_);
            wrapped_text(tr(L"由 XintingleiTeam 维护，感谢社区参与测试、反馈与规则整理。",
                            L"Maintained by XintingleiTeam, with thanks to the community for testing, feedback, and rule curation."),
                         card.left + 24, card.top + 322, main_width - 270, body_format_, 0xAAB6C1);
            button(rect(card.right - 198, card.top + 318, 178, 43),
                   tr(L"查看项目仓库", L"Project repository"), Action::project_link);
        }
        if (!status_.empty()) label(status_, rect(main_x, height - 52, main_width, 32), caption_format_, 0xAAB6C1);
        canvas_->SetTransform(settings_transform);
        if (section_reveal < 1.0f)
            fill(rect(main_x, 0, width - main_x, height), 0x0B0D10, (1.0f - section_reveal) * 0.24f);
        if ((language_drawer_open_ || language_animation_visible_) &&
            settings_section_ == SettingsSection::general) {
            const bool interactive = language_drawer_open_;
            if (interactive) {
                hits_.clear();
                hits_.push_back({rect(main_x + main_width - 198, 206, 178, 43), Action::language});
            }
            const auto popup = rect(main_x + main_width - 198, 253, 178, 104);
            const auto reveal = language_reveal();
            canvas_->PushAxisAlignedClip(rect(popup.left, popup.top, popup.right - popup.left,
                                               std::max(1.0f, 104.0f * reveal)),
                                         D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
            panel(rect(popup.left, popup.top - (1.0f - reveal) * 5.0f,
                       popup.right - popup.left, popup.bottom - popup.top), 0x20262D, 10);
            const auto zh = rect(popup.left + 6, popup.top + 5, popup.right - popup.left - 12, 44);
            const auto en = rect(popup.left + 6, popup.top + 55, popup.right - popup.left - 12, 44);
            if (!english_) panel(zh, 0x2B3640, 7);
            else panel(en, 0x2B3640, 7);
            label(L"简体中文", rect(zh.left + 14, zh.top, zh.right - zh.left - 28, 44), control_label_format_);
            label(L"English", rect(en.left + 14, en.top, en.right - en.left - 28, 44), control_label_format_);
            canvas_->PopAxisAlignedClip();
            if (interactive) {
                hits_.push_back({zh, Action::language_zh});
                hits_.push_back({en, Action::language_en});
            } else hits_.push_back({popup, Action::none});
        }
    }

    void draw_confirmation(float width, float height) {
        const float margin = 34;
        button(rect(margin, 32, 106, 44), tr(L"返回", L"Back"), Action::back, false, !busy_);
        label(busy_ ? tr(L"正在迁移", L"Migrating") : tr(L"确认迁移", L"Review migration"),
              rect(margin, 111, width - margin * 2, 51), brand_format_);
        label(busy_ ? tr(L"完成前请保持 Minecraft 与启动器关闭。",
                         L"Keep Minecraft and its launcher closed until this finishes.") :
                      tr(L"请先退出 Minecraft 和启动器。迁出实例不会被修改。",
                         L"Close Minecraft and its launcher first. The source instance stays unchanged."),
              rect(margin, 163, width - margin * 2, 42), body_format_, 0xAAB6C1);
        panel(rect(margin, 222, width - margin * 2, 360));
        const auto count = plan_ ? std::count_if(plan_->operations.begin(), plan_->operations.end(),
            [](const PlannedOperation& op) { return op.selected &&
                (op.status == ScanStatus::found || op.status == ScanStatus::identical || op.status == ScanStatus::conflict); }) : 0;
        const auto conflicts = plan_ ? std::count_if(plan_->operations.begin(), plan_->operations.end(),
            [](const PlannedOperation& op) { return op.selected && op.status == ScanStatus::conflict; }) : 0;
        const auto source_name = source_selection_ && source_index_ >= 0 &&
            source_index_ < static_cast<int>(source_selection_->candidates.size()) ?
            source_selection_->candidates[source_index_].display_name :
            plan_ ? plan_->source_root.filename().wstring() : tr(L"未选择", L"Not selected");
        const auto target_name = target_selection_ && target_index_ >= 0 &&
            target_index_ < static_cast<int>(target_selection_->candidates.size()) ?
            target_selection_->candidates[target_index_].display_name :
            plan_ ? plan_->target_root.filename().wstring() : tr(L"未选择", L"Not selected");
        const float inner_left = margin + 24;
        const float inner_width = width - margin * 2 - 48;
        const float column_width = (inner_width - 54) / 2;
        const float target_left = inner_left + column_width + 54;
        label(tr(L"将迁移的内容", L"Selected content"), rect(inner_left, 245, 320, 35), title_format_);
        label(std::to_wstring(count) + tr(L" 项", L" items"),
              rect(inner_left, 285, inner_width, 44), brand_format_);
        divider(inner_left, 341, width - margin - 24);
        label(tr(L"迁出实例", L"From"), rect(inner_left, 356, column_width, 28), caption_format_, 0xAAB6C1);
        label(tr(L"迁入实例", L"To"), rect(target_left, 356, column_width, 28), caption_format_, 0xAAB6C1);
        label(source_name, rect(inner_left, 386, column_width, 33), title_format_);
        label(L"→", rect(inner_left + column_width + 11, 386, 32, 33), title_format_, 0xD6E3E9);
        label(target_name, rect(target_left, 386, column_width, 33), title_format_);
        divider(inner_left, 430, width - margin - 24);
        label(tr(L"同名内容处理", L"Existing content"), rect(inner_left, 447, inner_width, 28),
              caption_format_, 0xAAB6C1);
        const auto policy = strategy_ == OverwriteStrategy::backup_and_replace ?
            tr(L"先备份目标中的不同内容，再覆盖", L"Back up differing destination files, then replace them") :
            strategy_ == OverwriteStrategy::replace ?
            tr(L"直接覆盖目标中的不同内容，不创建备份", L"Replace differing destination files without a backup") :
            tr(L"保留目标中已有的内容", L"Keep existing destination files");
        wrapped_text(policy, inner_left, 480, inner_width, body_format_, 0xF7F9FA);
        label(std::to_wstring(conflicts) +
              tr(L" 项与目标内容不同", L" item(s) differ from the destination"),
              rect(inner_left, 536, inner_width, 29), caption_format_, 0xAAB6C1);
        if (busy_) {
            label(status_, rect(margin, height - 147, width - margin * 2, 35), body_format_, 0xAAB6C1);
            draw_progress_bar(rect(margin, height - 100, width - margin * 2, 7));
        } else button(rect(width - margin - 182, height - 87, 182, 52),
                      tr(L"确认开始迁移", L"Start migration"), Action::confirm_start, true);
    }

    std::wstring result_section_text() const {
        return result_ && result_->failed_count > 0 ?
            tr(L"请检查失败项目", L"Review the failed items") :
            tr(L"迁移已完成", L"All done");
    }

    void draw_result(float width, float height) {
        const float margin = 34;
        button(rect(margin, 32, 106, 44), tr(L"返回", L"Back"), Action::back);
        label(tr(L"迁移完成", L"Migration complete"), rect(margin, 111, width - margin * 2, 52), brand_format_);
        if (!result_) return;
        const bool has_failures = result_->failed_count > 0;
        panel(rect(margin, 200, width - margin * 2, 248));
        label(result_section_text(), rect(margin + 24, 226, width - margin * 2 - 48, 34),
              title_format_, has_failures ? 0xD7A77D : 0xF7F9FA);
        label(std::to_wstring(result_->success_count) + tr(L" 项成功", L" succeeded"),
              rect(margin + 24, 280, 240, 46), brand_format_);
        label(std::to_wstring(result_->skipped_count) + tr(L" 项跳过", L" skipped"),
              rect(margin + 24, 348, 280, 32), body_format_, 0xAAB6C1);
        label(std::to_wstring(result_->failed_count) + tr(L" 项失败", L" failed"),
              rect(margin + 260, 348, 280, 32), body_format_, 0xAAB6C1);
        label(has_failures ? tr(L"打开本次记录，查看失败项目和原因。", L"Open the record to review failed items and their causes.") :
                             tr(L"请查看本次记录，确认每一项的处理结果。", L"Open the record to review each item's result."),
              rect(margin, 481, width - margin * 2, 41), body_format_, 0xAAB6C1);
        std::error_code file_error;
        if (fs::is_regular_file(result_->log_path, file_error))
            button(rect(margin, height - 94, 167, 48), tr(L"查看本次记录", L"Open record"), Action::report);
        file_error.clear();
        if (result_->backup_root && fs::is_directory(*result_->backup_root, file_error))
            button(rect(margin + 181, height - 94, 167, 48), tr(L"查看备份", L"Open backups"), Action::backup);
    }

    void draw_error(float width, float height) {
        const float margin = 34;
        const float content_width = width - margin * 2;
        const auto detail = migration_error_.empty() ?
            tr(L"迁移未能完成，请检查迁入实例。",
               L"Migration could not finish. Review the destination instance.") : migration_error_;
        fill(rect(margin, 93, content_width - 16, 2), 0x59636C);
        fill(rect(width - margin - 14, 87, 14, 14), 0xD7A77D);
        label(tr(L"迁移已中断", L"Migration stopped"),
              rect(margin, 119, content_width, 51), brand_format_);
        label(tr(L"已停止写入文件。", L"No further files are being written."),
              rect(margin, 177, content_width, 32), body_format_, 0xAAB6C1);
        const float panel_top = 238;
        error_total_height_ = wrapped_text_height(detail, content_width - 60, body_format_);
        const float panel_height = std::clamp(error_total_height_ + 115.0f, 190.0f,
            std::max(190.0f, std::min(300.0f, height - panel_top - 260.0f)));
        error_visible_height_ = panel_height - 107.0f;
        error_scroll_ = std::clamp(error_scroll_, 0.0f,
                                   std::max(0.0f, error_total_height_ - error_visible_height_));
        panel(rect(margin, panel_top, content_width, panel_height));
        label(tr(L"请检查以下问题", L"Check the issue"),
              rect(margin + 24, panel_top + 24, content_width - 48, 34), title_format_);
        divider(margin + 24, panel_top + 72, width - margin - 24);
        canvas_->PushAxisAlignedClip(rect(margin + 24, panel_top + 88,
                                          content_width - 58, error_visible_height_),
                                     D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        wrapped_text(detail, margin + 24, panel_top + 91 - displayed_scroll(error_scroll_), content_width - 60,
                     body_format_, 0xF7F9FA);
        canvas_->PopAxisAlignedClip();
        if (error_total_height_ > error_visible_height_) {
            const auto track_height = error_visible_height_;
            error_scroll_track_ = rect(width - margin - 21, panel_top + 88, 15, track_height);
            const auto thumb_height = std::max(25.0f, track_height * track_height / error_total_height_);
            panel(rect(width - margin - 14, panel_top + 88, 4, track_height), 0x27313A, 2);
            panel(rect(width - margin - 14, panel_top + 88 +
                       (track_height - thumb_height) * displayed_scroll(error_scroll_) /
                       std::max(1.0f, error_total_height_ - track_height), 4, thumb_height),
                  0xD7A77D, 2);
        }
        const auto guidance_top = panel_top + panel_height + 28;
        label(tr(L"接下来", L"Before trying again"),
              rect(margin, guidance_top, content_width, 32), title_format_);
        label(tr(L"先核对迁入实例中已写入的内容。解决问题后，返回清单重新扫描。",
                 L"Review what was written to the destination. Resolve the issue, then return to the list and scan again."),
              rect(margin, guidance_top + 38, content_width, 64), body_format_, 0xAAB6C1);
        button(rect(margin, height - 94, 175, 48),
               tr(L"返回清单", L"Back to list"), Action::back);
    }

    void draw_close_dialog(float width, float height) {
        hits_.clear();
        fill(rect(0, 0, width, height), 0x000000, 0.68f);
        const float x = (width - 580) / 2;
        const float y = (height - 306) / 2;
        panel(rect(x, y, 580, 306), 0x20262D, 20);
        label(tr(L"要离开 Sempervirens 吗？", L"Leave Sempervirens?"),
              rect(x + 28, y + 27, 524, 40), title_format_);
        label(busy_ ? tr(L"正在处理内容。退出会取消当前操作；留在托盘则继续。",
                         L"Work is in progress. Exiting cancels it; the tray keeps it running.") :
                      tr(L"可以留在托盘稍后继续，也可以直接退出。",
                         L"Keep the app in the tray, or exit it now."),
              rect(x + 28, y + 75, 524, 54), body_format_, 0xAAB6C1);
        const auto remember = rect(x + 28, y + 143, 300, 38);
        if (pointer_matches(Action::close_remember, -1, false)) panel(remember, 0x293139, 8);
        panel(rect(remember.left + 7, remember.top + 8, 22, 22), remember_close_choice_ ? 0xD6E3E9 : 0x151A1F, 6);
        if (remember_close_choice_)
            label(L"✓", rect(remember.left + 7, remember.top + 8, 22, 22), button_format_, 0x0B0D10);
        label(tr(L"记住本次选择", L"Remember this choice"),
              rect(remember.left + 39, remember.top, 245, 38), control_label_format_);
        hits_.push_back({remember, Action::close_remember});
        constexpr float action_gap = 15;
        const float action_width = (524 - action_gap * 2) / 3;
        const float action_left = x + 28;
        button(rect(action_left, y + 220, action_width, 49), tr(L"取消", L"Cancel"), Action::close_cancel);
        button(rect(action_left + action_width + action_gap, y + 220, action_width, 49),
               tr(L"直接退出", L"Exit now"), Action::close_exit);
        button(rect(action_left + (action_width + action_gap) * 2, y + 220, action_width, 49),
               tr(L"最小化到托盘", L"Minimize to tray"),
               Action::close_tray, true);
    }

    void draw_contents(float width, float height) {
        hits_.clear();
        drawer_scroll_track_.reset();
        list_scroll_track_.reset();
        detail_scroll_track_.reset();
        error_scroll_track_.reset();
        search_bounds_.reset();
        list_scroll_visible_ = 0;
        list_scroll_total_ = 0;
        detail_scroll_visible_height_ = 0;
        canvas_->Clear(color(0x0B0D10));
        const auto reveal = close_dialog_open_ ? 1.0f : page_reveal();
        D2D1_MATRIX_3X2_F previous_transform{};
        canvas_->GetTransform(&previous_transform);
        canvas_->SetTransform(D2D1::Matrix3x2F::Translation(
            (1.0f - reveal) * page_animation_direction_ * 16.0f, 0));
        if (page_ == Page::welcome) draw_welcome(width, height);
        else if (page_ == Page::settings) draw_settings(width, height);
        else if (page_ == Page::confirmation) draw_confirmation(width, height);
        else if (page_ == Page::result) draw_result(width, height);
        else if (page_ == Page::error) draw_error(width, height);
        else draw_migration(width, height);
        if (focus_index_ >= 0 && focus_index_ < static_cast<int>(hits_.size())) {
            const auto& bounds = hits_[focus_index_].rectangle;
            brush_->SetColor(color(0xD6E3E9));
            canvas_->DrawRoundedRectangle(D2D1::RoundedRect(
                rect(bounds.left - 2, bounds.top - 2, bounds.right - bounds.left + 4,
                     bounds.bottom - bounds.top + 4), 12, 12), brush_, 2);
        }
        canvas_->SetTransform(previous_transform);
        if (reveal < 1.0f) fill(rect(0, 0, width, height), 0x0B0D10, (1.0f - reveal) * 0.34f);
        if (close_dialog_open_) draw_close_dialog(width, height);
    }

    void draw() {
        if (!target_ || !brush_) return;
        canvas_ = target_;
        const auto size = target_->GetSize();
        target_->BeginDraw();
        draw_contents(size.width, size.height);
        if (accessibility_) {
            std::vector<std::pair<Action, int>> layout;
            layout.reserve(hits_.size());
            for (const auto& hit : hits_) layout.emplace_back(hit.action, hit.index);
            if (layout != accessibility_layout_) {
                accessibility_layout_ = std::move(layout);
                NotifyWinEvent(EVENT_OBJECT_REORDER, hwnd_, OBJID_CLIENT, CHILDID_SELF);
            }
        }
        const auto hr = target_->EndDraw();
        canvas_ = nullptr;
        if (hr == D2DERR_RECREATE_TARGET) { branding_.reset_target(); release(brush_); release(target_); InvalidateRect(hwnd_, nullptr, FALSE); }
    }

    HINSTANCE instance_ = nullptr;
    BrandingImage branding_;
    std::optional<fs::path> settings_override_;
    HWND hwnd_ = nullptr;
    UINT dpi_ = 96;
    ID2D1Factory* d2d_factory_ = nullptr;
    IDWriteFactory* write_factory_ = nullptr;
    ID2D1HwndRenderTarget* target_ = nullptr;
    ID2D1RenderTarget* canvas_ = nullptr;
    ID2D1SolidColorBrush* brush_ = nullptr;
    IDWriteTextFormat* brand_format_ = nullptr;
    IDWriteTextFormat* title_format_ = nullptr;
    IDWriteTextFormat* body_format_ = nullptr;
    IDWriteTextFormat* button_format_ = nullptr;
    IDWriteTextFormat* caption_format_ = nullptr;
    IDWriteTextFormat* search_format_ = nullptr;
    IDWriteTextFormat* control_label_format_ = nullptr;
    IDWriteTextFormat* mono_format_ = nullptr;
    IDWriteTextFormat* mono_single_format_ = nullptr;
    IDWriteInlineObject* ellipsis_sign_ = nullptr;
    IAccessible* accessibility_ = nullptr;
    Page page_ = Page::migration;
    SettingsSection settings_section_ = SettingsSection::general;
    OverwriteStrategy strategy_ = OverwriteStrategy::backup_and_replace;
    bool english_ = false;
    bool prompt_on_close_ = true;
    bool close_to_tray_ = false;
    bool auto_check_updates_ = true;
    bool tray_added_ = false;
    bool close_dialog_open_ = false;
    bool remember_close_choice_ = false;
    bool profile_changed_in_settings_ = false;
    bool use_minecraft_profile_ = false;
    bool onboarding_ = false;
    bool startup_splash_active_ = false;
    bool pending_activation_after_splash_ = false;
    bool reduced_motion_ = false;
    bool system_backdrop_active_ = false;
    bool resizing_ = false;
    bool fast_animation_timer_ = false;
    bool tracking_mouse_ = false;
    bool drawer_scroll_dragging_ = false;
    std::optional<D2D1_RECT_F> drawer_scroll_track_;
    ContentScrollDrag content_scroll_drag_ = ContentScrollDrag::none;
    std::optional<D2D1_RECT_F> list_scroll_track_;
    std::optional<D2D1_RECT_F> detail_scroll_track_;
    std::optional<D2D1_RECT_F> error_scroll_track_;
    float list_scroll_visible_ = 0;
    int list_scroll_total_ = 0;
    float detail_scroll_visible_height_ = 0;
    Action hovered_action_ = Action::none;
    int hovered_index_ = -1;
    Action pressed_action_ = Action::none;
    int pressed_index_ = -1;
    std::vector<Hit> hits_;
    std::vector<std::pair<Action, int>> accessibility_layout_;
    std::wstring search_;
    bool search_focused_ = false;
    bool search_mouse_selecting_ = false;
    std::optional<D2D1_RECT_F> search_bounds_;
    std::optional<wchar_t> pending_search_high_surrogate_;
    std::size_t search_caret_ = 0;
    std::optional<std::size_t> search_anchor_;
    float search_scroll_ = 0;
    std::vector<SearchEditState> search_undo_;
    std::vector<SearchEditState> search_redo_;
    bool single_instance_smoke_ = false;
    int single_instance_activations_ = 0;
    int focus_index_ = -1;
    std::optional<MigrationProfile> profile_;
    std::optional<fs::path> profile_file_;
    std::optional<InstanceSelection> source_selection_;
    std::optional<InstanceSelection> target_selection_;
    int source_index_ = -1;
    int target_index_ = -1;
    std::optional<MigrationPlan> plan_;
    std::optional<MigrationResult> result_;
    int selected_operation_ = -1;
    float list_first_ = 0;
    float detail_scroll_ = 0;
    float detail_total_height_ = 0;
    float detail_content_bottom_ = 0;
    float detail_viewport_bottom_ = 0;
    bool technical_expanded_ = false;
    bool busy_ = false;
    bool scan_cancel_requested_ = false;
    bool close_after_read_only_job_ = false;
    bool force_exit_after_read_only_job_ = false;
    Job job_ = Job::none;
    bool source_drawer_open_ = false;
    bool target_drawer_open_ = false;
    static constexpr ULONGLONG drawer_animation_duration_ms = 220;
    ULONGLONG drawer_animation_started_ = 0;
    bool drawer_animation_source_ = true;
    bool drawer_animation_visible_ = false;
    float drawer_animation_from_ = 0.0f;
    float drawer_animation_to_ = 0.0f;
    static constexpr ULONGLONG page_animation_duration_ms = 190;
    ULONGLONG page_animation_started_ = 0;
    float page_animation_direction_ = 1.0f;
    static constexpr ULONGLONG settings_section_animation_duration_ms = 140;
    ULONGLONG settings_section_animation_started_ = 0;
    float settings_section_animation_direction_ = 1.0f;
    float source_drawer_first_ = 0;
    float target_drawer_first_ = 0;
    float drawer_visible_rows_ = 5;
    float* animated_scroll_value_ = nullptr;
    float scroll_animation_from_ = 0;
    float scroll_animation_to_ = 0;
    ULONGLONG scroll_animation_started_ = 0;
    bool language_drawer_open_ = false;
    static constexpr ULONGLONG language_animation_duration_ms = 160;
    ULONGLONG language_animation_started_ = 0;
    bool language_animation_visible_ = false;
    float language_animation_from_ = 0.0f;
    float language_animation_to_ = 0.0f;
    std::optional<int> progress_percent_;
    std::wstring status_;
    std::wstring migration_error_;
    float error_scroll_ = 0;
    float error_total_height_ = 0;
    float error_visible_height_ = 0;
    std::mutex worker_mutex_;
    std::optional<ScanProgress> pending_progress_;
    bool progress_message_queued_ = false;
    std::optional<WorkerOutcome> pending_outcome_;
    std::optional<fs::path> active_discovery_folder_;
    std::jthread worker_;
    UpdatePhase update_phase_ = UpdatePhase::idle;
    bool update_manual_check_ = false;
    std::wstring update_message_;
    std::optional<update::Manifest> update_manifest_;
    std::vector<std::uint8_t> update_manifest_bytes_;
    std::vector<std::uint8_t> update_signature_;
    fs::path update_staging_;
    std::atomic<int> update_progress_value_{-1};
    std::uint64_t update_expected_download_ = 1;
    std::mutex update_mutex_;
    std::optional<UpdateOutcome> pending_update_outcome_;
    std::jthread update_worker_;
};

class AppAccessible final : public IAccessible {
public:
    explicit AppAccessible(App* app) : app_(app) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** value) override {
        if (!value) return E_POINTER;
        *value = nullptr;
        if (IsEqualIID(iid, IID_IUnknown) || IsEqualIID(iid, IID_IDispatch) ||
            IsEqualIID(iid, IID_IAccessible)) {
            *value = static_cast<IAccessible*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override {
        return static_cast<ULONG>(InterlockedIncrement(&references_));
    }
    ULONG STDMETHODCALLTYPE Release() override {
        const auto remaining = static_cast<ULONG>(InterlockedDecrement(&references_));
        if (!remaining) delete this;
        return remaining;
    }
    HRESULT STDMETHODCALLTYPE GetTypeInfoCount(UINT* count) override {
        if (!count) return E_POINTER;
        *count = 0;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetTypeInfo(UINT, LCID, ITypeInfo**) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetIDsOfNames(REFIID, LPOLESTR*, UINT, LCID, DISPID*) override {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE Invoke(DISPID, REFIID, LCID, WORD, DISPPARAMS*, VARIANT*,
                                     EXCEPINFO*, UINT*) override {
        return DISP_E_MEMBERNOTFOUND;
    }

    HRESULT STDMETHODCALLTYPE get_accParent(IDispatch** parent) override {
        if (!parent) return E_POINTER;
        *parent = nullptr;
        return S_FALSE;
    }
    HRESULT STDMETHODCALLTYPE get_accChildCount(long* count) override {
        if (!count) return E_POINTER;
        *count = static_cast<long>(app_->hits_.size());
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_accChild(VARIANT child, IDispatch** result) override {
        if (!result) return E_POINTER;
        *result = nullptr;
        long id = 0;
        return child_id(child, id) && id != CHILDID_SELF ? S_FALSE : E_INVALIDARG;
    }
    HRESULT STDMETHODCALLTYPE get_accName(VARIANT child, BSTR* name) override {
        if (!name) return E_POINTER;
        *name = nullptr;
        long id = 0;
        if (!child_id(child, id)) return E_INVALIDARG;
        const auto value = id == CHILDID_SELF ? self_name() : hit_name(app_->hits_[id - 1]);
        *name = SysAllocStringLen(value.data(), static_cast<UINT>(value.size()));
        return *name ? S_OK : E_OUTOFMEMORY;
    }
    HRESULT STDMETHODCALLTYPE get_accValue(VARIANT child, BSTR* value) override {
        if (!value) return E_POINTER;
        *value = nullptr;
        long id = 0;
        if (!child_id(child, id)) return E_INVALIDARG;
        if (id == CHILDID_SELF || app_->hits_[id - 1].action != Action::search) return S_FALSE;
        *value = SysAllocStringLen(app_->search_.data(), static_cast<UINT>(app_->search_.size()));
        return *value ? S_OK : E_OUTOFMEMORY;
    }
    HRESULT STDMETHODCALLTYPE get_accDescription(VARIANT child, BSTR* description) override {
        if (!description) return E_POINTER;
        *description = nullptr;
        long id = 0;
        if (!child_id(child, id)) return E_INVALIDARG;
        std::wstring value;
        if (id == CHILDID_SELF) value = self_description();
        else {
            const auto& hit = app_->hits_[id - 1];
            const auto action = hit.action;
            if (action == Action::language)
                value = app_->tr(L"当前为简体中文", L"Currently English");
            else if (action == Action::close_prompt)
                value = app_->prompt_on_close_ ?
                    app_->tr(L"当前已开启", L"Currently on") :
                    app_->tr(L"当前已关闭", L"Currently off");
            else if (action == Action::close_action)
                value = app_->close_to_tray_ ?
                    app_->tr(L"当前为最小化到托盘", L"Currently minimize to tray") :
                    app_->tr(L"当前为直接退出", L"Currently exit app");
            else if (action == Action::close_remember)
                value = app_->remember_close_choice_ ?
                    app_->tr(L"已勾选；执行关闭选择后将保存为默认。",
                             L"Checked; the selected close action will become the default.") :
                    app_->tr(L"未勾选；本次选择不会更改设置。",
                             L"Not checked; this choice will not change settings.");
            else if (action == Action::source_drawer) {
                if (const auto path = app_->source_path()) value = path->wstring();
            } else if (action == Action::target_drawer) {
                if (const auto path = app_->target_path()) value = path->wstring();
            } else if (action == Action::drawer_item) {
                const bool source = hit.index >= 0;
                const auto index = source ? hit.index : -hit.index - 1;
                const auto& selection = source ? app_->source_selection_ : app_->target_selection_;
                if (selection && index >= 0 && index < static_cast<int>(selection->candidates.size()))
                    value = selection->candidates[index].root.wstring();
            }
        }
        if (value.empty()) return S_FALSE;
        *description = SysAllocStringLen(value.data(), static_cast<UINT>(value.size()));
        return *description ? S_OK : E_OUTOFMEMORY;
    }
    HRESULT STDMETHODCALLTYPE get_accRole(VARIANT child, VARIANT* role) override {
        if (!role) return E_POINTER;
        VariantInit(role);
        long id = 0;
        if (!child_id(child, id)) return E_INVALIDARG;
        role->vt = VT_I4;
        role->lVal = id == CHILDID_SELF ?
            (app_->close_dialog_open_ ? ROLE_SYSTEM_DIALOG : ROLE_SYSTEM_WINDOW) :
            hit_role(app_->hits_[id - 1]);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_accState(VARIANT child, VARIANT* state) override {
        if (!state) return E_POINTER;
        VariantInit(state);
        long id = 0;
        if (!child_id(child, id)) return E_INVALIDARG;
        state->vt = VT_I4;
        state->lVal = id == CHILDID_SELF ? STATE_SYSTEM_FOCUSABLE : hit_state(id - 1);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_accHelp(VARIANT, BSTR* help) override {
        if (!help) return E_POINTER;
        *help = nullptr;
        return S_FALSE;
    }
    HRESULT STDMETHODCALLTYPE get_accHelpTopic(BSTR* file, VARIANT, long* topic) override {
        if (!file || !topic) return E_POINTER;
        *file = nullptr;
        *topic = 0;
        return S_FALSE;
    }
    HRESULT STDMETHODCALLTYPE get_accKeyboardShortcut(VARIANT, BSTR* shortcut) override {
        if (!shortcut) return E_POINTER;
        *shortcut = nullptr;
        return S_FALSE;
    }
    HRESULT STDMETHODCALLTYPE get_accFocus(VARIANT* focus) override {
        if (!focus) return E_POINTER;
        VariantInit(focus);
        long focused = -1;
        if (app_->search_focused_) {
            for (std::size_t index = 0; index < app_->hits_.size(); ++index)
                if (app_->hits_[index].action == Action::search) { focused = static_cast<long>(index); break; }
        } else if (app_->focus_index_ >= 0 &&
                   app_->focus_index_ < static_cast<int>(app_->hits_.size())) focused = app_->focus_index_;
        if (focused < 0) return S_FALSE;
        focus->vt = VT_I4;
        focus->lVal = focused + 1;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_accSelection(VARIANT* selection) override {
        if (!selection) return E_POINTER;
        VariantInit(selection);
        for (std::size_t index = 0; index < app_->hits_.size(); ++index) {
            const auto& hit = app_->hits_[index];
            if (hit.action == Action::list_item && hit.index == app_->selected_operation_) {
                selection->vt = VT_I4;
                selection->lVal = static_cast<long>(index + 1);
                return S_OK;
            }
        }
        return S_FALSE;
    }
    HRESULT STDMETHODCALLTYPE get_accDefaultAction(VARIANT child, BSTR* action) override {
        if (!action) return E_POINTER;
        *action = nullptr;
        long id = 0;
        if (!child_id(child, id)) return E_INVALIDARG;
        if (id == CHILDID_SELF || app_->hits_[id - 1].action == Action::search) return S_FALSE;
        const auto& hit = app_->hits_[id - 1];
        const bool toggle = hit.action == Action::list_toggle || hit.action == Action::group_toggle ||
            hit.action == Action::close_prompt || hit.action == Action::close_remember ||
            hit.action == Action::update_auto;
        const bool select = hit.action == Action::strategy_backup ||
            hit.action == Action::strategy_replace || hit.action == Action::strategy_skip ||
            hit.action == Action::language_zh || hit.action == Action::language_en ||
            hit.action == Action::profile_xintinglei || hit.action == Action::profile_minecraft ||
            hit.action == Action::list_item || hit.action == Action::drawer_item;
        const auto value = toggle ? app_->tr(L"切换", L"Toggle") :
            select ? app_->tr(L"选择", L"Select") : app_->tr(L"按下", L"Press");
        *action = SysAllocStringLen(value.data(), static_cast<UINT>(value.size()));
        return *action ? S_OK : E_OUTOFMEMORY;
    }
    HRESULT STDMETHODCALLTYPE accSelect(long flags, VARIANT child) override {
        long id = 0;
        if (!child_id(child, id) || id == CHILDID_SELF) return E_INVALIDARG;
        if (!(flags & (SELFLAG_TAKEFOCUS | SELFLAG_TAKESELECTION))) return S_FALSE;
        SetFocus(app_->hwnd_);
        app_->focus_index_ = id - 1;
        app_->search_focused_ = app_->hits_[id - 1].action == Action::search;
        InvalidateRect(app_->hwnd_, nullptr, FALSE);
        NotifyWinEvent(EVENT_OBJECT_FOCUS, app_->hwnd_, OBJID_CLIENT, id);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE accLocation(long* left, long* top, long* width, long* height,
                                          VARIANT child) override {
        if (!left || !top || !width || !height) return E_POINTER;
        long id = 0;
        if (!child_id(child, id)) return E_INVALIDARG;
        if (id == CHILDID_SELF) {
            RECT bounds{};
            if (!GetWindowRect(app_->hwnd_, &bounds)) return E_FAIL;
            *left = bounds.left; *top = bounds.top;
            *width = bounds.right - bounds.left; *height = bounds.bottom - bounds.top;
            return S_OK;
        }
        const auto& bounds = app_->hits_[id - 1].rectangle;
        POINT origin{0, 0};
        if (!ClientToScreen(app_->hwnd_, &origin)) return E_FAIL;
        *left = origin.x + static_cast<long>(std::lround(bounds.left * app_->scale()));
        *top = origin.y + static_cast<long>(std::lround(bounds.top * app_->scale()));
        *width = static_cast<long>(std::lround((bounds.right - bounds.left) * app_->scale()));
        *height = static_cast<long>(std::lround((bounds.bottom - bounds.top) * app_->scale()));
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE accNavigate(long direction, VARIANT start, VARIANT* destination) override {
        if (!destination) return E_POINTER;
        VariantInit(destination);
        long id = 0;
        if (!child_id(start, id)) return E_INVALIDARG;
        long target = 0;
        if (id == CHILDID_SELF && direction == NAVDIR_FIRSTCHILD && !app_->hits_.empty()) target = 1;
        else if (id == CHILDID_SELF && direction == NAVDIR_LASTCHILD && !app_->hits_.empty())
            target = static_cast<long>(app_->hits_.size());
        else if (id > 0 && direction == NAVDIR_NEXT && id < static_cast<long>(app_->hits_.size()))
            target = id + 1;
        else if (id > 1 && direction == NAVDIR_PREVIOUS) target = id - 1;
        else return S_FALSE;
        destination->vt = VT_I4;
        destination->lVal = target;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE accHitTest(long screen_x, long screen_y, VARIANT* child) override {
        if (!child) return E_POINTER;
        VariantInit(child);
        POINT point{screen_x, screen_y};
        if (!ScreenToClient(app_->hwnd_, &point)) return E_FAIL;
        const auto x = point.x / app_->scale();
        const auto y = point.y / app_->scale();
        for (std::size_t offset = 0; offset < app_->hits_.size(); ++offset) {
            const auto index = app_->hits_.size() - 1 - offset;
            if (contains(app_->hits_[index].rectangle, x, y)) {
                child->vt = VT_I4;
                child->lVal = static_cast<long>(index + 1);
                return S_OK;
            }
        }
        RECT client{};
        if (!GetClientRect(app_->hwnd_, &client) || point.x < 0 || point.y < 0 ||
            point.x >= client.right || point.y >= client.bottom) return S_FALSE;
        child->vt = VT_I4;
        child->lVal = CHILDID_SELF;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE accDoDefaultAction(VARIANT child) override {
        long id = 0;
        if (!child_id(child, id) || id == CHILDID_SELF) return E_INVALIDARG;
        const auto hit = app_->hits_[id - 1];
        if (hit.action == Action::search) return S_FALSE;
        app_->click((hit.rectangle.left + hit.rectangle.right) / 2,
                    (hit.rectangle.top + hit.rectangle.bottom) / 2);
        NotifyWinEvent(EVENT_OBJECT_STATECHANGE, app_->hwnd_, OBJID_CLIENT, id);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE put_accName(VARIANT, BSTR) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE put_accValue(VARIANT, BSTR) override { return E_NOTIMPL; }

private:
    std::wstring self_name() const {
        if (app_->close_dialog_open_) return app_->tr(L"Sempervirens · 关闭", L"Sempervirens · Close");
        switch (app_->page_) {
        case Page::welcome: return app_->tr(L"Sempervirens · 首次设置", L"Sempervirens · First-time setup");
        case Page::settings: return app_->tr(L"Sempervirens · 设置", L"Sempervirens · Settings");
        case Page::confirmation: return app_->busy_ ?
            app_->tr(L"Sempervirens · 正在迁移", L"Sempervirens · Migrating") :
            app_->tr(L"Sempervirens · 确认迁移", L"Sempervirens · Review migration");
        case Page::result: return app_->tr(L"Sempervirens · 迁移完成", L"Sempervirens · Migration complete");
        case Page::error: return app_->tr(L"Sempervirens · 迁移已中断", L"Sempervirens · Migration stopped");
        default: return app_->tr(L"Sempervirens · 迁移", L"Sempervirens · Migration");
        }
    }

    std::wstring self_description() const {
        if (app_->close_dialog_open_)
            return app_->tr(L"可以取消、直接退出，或最小化到托盘。",
                            L"Cancel, exit now, or minimize to the tray.");
        switch (app_->page_) {
        case Page::welcome:
            return app_->tr(L"选择首次使用的迁移规则。默认仅迁移原版 Minecraft 内容。",
                            L"Choose the initial migration rules. Vanilla Minecraft content is selected by default.");
        case Page::settings:
            return app_->tr(L"调整界面语言、关闭方式和迁移规则，并查看项目仓库。",
                            L"Adjust the language, closing behavior, migration rules, and project repository.");
        case Page::confirmation:
            return app_->busy_ ? app_->status_ :
                app_->tr(L"核对迁出、迁入实例和同名内容处理方式，然后开始迁移。",
                         L"Review the source, destination, and existing-content policy, then start migration.");
        case Page::result:
            if (app_->result_)
                return std::to_wstring(app_->result_->success_count) + app_->tr(L" 项成功，", L" succeeded, ") +
                    std::to_wstring(app_->result_->skipped_count) + app_->tr(L" 项跳过，", L" skipped, ") +
                    std::to_wstring(app_->result_->failed_count) + app_->tr(L" 项失败。", L" failed.");
            return app_->tr(L"迁移结果尚不可用。", L"Migration results are not available.");
        case Page::error:
            return app_->migration_error_.empty() ?
                app_->tr(L"迁移未能完成，请检查迁入实例。",
                         L"Migration could not finish. Review the destination instance.") :
                app_->migration_error_;
        default:
            return app_->status_.empty() ?
                app_->tr(L"选择迁出和迁入实例，扫描并选择要迁移的内容。",
                         L"Choose source and destination instances, then scan and select content to migrate.") :
                app_->status_;
        }
    }

    bool child_id(const VARIANT& child, long& id) const {
        if (child.vt != VT_I4 || child.lVal < CHILDID_SELF ||
            child.lVal > static_cast<long>(app_->hits_.size())) return false;
        id = child.lVal;
        return true;
    }

    long hit_role(const Hit& hit) const {
        switch (hit.action) {
        case Action::search: return ROLE_SYSTEM_TEXT;
        case Action::list_item:
        case Action::drawer_item: return ROLE_SYSTEM_LISTITEM;
        case Action::list_toggle:
        case Action::group_toggle:
        case Action::close_prompt:
        case Action::close_remember:
        case Action::update_auto: return ROLE_SYSTEM_CHECKBUTTON;
        case Action::strategy_backup:
        case Action::strategy_replace:
        case Action::strategy_skip:
        case Action::language_zh:
        case Action::language_en:
        case Action::profile_xintinglei:
        case Action::profile_minecraft: return ROLE_SYSTEM_RADIOBUTTON;
        default: return ROLE_SYSTEM_PUSHBUTTON;
        }
    }

    long hit_state(long hit_index) const {
        const auto& hit = app_->hits_[hit_index];
        long state = STATE_SYSTEM_FOCUSABLE;
        if (app_->focus_index_ == hit_index ||
            (app_->search_focused_ && hit.action == Action::search)) state |= STATE_SYSTEM_FOCUSED;
        if (hit.action == Action::list_item && hit.index == app_->selected_operation_)
            state |= STATE_SYSTEM_SELECTED;
        if (hit.action == Action::list_toggle && app_->plan_ && hit.index >= 0 &&
            hit.index < static_cast<int>(app_->plan_->operations.size()) &&
            app_->plan_->operations[hit.index].selected) state |= STATE_SYSTEM_CHECKED;
        if (hit.action == Action::group_toggle && app_->plan_) {
            const auto members = app_->filtered_group_members(hit.index);
            if (std::any_of(members.begin(), members.end(), [&](int member) {
                    return app_->plan_->operations[member].selected;
                })) state |= STATE_SYSTEM_CHECKED;
        }
        if ((hit.action == Action::strategy_backup &&
             app_->strategy_ == OverwriteStrategy::backup_and_replace) ||
            (hit.action == Action::strategy_replace && app_->strategy_ == OverwriteStrategy::replace) ||
             (hit.action == Action::strategy_skip && app_->strategy_ == OverwriteStrategy::skip) ||
             (hit.action == Action::close_prompt && app_->prompt_on_close_) ||
             (hit.action == Action::close_remember && app_->remember_close_choice_) ||
             (hit.action == Action::update_auto && app_->auto_check_updates_) ||
             (hit.action == Action::language_zh && !app_->english_) ||
             (hit.action == Action::language_en && app_->english_) ||
             (hit.action == Action::profile_minecraft && !app_->profile_file_ && app_->use_minecraft_profile_) ||
             (hit.action == Action::profile_xintinglei && !app_->profile_file_ && !app_->use_minecraft_profile_))
            state |= STATE_SYSTEM_CHECKED;
        return state;
    }

    std::wstring hit_name(const Hit& hit) const {
        switch (hit.action) {
        case Action::settings: return app_->tr(L"设置", L"Settings");
        case Action::back: return app_->tr(L"返回", L"Back");
        case Action::source_browse: return app_->tr(L"选择迁出实例文件夹", L"Choose source instance folder");
        case Action::target_browse: return app_->tr(L"选择迁入实例文件夹", L"Choose destination instance folder");
        case Action::source_drawer:
            return app_->source_selection_ && !app_->source_selection_->candidates.empty() ?
                app_->tr(L"选择迁出实例", L"Choose source instance") :
                app_->tr(L"选择迁出实例文件夹", L"Choose source instance folder");
        case Action::target_drawer:
            return app_->target_selection_ && !app_->target_selection_->candidates.empty() ?
                app_->tr(L"选择迁入实例", L"Choose destination instance") :
                app_->tr(L"选择迁入实例文件夹", L"Choose destination instance folder");
        case Action::drawer_previous: return app_->tr(L"上一页实例", L"Previous instances");
        case Action::drawer_next: return app_->tr(L"下一页实例", L"Next instances");
        case Action::scan: return app_->busy_ ? app_->tr(L"取消扫描", L"Cancel scan") :
                                                app_->tr(L"扫描内容", L"Scan contents");
        case Action::start: return app_->tr(L"开始迁移", L"Start migration");
        case Action::strategy_backup: return app_->tr(L"备份后覆盖", L"Back up first");
        case Action::strategy_replace: return app_->tr(L"直接覆盖", L"Replace");
        case Action::strategy_skip: return app_->tr(L"保留目标", L"Keep target");
        case Action::settings_general: return app_->tr(L"常规设置", L"General settings");
        case Action::settings_migration: return app_->tr(L"迁移规则设置", L"Migration rule settings");
        case Action::settings_about: return app_->tr(L"关于与鸣谢", L"About and credits");
        case Action::language: return app_->tr(L"界面语言", L"Interface language");
        case Action::language_zh: return L"简体中文";
        case Action::language_en: return L"English";
        case Action::close_prompt: return app_->tr(L"关闭时询问", L"Ask when closing");
        case Action::close_action: return app_->tr(L"关闭方式", L"Close action");
        case Action::project_link: return app_->tr(L"查看项目仓库", L"Project repository");
        case Action::update_check: return app_->tr(L"检查更新", L"Check for updates");
        case Action::update_download: return app_->tr(L"下载更新", L"Download update");
        case Action::update_restart: return app_->tr(L"重启并更新", L"Restart to update");
        case Action::update_auto: return app_->tr(L"自动检查更新", L"Automatically check for updates");
        case Action::profile_browse: return app_->tr(L"选择迁移规则", L"Choose migration rules");
        case Action::profile_reset: return app_->tr(L"恢复默认规则", L"Restore default rules");
        case Action::profile_xintinglei: return app_->tr(L"使用新亭泪整合实例规则", L"Use Xintinglei modpack rules");
        case Action::profile_minecraft: return app_->tr(L"使用原版 Minecraft 规则", L"Use Minecraft rules");
        case Action::welcome_continue: return app_->tr(L"保存规则并继续", L"Save rules and continue");
        case Action::search: return app_->tr(L"搜索内容", L"Search contents");
        case Action::list_item:
            if (app_->plan_ && hit.index >= 0 && hit.index < static_cast<int>(app_->plan_->operations.size()))
                return display_operation_name(app_->plan_->operations[hit.index], app_->english_);
            break;
        case Action::list_toggle:
            if (app_->plan_ && hit.index >= 0 && hit.index < static_cast<int>(app_->plan_->operations.size()))
                return app_->tr(L"选择：", L"Select: ") +
                    display_operation_name(app_->plan_->operations[hit.index], app_->english_);
            break;
        case Action::group_toggle:
            if (app_->plan_ && hit.index >= 0 && hit.index < static_cast<int>(app_->plan_->operations.size()))
                return app_->tr(L"切换分组：", L"Toggle group: ") +
                    display_operation_group(app_->plan_->operations[hit.index], app_->english_);
            break;
        case Action::drawer_item: {
            const bool source = hit.index >= 0;
            const auto index = source ? hit.index : -hit.index - 1;
            const auto& selection = source ? app_->source_selection_ : app_->target_selection_;
            if (selection && index >= 0 && index < static_cast<int>(selection->candidates.size()))
                return selection->candidates[index].display_name;
            break;
        }
        case Action::confirm_start: return app_->tr(L"确认开始迁移", L"Start migration");
        case Action::report: return app_->tr(L"查看本次记录", L"Open record");
        case Action::backup: return app_->tr(L"查看备份", L"Open backups");
        case Action::technical_toggle: return app_->tr(L"切换路径详情", L"Toggle path details");
        case Action::close_remember: return app_->tr(L"记住本次选择", L"Remember this choice");
        case Action::close_tray: return app_->tr(L"最小化到托盘", L"Minimize to tray");
        case Action::close_exit: return app_->tr(L"直接退出", L"Exit");
        case Action::close_cancel: return app_->tr(L"取消关闭", L"Cancel closing");
        default: break;
        }
        return app_->tr(L"控件", L"Control");
    }

    App* app_ = nullptr;
    LONG references_ = 1;
};

LRESULT App::accessibility_result(WPARAM client_request) {
    if (!accessibility_) accessibility_ = new AppAccessible(this);
    return LresultFromObject(IID_IAccessible, client_request, accessibility_);
}

int App::smoke_accessibility() {
    prepare_visual_sample(false);
    page_ = Page::migration;
    paint();
    IAccessible* accessible = nullptr;
    if (FAILED(AccessibleObjectFromWindow(hwnd_, OBJID_CLIENT, IID_IAccessible,
                                         reinterpret_cast<void**>(&accessible))) || !accessible) return 2;
    int result = 0;
    long count = 0;
    if (FAILED(accessible->get_accChildCount(&count)) || count != static_cast<long>(hits_.size()) || count < 6)
        result = 3;
    long settings_id = 0;
    long search_id = 0;
    long toggle_id = 0;
    for (long id = 1; !result && id <= count; ++id) {
        VARIANT child{};
        child.vt = VT_I4;
        child.lVal = id;
        BSTR name = nullptr;
        VARIANT role{};
        VARIANT state{};
        if (FAILED(accessible->get_accName(child, &name)) || !name ||
            FAILED(accessible->get_accRole(child, &role)) || role.vt != VT_I4 ||
            FAILED(accessible->get_accState(child, &state)) || state.vt != VT_I4) {
            if (name) SysFreeString(name);
            result = 4;
            break;
        }
        const std::wstring label(name, SysStringLen(name));
        SysFreeString(name);
        if (label == L"设置" && role.lVal == ROLE_SYSTEM_PUSHBUTTON) settings_id = id;
        if (label == L"搜索内容" && role.lVal == ROLE_SYSTEM_TEXT) search_id = id;
        if (label == L"选择：键位设置" && role.lVal == ROLE_SYSTEM_CHECKBUTTON &&
            (state.lVal & STATE_SYSTEM_CHECKED)) toggle_id = id;
    }
    if (!result && (!settings_id || !search_id || !toggle_id)) result = 5;
    if (!result) {
        VARIANT child{};
        child.vt = VT_I4;
        child.lVal = settings_id;
        long left = 0, top = 0, width = 0, height = 0;
        if (FAILED(accessible->accLocation(&left, &top, &width, &height, child)) ||
            width <= 0 || height <= 0) result = 6;
        VARIANT hit{};
        if (!result && (FAILED(accessible->accHitTest(left + width / 2, top + height / 2, &hit)) ||
                        hit.vt != VT_I4 || hit.lVal != settings_id)) result = 7;
    }
    if (!result) {
        VARIANT search{};
        search.vt = VT_I4;
        search.lVal = search_id;
        if (FAILED(accessible->accSelect(SELFLAG_TAKEFOCUS, search)) || !search_focused_) result = 8;
        VARIANT focus{};
        if (!result && (FAILED(accessible->get_accFocus(&focus)) || focus.vt != VT_I4 ||
                        focus.lVal != search_id)) result = 9;
    }
    if (!result) {
        VARIANT toggle{};
        toggle.vt = VT_I4;
        toggle.lVal = toggle_id;
        if (FAILED(accessible->accDoDefaultAction(toggle)) || !plan_ ||
            plan_->operations[0].selected) result = 10;
    }
    accessible->Release();
    return result;
}

int App::smoke_accessibility_pages() {
    english_ = false;
    prompt_on_close_ = true;
    busy_ = false;
    language_drawer_open_ = false;
    close_dialog_open_ = false;
    focus_index_ = -1;
    page_ = Page::settings;
    paint();

    IAccessible* accessible = nullptr;
    if (FAILED(AccessibleObjectFromWindow(hwnd_, OBJID_CLIENT, IID_IAccessible,
                                         reinterpret_cast<void**>(&accessible))) || !accessible) return 2;
    int result = 0;
    const auto child_variant = [](long id) {
        VARIANT child{};
        child.vt = VT_I4;
        child.lVal = id;
        return child;
    };
    const auto text_property = [&](long id, bool description) -> std::optional<std::wstring> {
        auto child = child_variant(id);
        BSTR value = nullptr;
        const auto hr = description ? accessible->get_accDescription(child, &value) :
                                      accessible->get_accName(child, &value);
        if (FAILED(hr) || !value) return std::nullopt;
        std::wstring text(value, SysStringLen(value));
        SysFreeString(value);
        return text;
    };
    const auto role_of = [&](long id) -> long {
        auto child = child_variant(id);
        VARIANT role{};
        return SUCCEEDED(accessible->get_accRole(child, &role)) && role.vt == VT_I4 ? role.lVal : 0;
    };
    const auto state_of = [&](long id) -> long {
        auto child = child_variant(id);
        VARIANT state{};
        return SUCCEEDED(accessible->get_accState(child, &state)) && state.vt == VT_I4 ? state.lVal : 0;
    };
    const auto find_child = [&](std::wstring_view expected) -> long {
        long count = 0;
        if (FAILED(accessible->get_accChildCount(&count))) return 0;
        for (long id = 1; id <= count; ++id) {
            const auto name = text_property(id, false);
            if (name && *name == expected) return id;
        }
        return 0;
    };

    const auto settings_name = text_property(CHILDID_SELF, false);
    const auto settings_description = text_property(CHILDID_SELF, true);
    const auto language_id = find_child(L"界面语言");
    const auto prompt_id = find_child(L"关闭时询问");
    if (!settings_name || *settings_name != L"Sempervirens · 设置" ||
        !settings_description || settings_description->find(L"迁移规则") == std::wstring::npos ||
        role_of(CHILDID_SELF) != ROLE_SYSTEM_WINDOW || !language_id || !prompt_id ||
        find_child(L"关于与鸣谢") == 0 || role_of(language_id) != ROLE_SYSTEM_PUSHBUTTON ||
        role_of(prompt_id) != ROLE_SYSTEM_CHECKBUTTON ||
        !(state_of(prompt_id) & STATE_SYSTEM_CHECKED)) result = 3;
    if (!result) {
        const auto language_description = text_property(language_id, true);
        auto language = child_variant(language_id);
        if (!language_description || *language_description != L"当前为简体中文" ||
            FAILED(accessible->accDoDefaultAction(language)) || !language_drawer_open_) result = 4;
    }
    if (!result) {
        paint();
        long count = 0;
        const auto chinese_id = find_child(L"简体中文");
        const auto english_id = find_child(L"English");
        if (FAILED(accessible->get_accChildCount(&count)) || count != 3 || !chinese_id || !english_id ||
            find_child(L"返回") || role_of(chinese_id) != ROLE_SYSTEM_RADIOBUTTON ||
            role_of(english_id) != ROLE_SYSTEM_RADIOBUTTON ||
            !(state_of(chinese_id) & STATE_SYSTEM_CHECKED) ||
            (state_of(english_id) & STATE_SYSTEM_CHECKED)) result = 5;
        if (!result) {
            auto english = child_variant(english_id);
            BSTR action = nullptr;
            if (FAILED(accessible->get_accDefaultAction(english, &action)) || !action ||
                std::wstring_view(action, SysStringLen(action)) != L"选择" ||
                FAILED(accessible->accSelect(SELFLAG_TAKEFOCUS, english))) result = 6;
            if (action) SysFreeString(action);
        }
    }

    if (!result) {
        language_drawer_open_ = false;
        focus_index_ = -1;
        settings_section_ = SettingsSection::about;
        paint();
        if (!find_child(L"查看项目仓库")) result = 11;
    }

    if (!result) {
        settings_section_ = SettingsSection::general;
        page_ = Page::migration;
        close_requested();
        paint();
        const auto dialog_name = text_property(CHILDID_SELF, false);
        const auto dialog_description = text_property(CHILDID_SELF, true);
        long count = 0;
        const auto cancel_id = find_child(L"取消关闭");
        const auto tray_id = find_child(L"最小化到托盘");
        const auto remember_id = find_child(L"记住本次选择");
        VARIANT dialog_focus{};
        if (FAILED(accessible->get_accChildCount(&count)) || count != 4 ||
            !dialog_name || *dialog_name != L"Sempervirens · 关闭" ||
            !dialog_description || dialog_description->find(L"最小化到托盘") == std::wstring::npos ||
            role_of(CHILDID_SELF) != ROLE_SYSTEM_DIALOG || find_child(L"设置") ||
            !cancel_id || !find_child(L"直接退出") || !tray_id || !remember_id ||
            role_of(remember_id) != ROLE_SYSTEM_CHECKBUTTON ||
            (state_of(remember_id) & STATE_SYSTEM_CHECKED) ||
            FAILED(accessible->get_accFocus(&dialog_focus)) || dialog_focus.vt != VT_I4 ||
            dialog_focus.lVal != tray_id) result = 7;
        if (!result) {
            auto cancel = child_variant(cancel_id);
            if (FAILED(accessible->accDoDefaultAction(cancel)) || close_dialog_open_ || !IsWindow(hwnd_)) result = 8;
        }
    }

    if (!result) {
        result_ = MigrationResult{};
        result_->success_count = 2;
        result_->skipped_count = 1;
        result_->failed_count = 0;
        page_ = Page::result;
        focus_index_ = -1;
        paint();
        const auto result_name = text_property(CHILDID_SELF, false);
        const auto result_description = text_property(CHILDID_SELF, true);
        const auto back_id = find_child(L"返回");
        if (!result_name || *result_name != L"Sempervirens · 迁移完成" ||
            !result_description || result_description->find(L"2 项成功") == std::wstring::npos ||
            result_description->find(L"1 项跳过") == std::wstring::npos ||
            !back_id || role_of(back_id) != ROLE_SYSTEM_PUSHBUTTON) result = 9;
        if (!result) {
            auto back = child_variant(back_id);
            if (FAILED(accessible->accDoDefaultAction(back)) || page_ != Page::migration) result = 10;
        }
    }
    if (!result) {
        page_ = Page::confirmation;
        busy_ = false;
        paint();
        const auto confirmation_name = text_property(CHILDID_SELF, false);
        const auto confirmation_description = text_property(CHILDID_SELF, true);
        migration_error_ = L"测试用错误说明";
        page_ = Page::error;
        paint();
        const auto error_name = text_property(CHILDID_SELF, false);
        const auto error_description = text_property(CHILDID_SELF, true);
        if (!confirmation_name || *confirmation_name != L"Sempervirens · 确认迁移" ||
            !confirmation_description || confirmation_description->find(L"核对迁出") == std::wstring::npos ||
            !error_name || *error_name != L"Sempervirens · 迁移已中断" ||
            !error_description || *error_description != migration_error_) result = 11;
    }
    accessible->Release();
    return result;
}

int App::smoke_instance_card_behavior() {
    english_ = false;
    busy_ = false;
    page_ = Page::migration;
    plan_.reset();
    source_selection_.reset();
    target_selection_.reset();
    source_index_ = target_index_ = -1;
    source_drawer_open_ = target_drawer_open_ = false;
    paint();
    const auto action_count = [&](Action action) {
        return std::count_if(hits_.begin(), hits_.end(), [&](const Hit& hit) {
            return hit.action == action;
        });
    };
    if (action_count(Action::source_drawer) != 1 || action_count(Action::target_drawer) != 1)
        return 2;

    IAccessible* accessible = nullptr;
    if (FAILED(AccessibleObjectFromWindow(hwnd_, OBJID_CLIENT, IID_IAccessible,
                                         reinterpret_cast<void**>(&accessible))) || !accessible) return 3;
    const auto has_accessible_name = [&](std::wstring_view expected) {
        long count = 0;
        if (FAILED(accessible->get_accChildCount(&count))) return false;
        for (long id = 1; id <= count; ++id) {
            VARIANT child{};
            child.vt = VT_I4;
            child.lVal = id;
            BSTR value = nullptr;
            if (SUCCEEDED(accessible->get_accName(child, &value)) && value) {
                const bool matches = std::wstring_view(value, SysStringLen(value)) == expected;
                SysFreeString(value);
                if (matches) return true;
            }
        }
        return false;
    };
    const auto accessible_description = [&](std::wstring_view expected_name)
            -> std::optional<std::wstring> {
        long count = 0;
        if (FAILED(accessible->get_accChildCount(&count))) return std::nullopt;
        for (long id = 1; id <= count; ++id) {
            VARIANT child{};
            child.vt = VT_I4;
            child.lVal = id;
            BSTR name = nullptr;
            if (FAILED(accessible->get_accName(child, &name)) || !name) continue;
            const bool matches = std::wstring_view(name, SysStringLen(name)) == expected_name;
            SysFreeString(name);
            if (!matches) continue;
            BSTR description = nullptr;
            if (FAILED(accessible->get_accDescription(child, &description)) || !description)
                return std::nullopt;
            std::wstring result(description, SysStringLen(description));
            SysFreeString(description);
            return result;
        }
        return std::nullopt;
    };
    if (!has_accessible_name(L"选择迁出实例文件夹") ||
        !has_accessible_name(L"选择迁入实例文件夹")) {
        accessible->Release();
        return 4;
    }

    InstanceInfo only;
    only.root = application_directory();
    only.display_name = L"单个实例";
    source_selection_ = InstanceSelection{only.root, {only}};
    source_index_ = 0;
    paint();
    const auto source_card = std::find_if(hits_.begin(), hits_.end(), [](const Hit& hit) {
        return hit.action == Action::source_drawer;
    });
    const auto selected_path_description = accessible_description(L"选择迁出实例");
    if (source_card == hits_.end() || !has_accessible_name(L"选择迁出实例") ||
        !selected_path_description || *selected_path_description != only.root.wstring()) {
        accessible->Release();
        return 5;
    }
    const auto card_bounds = source_card->rectangle;
    click((card_bounds.left + card_bounds.right) / 2,
          (card_bounds.top + card_bounds.bottom) / 2);
    if (!reduced_motion_ && drawer_animation_started_ == 0) {
        accessible->Release();
        return 12;
    }
    paint();
    const auto item = std::find_if(hits_.begin(), hits_.end(), [](const Hit& hit) {
        return hit.action == Action::drawer_item && hit.index == 0;
    });
    if (!source_drawer_open_ || item == hits_.end() ||
        accessible_description(L"单个实例") != std::optional<std::wstring>(only.root.wstring())) {
        accessible->Release();
        return 6;
    }
    const auto reveal_before_fast_close = drawer_reveal(true);
    mouse_double_click((card_bounds.left + card_bounds.right) / 2,
                       (card_bounds.top + card_bounds.bottom) / 2);
    mouse_up((card_bounds.left + card_bounds.right) / 2,
             (card_bounds.top + card_bounds.bottom) / 2);
    if (source_drawer_open_ || (!reduced_motion_ && reveal_before_fast_close > 0.01f &&
        (drawer_animation_started_ == 0 || !drawer_animation_visible_ || drawer_animation_to_ != 0.0f))) {
        accessible->Release();
        return 9;
    }
    click((card_bounds.left + card_bounds.right) / 2,
          (card_bounds.top + card_bounds.bottom) / 2);
    paint();
    mouse_down(5, 5);
    if (source_drawer_open_) {
        accessible->Release();
        return 10;
    }
    click((card_bounds.left + card_bounds.right) / 2,
          (card_bounds.top + card_bounds.bottom) / 2);
    paint();
    const auto reopened_item = std::find_if(hits_.begin(), hits_.end(), [](const Hit& hit) {
        return hit.action == Action::drawer_item && hit.index == 0;
    });
    if (reopened_item == hits_.end()) {
        accessible->Release();
        return 11;
    }
    const auto item_bounds = reopened_item->rectangle;
    click((item_bounds.left + item_bounds.right) / 2,
          (item_bounds.top + item_bounds.bottom) / 2);
    if (source_drawer_open_ || source_index_ != 0 || busy_) {
        accessible->Release();
        return 7;
    }

    source_selection_ = InstanceSelection{application_directory(), {}};
    source_index_ = -1;
    paint();
    const bool empty_card_is_actionable = action_count(Action::source_drawer) == 1 &&
        has_accessible_name(L"选择迁出实例文件夹");
    accessible->Release();
    return empty_card_is_actionable ? 0 : 8;
}

int App::smoke_pointer_feedback() {
    page_ = Page::migration;
    busy_ = false;
    close_dialog_open_ = false;
    paint();
    const auto settings = std::find_if(hits_.begin(), hits_.end(), [](const Hit& hit) {
        return hit.action == Action::settings;
    });
    if (settings == hits_.end()) return 2;
    const auto bounds = settings->rectangle;
    const auto x = (bounds.left + bounds.right) / 2;
    const auto y = (bounds.top + bounds.bottom) / 2;
    mouse_move(x, y);
    if (hovered_action_ != Action::settings || hovered_index_ != -1 || !tracking_mouse_) return 3;
    mouse_down(x, y);
    if (pressed_action_ != Action::settings || GetCapture() != hwnd_) return 4;
    mouse_up(1, 1);
    if (page_ != Page::migration || pressed_action_ != Action::none || GetCapture() == hwnd_) return 5;
    mouse_down(x, y);
    mouse_up(x, y);
    if (page_ != Page::settings || pressed_action_ != Action::none || GetCapture() == hwnd_) return 6;
    mouse_leave();
    return hovered_action_ == Action::none && hovered_index_ == -1 && !tracking_mouse_ ? 0 : 7;
}

int App::smoke_drawer_scrollbar_and_deactivation() {
    page_ = Page::migration;
    busy_ = false;
    close_dialog_open_ = false;
    source_index_ = -1;
    target_drawer_open_ = false;
    std::vector<InstanceInfo> candidates;
    for (int index = 0; index < 12; ++index) {
        InstanceInfo candidate;
        candidate.root = application_directory() / (L"drawer-instance-" + std::to_wstring(index));
        candidate.display_name = L"实例 " + std::to_wstring(index + 1);
        candidates.push_back(std::move(candidate));
    }
    source_selection_ = InstanceSelection{application_directory(), std::move(candidates)};
    source_drawer_open_ = true;
    source_drawer_first_ = 0;
    paint();
    if (!drawer_scroll_track_) return 2;
    const auto track = *drawer_scroll_track_;
    const auto x = (track.left + track.right) / 2.0f;
    mouse_down(x, track.bottom - 1.0f);
    if (!drawer_scroll_dragging_ || GetCapture() != hwnd_ ||
        std::abs(source_drawer_first_ - (12 - drawer_visible_rows_)) > 0.001f) return 3;
    mouse_move(x, track.top + 1.0f);
    if (source_drawer_first_ != 0) return 4;
    mouse_up(x, track.top + 1.0f);
    if (drawer_scroll_dragging_ || GetCapture() == hwnd_) return 5;

    source_drawer_open_ = true;
    paint();
    SendMessageW(hwnd_, WM_ACTIVATE, WA_INACTIVE, 0);
    if (source_drawer_open_ || target_drawer_open_) return 6;

    page_ = Page::settings;
    language_drawer_open_ = true;
    paint();
    SendMessageW(hwnd_, WM_ACTIVATE, WA_INACTIVE, 0);
    return !language_drawer_open_ && pressed_action_ == Action::none &&
        hovered_action_ == Action::none && GetCapture() != hwnd_ ? 0 : 7;
}

int App::smoke_session_end(bool destroy) {
    page_ = Page::migration;
    close_dialog_open_ = false;
    busy_ = true;
    job_ = Job::migrate;
    if (SendMessageW(hwnd_, WM_QUERYENDSESSION, 0, ENDSESSION_CLOSEAPP) != FALSE ||
        !IsWindow(hwnd_) || close_dialog_open_ ||
        status_.find(english_ ? L"shutdown" : L"关机") == std::wstring::npos) {
        busy_ = false;
        job_ = Job::none;
        return 2;
    }
    busy_ = false;
    job_ = Job::none;
    if (SendMessageW(hwnd_, WM_QUERYENDSESSION, 0, ENDSESSION_CLOSEAPP) == FALSE) return 3;
    SendMessageW(hwnd_, WM_ENDSESSION, destroy ? TRUE : FALSE, ENDSESSION_CLOSEAPP);
    return IsWindow(hwnd_) == !destroy ? 0 : 4;
}

int App::smoke_dpi_change() {
    RECT original{};
    if (!GetWindowRect(hwnd_, &original)) return 2;
    const auto original_dpi = dpi_;
    const auto changed_dpi = original_dpi == 144 ? 120U : 144U;
    RECT proposed = original;
    proposed.right = proposed.left + MulDiv(original.right - original.left, changed_dpi, original_dpi);
    proposed.bottom = proposed.top + MulDiv(original.bottom - original.top, changed_dpi, original_dpi);
    SendMessageW(hwnd_, WM_DPICHANGED, MAKELONG(changed_dpi, changed_dpi),
                 reinterpret_cast<LPARAM>(&proposed));
    paint();
    FLOAT target_x = 0;
    FLOAT target_y = 0;
    if (!target_) return 3;
    target_->GetDpi(&target_x, &target_y);
    if (dpi_ != changed_dpi || std::abs(target_x - changed_dpi) > 0.1f ||
        std::abs(target_y - changed_dpi) > 0.1f) return 4;
    const auto settings = std::find_if(hits_.begin(), hits_.end(), [](const Hit& hit) {
        return hit.action == Action::settings;
    });
    if (settings == hits_.end()) return 5;
    const auto logical_x = (settings->rectangle.left + settings->rectangle.right) / 2.0f;
    const auto logical_y = (settings->rectangle.top + settings->rectangle.bottom) / 2.0f;
    const auto pixel_x = static_cast<int>(std::lround(logical_x * scale()));
    const auto pixel_y = static_cast<int>(std::lround(logical_y * scale()));
    SendMessageW(hwnd_, WM_MOUSEMOVE, 0, MAKELPARAM(pixel_x, pixel_y));
    if (hovered_action_ != Action::settings) return 6;

    SendMessageW(hwnd_, WM_DPICHANGED, MAKELONG(original_dpi, original_dpi),
                 reinterpret_cast<LPARAM>(&original));
    paint();
    target_->GetDpi(&target_x, &target_y);
    mouse_leave();
    return dpi_ == original_dpi && std::abs(target_x - original_dpi) <= 0.1f &&
        std::abs(target_y - original_dpi) <= 0.1f ? 0 : 7;
}

int App::smoke_content_scrollbars() {
    page_ = Page::migration;
    busy_ = false;
    close_dialog_open_ = false;
    source_drawer_open_ = false;
    target_drawer_open_ = false;
    prepare_visual_sample(false);
    technical_expanded_ = true;
    list_first_ = 0;
    detail_scroll_ = 0;
    paint();
    if (!list_scroll_track_ || list_scroll_total_ <= list_scroll_visible_) return 2;
    const auto list_track = *list_scroll_track_;
    const auto list_x = (list_track.left + list_track.right) / 2.0f;
    mouse_down(list_x, list_track.bottom - 1.0f);
    if (content_scroll_drag_ != ContentScrollDrag::list || GetCapture() != hwnd_ ||
        list_first_ != list_scroll_total_ - list_scroll_visible_) return 3;
    mouse_move(list_x, list_track.top + 1.0f);
    if (list_first_ != 0) return 4;
    mouse_up(list_x, list_track.top + 1.0f);
    if (content_scroll_drag_ != ContentScrollDrag::none || GetCapture() == hwnd_) return 5;

    paint();
    if (!detail_scroll_track_ || detail_total_height_ <= detail_scroll_visible_height_) return 6;
    const auto detail_track = *detail_scroll_track_;
    const auto detail_x = (detail_track.left + detail_track.right) / 2.0f;
    const auto maximum = detail_total_height_ - detail_scroll_visible_height_;
    mouse_down(detail_x, detail_track.bottom - 1.0f);
    if (content_scroll_drag_ != ContentScrollDrag::detail || GetCapture() != hwnd_ ||
        detail_scroll_ < maximum - 1.0f) return 7;
    mouse_move(detail_x, detail_track.top + 1.0f);
    if (detail_scroll_ > 1.0f) return 8;
    mouse_up(detail_x, detail_track.top + 1.0f);
    if (content_scroll_drag_ != ContentScrollDrag::none || GetCapture() == hwnd_) return 9;

    // High-resolution wheels must preserve fractional movement, and reversing
    // quickly must use the current position rather than snap to a whole row.
    reduced_motion_ = false;
    list_first_ = 0;
    scroll_at(-15, list_x);
    if (std::abs(list_first_ - 6.0f/62) > 0.0001f || !scroll_animation_started_) return 14;
    const auto tiny_scroll = list_first_;
    scroll_at(0, list_x);
    if (list_first_ != tiny_scroll) return 15;
    scroll_at(15, list_x);
    if (list_first_ != 0) return 16;
    finish_scroll_animation();
    reduced_motion_ = true;
    scroll_at(-15, list_x);
    if (std::abs(list_first_ - 6.0f/62) > 0.0001f || scroll_animation_started_) return 17;

    // Real long wrapped paths, both languages, and the smallest client size.
    // Reuse only the in-memory visual fixture; no player files are accessed.
    auto long_source = fs::path(L"C:\\Minecraft");
    for (int part=0; part<18; ++part) long_source /= L"long-instance-directory";
    auto& operation = plan_->operations[selected_operation_];
    operation.source_path = long_source / L"source-last-component.txt";
    operation.target_path = long_source / L"destination-last-component.txt";
    for (const bool english : {false, true}) {
        english_ = english;
        technical_expanded_ = true;
        detail_scroll_ = 0;
        paint();
        const auto long_maximum = detail_total_height_ - detail_scroll_visible_height_;
        if (long_maximum <= 0) return 18;
        scroll_at(-15, detail_x);
        if (detail_scroll_ != 6) return 19;
        for (int step=0; step<100; ++step) scroll_at(-120, detail_x);
        paint();
        if (std::abs(detail_scroll_-long_maximum) > 0.1f ||
            detail_viewport_bottom_-detail_content_bottom_ < 11) return 20;
        // Capture uses a new viewport, so deliberately request its lower bound.
        detail_scroll_ = 100000;
        if (!capture(application_directory().parent_path() /
            (english ? L"ui-technical-bottom-en.png" : L"ui-technical-bottom-zh.png"),
            false, false, false, 945, 681, english)) return 21;
        if (detail_viewport_bottom_-detail_content_bottom_ < 11) return 22;
        technical_expanded_ = false;
        paint();
        if (detail_scroll_ > std::max(0.0f, detail_total_height_-detail_scroll_visible_height_)) return 23;
    }

    page_ = Page::error;
    migration_error_.clear();
    for (int line = 0; line < 80; ++line)
        migration_error_ += L"无法读取迁入实例中的测试文件，请检查访问权限。\n";
    error_scroll_ = 0;
    paint();
    if (!error_scroll_track_ || error_total_height_ <= error_visible_height_) return 10;
    const auto error_track = *error_scroll_track_;
    const auto error_x = (error_track.left + error_track.right) / 2.0f;
    const auto error_maximum = error_total_height_ - error_visible_height_;
    mouse_down(error_x, error_track.bottom - 1.0f);
    if (content_scroll_drag_ != ContentScrollDrag::error || GetCapture() != hwnd_ ||
        error_scroll_ < error_maximum - 1.0f) return 11;
    mouse_move(error_x, error_track.top + 1.0f);
    if (error_scroll_ > 1.0f) return 12;
    mouse_up(error_x, error_track.top + 1.0f);
    return content_scroll_drag_ == ContentScrollDrag::none && GetCapture() != hwnd_ ? 0 : 13;
}

LRESULT CALLBACK window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* app = reinterpret_cast<App*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
        app = static_cast<App*>(create->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
    }
    if (!app) return DefWindowProcW(hwnd, message, wparam, lparam);
    if (registered_taskbar_created_message && message == registered_taskbar_created_message) {
        app->taskbar_created();
        return 0;
    }
    switch (message) {
    case WM_CREATE:
        if (!app->initialize(hwnd)) return -1;
        return 0;
    case WM_PAINT: app->paint(); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_SIZE: app->resize(LOWORD(lparam), HIWORD(lparam)); return 0;
    case WM_TIMER: if (wparam == animation_timer) app->timer(); return 0; break;
    case WM_ENTERSIZEMOVE: app->set_resizing(true); return 0;
    case WM_EXITSIZEMOVE: app->set_resizing(false); return 0;
    case WM_MOUSEMOVE:
        app->mouse_move(GET_X_LPARAM(lparam) / app->scale(), GET_Y_LPARAM(lparam) / app->scale());
        return 0;
    case WM_MOUSELEAVE: app->mouse_leave(); return 0;
    case WM_CAPTURECHANGED: app->capture_changed(); return 0;
    case WM_LBUTTONDOWN:
        app->mouse_down(GET_X_LPARAM(lparam) / app->scale(), GET_Y_LPARAM(lparam) / app->scale());
        return 0;
    case WM_LBUTTONUP:
        app->mouse_up(GET_X_LPARAM(lparam) / app->scale(), GET_Y_LPARAM(lparam) / app->scale());
        return 0;
    case WM_LBUTTONDBLCLK:
        app->mouse_double_click(GET_X_LPARAM(lparam) / app->scale(),
                                GET_Y_LPARAM(lparam) / app->scale());
        return 0;
    case WM_MOUSEWHEEL: app->scroll(GET_WHEEL_DELTA_WPARAM(wparam)); return 0;
    case WM_DROPFILES: app->drop_files(reinterpret_cast<HDROP>(wparam)); return 0;
    case WM_KEYDOWN: app->key_down(wparam); return 0;
    case WM_CHAR: app->character(static_cast<wchar_t>(wparam)); return 0;
    case WM_IME_STARTCOMPOSITION:
        app->position_ime();
        return DefWindowProcW(hwnd, message, wparam, lparam);
    case WM_GETOBJECT:
        if (static_cast<LONG>(lparam) == OBJID_CLIENT) return app->accessibility_result(wparam);
        return DefWindowProcW(hwnd, message, wparam, lparam);
    case wm_worker_progress: app->worker_progress(); return 0;
    case wm_worker: app->worker_complete(); return 0;
    case wm_update: app->update_complete(wparam); return 0;
    case WM_GETMINMAXINFO: {
        auto* info = reinterpret_cast<MINMAXINFO*>(lparam);
        const auto dpi = static_cast<UINT>(app->scale() * 96);
        info->ptMinTrackSize.x = MulDiv(960, dpi, 96);
        info->ptMinTrackSize.y = MulDiv(720, dpi, 96);
        return 0;
    }
    case WM_CLOSE: app->close_requested(); return 0;
    case WM_QUERYENDSESSION: return app->query_end_session() ? TRUE : FALSE;
    case WM_ENDSESSION: app->end_session(wparam != FALSE); return 0;
    case wm_tray: app->tray_message(lparam); return 0;
    case WM_COMMAND: app->tray_command(LOWORD(wparam)); return 0;
    case WM_ACTIVATE:
        if (LOWORD(wparam) == WA_INACTIVE) app->deactivate();
        return DefWindowProcW(hwnd, message, wparam, lparam);
    case WM_DPICHANGED: {
        app->set_dpi(HIWORD(wparam));
        const auto* proposed = reinterpret_cast<const RECT*>(lparam);
        SetWindowPos(hwnd, nullptr, proposed->left, proposed->top,
                     proposed->right - proposed->left, proposed->bottom - proposed->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_DESTROY: PostQuitMessage(0); return 0;
    default: return DefWindowProcW(hwnd, message, wparam, lparam);
    }
}

bool place_initial_window(HWND hwnd) {
    const auto dpi = win_compat::window_dpi(hwnd);
    RECT frame{0, 0, MulDiv(1120, dpi, 96), MulDiv(800, dpi, 96)};
    if (!win_compat::adjust_window_rect(&frame, GetWindowLongW(hwnd, GWL_STYLE), FALSE,
                                        GetWindowLongW(hwnd, GWL_EXSTYLE), dpi)) return false;
    MONITORINFO monitor{sizeof(monitor)};
    if (!GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &monitor)) return false;
    const auto work_width = monitor.rcWork.right - monitor.rcWork.left;
    const auto work_height = monitor.rcWork.bottom - monitor.rcWork.top;
    const auto width = std::max<LONG>(MulDiv(960, dpi, 96),
                                      std::min<LONG>(frame.right - frame.left, work_width));
    const auto height = std::max<LONG>(MulDiv(720, dpi, 96),
                                       std::min<LONG>(frame.bottom - frame.top, work_height));
    const auto x = monitor.rcWork.left + std::max<LONG>(0, (work_width - width) / 2);
    const auto y = monitor.rcWork.top + std::max<LONG>(0, (work_height - height) / 2);
    return SetWindowPos(hwnd, nullptr, x, y, width, height, SWP_NOZORDER | SWP_NOACTIVATE);
}

bool smoke_test_folder_drop(HWND hwnd, App& app, bool source, bool multiple = false) {
    const auto folder = application_directory().wstring();
    const auto ignored = fs::temp_directory_path().wstring();
    const auto characters = folder.size() + 1 + (multiple ? ignored.size() + 1 : 0) + 1;
    const auto bytes = sizeof(DROPFILES) + characters * sizeof(wchar_t);
    const auto memory = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, bytes);
    if (!memory) return false;
    auto* drop = static_cast<DROPFILES*>(GlobalLock(memory));
    if (!drop) { GlobalFree(memory); return false; }
    drop->pFiles = sizeof(DROPFILES);
    drop->pt = {MulDiv(source ? 80 : 700, win_compat::window_dpi(hwnd), 96),
                MulDiv(200, win_compat::window_dpi(hwnd), 96)};
    drop->fWide = TRUE;
    auto* path = reinterpret_cast<wchar_t*>(reinterpret_cast<char*>(drop) + drop->pFiles);
    std::copy(folder.begin(), folder.end(), path);
    path[folder.size()] = L'\0';
    auto offset = folder.size() + 1;
    if (multiple) {
        std::copy(ignored.begin(), ignored.end(), path + offset);
        offset += ignored.size();
        path[offset++] = L'\0';
    }
    path[offset] = L'\0';
    GlobalUnlock(memory);
    SendMessageW(hwnd, WM_DROPFILES, reinterpret_cast<WPARAM>(memory), 0);
    return app.discovery_started_for_smoke(source, application_directory());
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR arguments, int show) {
    (void)arguments;
    int argument_count = 0;
    wchar_t** argument_values = CommandLineToArgvW(GetCommandLineW(), &argument_count);
    std::wstring parsed_argument;
    std::wstring update_health_token;
    for (int index = 1; argument_values && index < argument_count; ++index) {
        const std::wstring_view value(argument_values[index]);
        if (value.starts_with(L"--update-health="))
            update_health_token = value.substr(std::wstring_view(L"--update-health=").size());
        else if (parsed_argument.empty()) parsed_argument = value;
    }
    if (argument_values) LocalFree(argument_values);
    const auto args = std::wstring_view(parsed_argument);
    const bool smoke_test = args == L"--smoke";
    const bool smoke_single_instance_test = args == L"--smoke-single-instance";
    const bool smoke_single_instance_child = args == L"--smoke-single-instance-child";
    const bool smoke_tray_lifecycle_test = args == L"--smoke-tray-lifecycle";
    const bool smoke_search_input_test = args == L"--smoke-search-input";
    const bool smoke_group_toggle_test = args == L"--smoke-group-toggle";
    const bool smoke_installer_language_test = args == L"--smoke-installer-language";
    const bool isolated_single_instance = smoke_single_instance_test || smoke_single_instance_child;
    const bool automated_test = args.starts_with(L"--smoke") || args.starts_with(L"--capture");
    const auto selected_window_class = isolated_single_instance ?
        single_instance_smoke_window_class : automated_test ? automated_test_window_class : window_class;
    const bool smoke_drop_test = args == L"--smoke-drop" || args == L"--smoke-drop-target" ||
                                 args == L"--smoke-drop-multiple";
    const bool smoke_splash_test = args == L"--smoke-splash" || args == L"--smoke-splash-skip";
    const bool smoke_resize_test = args == L"--smoke-resize";
    const bool smoke_material_test = args == L"--smoke-material" ||
                                     args == L"--smoke-material-required";
    const bool smoke_progress_test = args == L"--smoke-progress";
    const bool smoke_cancel_scan_test = args == L"--smoke-cancel-scan";
    const bool smoke_close_during_scan_test = args == L"--smoke-close-during-scan";
    const bool smoke_instance_choice_test = args == L"--smoke-instance-choice";
    const bool smoke_instance_card_test = args == L"--smoke-instance-card";
    const bool smoke_pointer_feedback_test = args == L"--smoke-pointer-feedback";
    const bool smoke_drawer_scrollbar_test = args == L"--smoke-drawer-scrollbar";
    const bool smoke_session_end_test = args == L"--smoke-session-end" ||
                                        args == L"--smoke-session-end-destroy";
    const bool smoke_dpi_change_test = args == L"--smoke-dpi-change";
    const bool smoke_content_scrollbars_test = args == L"--smoke-content-scrollbars";
    const bool smoke_migration_close_test = args == L"--smoke-migration-close";
    const bool smoke_migration_error_test = args == L"--smoke-migration-error";
    const bool smoke_real_migration_error_test = args == L"--smoke-real-migration-error";
    const bool smoke_real_migration_success_test = args == L"--smoke-real-migration-success" ||
                                                   args == L"--smoke-real-migration-success-en" ||
                                                   args == L"--smoke-real-migration-keyboard";
    const bool smoke_result_state_test = args == L"--smoke-result-state";
    const bool smoke_result_open_test = args == L"--smoke-result-open";
    const bool smoke_project_link_test = args == L"--smoke-project-link";
    const bool smoke_profile_rescan_test = args == L"--smoke-profile-rescan";
    const bool smoke_accessibility_test = args == L"--smoke-accessibility";
    const bool smoke_accessibility_pages_test = args == L"--smoke-accessibility-pages";
    const bool smoke_settings_write_test = args.starts_with(L"--smoke-settings-write=");
    const bool smoke_settings_read_test = args.starts_with(L"--smoke-settings-read=");
    const bool smoke_settings_invalid_test = args.starts_with(L"--smoke-settings-invalid=");
    const bool capture_error = args.starts_with(L"--capture-error=");
    const bool capture_close = args.starts_with(L"--capture-close=");
    const bool capture_settings_migration = args.starts_with(L"--capture-settings-migration=");
    const bool capture_settings_about = args.starts_with(L"--capture-settings-about=");
    const bool capture_error_min = args.starts_with(L"--capture-error-min=");
    const bool capture_error_english = args.starts_with(L"--capture-error-en=");
    const bool capture_confirm = args.starts_with(L"--capture-confirm=");
    const bool capture_confirm_min = args.starts_with(L"--capture-confirm-min=");
    const bool capture_confirm_english = args.starts_with(L"--capture-confirm-en=");
    const bool capture_confirm_min_english = args.starts_with(L"--capture-confirm-min-en=");
    const bool capture_result = args.starts_with(L"--capture-result=");
    const bool capture_result_failed = args.starts_with(L"--capture-result-failed=");
    const bool capture_result_failed_min = args.starts_with(L"--capture-result-failed-min=");
    const bool capture_result_failed_english = args.starts_with(L"--capture-result-failed-en=");
    const bool capture_post_result = args.starts_with(L"--capture-post-result=");
    const bool capture_post_result_min = args.starts_with(L"--capture-post-result-min=");
    const bool capture_main = args.starts_with(L"--capture=");
    const bool capture_scan_progress = args.starts_with(L"--capture-scan-progress=");
    const bool capture_scan_indeterminate = args.starts_with(L"--capture-scan-indeterminate=");
    const bool capture_search_edit = args.starts_with(L"--capture-search-edit=");
    const bool capture_drawer = args.starts_with(L"--capture-drawer=");
    const bool capture_settings = args.starts_with(L"--capture-settings=");
    const bool capture_populated = args.starts_with(L"--capture-populated=");
    const bool capture_min = args.starts_with(L"--capture-min=");
    const bool capture_language = args.starts_with(L"--capture-language=");
    const bool capture_settings_min = args.starts_with(L"--capture-settings-min=");
    const bool capture_min_english = args.starts_with(L"--capture-min-en=");
    const bool capture_populated_min_english = args.starts_with(L"--capture-populated-min-en=");
    const bool capture_settings_min_english = args.starts_with(L"--capture-settings-min-en=");
    const bool capture_splash = args.starts_with(L"--capture-splash=");
    const bool capture_splash_start = args.starts_with(L"--capture-splash-start=");
    const bool capture_splash_orbit = args.starts_with(L"--capture-splash-orbit=");
    const bool capture_splash_arrow = args.starts_with(L"--capture-splash-arrow=");
    const bool capture_splash_benchmark = args.starts_with(L"--capture-splash-benchmark=");
    const bool capture_splash_end = args.starts_with(L"--capture-splash-end=");
    const bool capture_splash_english = args.starts_with(L"--capture-splash-en=");
    const bool capture_welcome = args.starts_with(L"--capture-welcome=");
    std::optional<fs::path> test_settings_path;
    if (smoke_settings_write_test || smoke_settings_read_test || smoke_settings_invalid_test) {
        try {
            const auto prefix = smoke_settings_write_test ?
                std::wstring_view(L"--smoke-settings-write=").size() :
                smoke_settings_read_test ? std::wstring_view(L"--smoke-settings-read=").size() :
                                           std::wstring_view(L"--smoke-settings-invalid=").size();
            const auto requested = fs::absolute(fs::path(args.substr(prefix)));
            const auto build_root = application_directory().parent_path();
            if (requested.filename() != L"settings.json" ||
                !requested.parent_path().filename().wstring().starts_with(L"settings-smoke-") ||
                !is_within(build_root, requested.parent_path(), true)) return 2;
            ensure_no_reparse_points_between(build_root, requested.parent_path());
            test_settings_path = requested;
        } catch (const std::exception&) { return 2; }
    }
    win_compat::enable_best_dpi_awareness();
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    registered_taskbar_created_message = RegisterWindowMessageW(L"TaskbarCreated");
    HANDLE single_instance = CreateMutexW(nullptr, TRUE, isolated_single_instance ?
        L"Local\\SempervirensSingleInstanceSmoke" : automated_test ?
        L"Local\\SempervirensAutomatedTest" :
        L"Local\\SempervirensSingleInstance");
    if (!single_instance) { CoUninitialize(); return 1; }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        const auto existing = FindWindowW(selected_window_class, nullptr);
        const bool signaled = existing && PostMessageW(existing, wm_tray, 0, WM_LBUTTONDBLCLK);
        CloseHandle(single_instance);
        CoUninitialize();
        return isolated_single_instance ? (smoke_single_instance_child && signaled ? 0 : 2) :
               automated_test ? 2 : 0;
    }
    if (smoke_single_instance_child) { CloseHandle(single_instance); CoUninitialize(); return 3; }
    auto app = std::make_unique<App>(instance, std::move(test_settings_path));
    app->set_single_instance_smoke(smoke_single_instance_test);
    WNDCLASSEXW cls{};
    cls.cbSize = sizeof(cls);
    cls.hInstance = instance;
    cls.lpfnWndProc = window_proc;
    cls.lpszClassName = selected_window_class;
    cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    cls.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1));
    cls.hIconSm = cls.hIcon;
    cls.style = CS_DBLCLKS;
    if (!RegisterClassExW(&cls)) { CloseHandle(single_instance); CoUninitialize(); return 1; }
    HWND hwnd = CreateWindowExW(0, selected_window_class, L"Sempervirens", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                               CW_USEDEFAULT, CW_USEDEFAULT, 1120, 800, nullptr, nullptr, instance, app.get());
    if (!hwnd) { CloseHandle(single_instance); CoUninitialize(); return 1; }
    if (automated_test) app->prepare_automated_test(
        smoke_settings_read_test || smoke_installer_language_test);
    app->set_dpi(win_compat::window_dpi(hwnd));
    if (!place_initial_window(hwnd)) {
        DestroyWindow(hwnd);
        app.reset();
        CloseHandle(single_instance);
        CoUninitialize();
        return 1;
    }
    if (smoke_single_instance_test) {
        const auto result = app->smoke_single_instance();
        DestroyWindow(hwnd);
        app.reset();
        CloseHandle(single_instance);
        CoUninitialize();
        return result;
    }
    if (smoke_tray_lifecycle_test) {
        const auto result = app->smoke_tray_lifecycle();
        DestroyWindow(hwnd);
        app.reset();
        CloseHandle(single_instance);
        CoUninitialize();
        return result;
    }
    if (smoke_search_input_test) {
        const auto result = app->smoke_search_input();
        DestroyWindow(hwnd);
        app.reset();
        CloseHandle(single_instance);
        CoUninitialize();
        return result;
    }
    if (smoke_group_toggle_test) {
        const auto result = app->smoke_group_toggle();
        DestroyWindow(hwnd);
        app.reset();
        CloseHandle(single_instance);
        CoUninitialize();
        return result;
    }
    if (smoke_installer_language_test) {
        const auto result = app->smoke_installer_language();
        DestroyWindow(hwnd);
        app.reset();
        CloseHandle(single_instance);
        CoUninitialize();
        return result;
    }
    const bool compact_capture = capture_min || capture_settings_min || capture_min_english ||
        capture_populated_min_english || capture_settings_min_english || capture_error_min ||
        capture_error_english || capture_confirm_min || capture_confirm_english ||
        capture_confirm_min_english || capture_result_failed_min;
    const bool compact_post_result = capture_post_result_min;
    UINT compact_width = 960;
    UINT compact_height = 720;
    if (compact_capture || compact_post_result) {
        const auto dpi = win_compat::window_dpi(hwnd);
        RECT client{};
        if (!SetWindowPos(hwnd, nullptr, 0, 0, MulDiv(960, dpi, 96), MulDiv(720, dpi, 96),
                          SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE) ||
            !GetClientRect(hwnd, &client) || client.right <= 0 || client.bottom <= 0) {
            DestroyWindow(hwnd);
            app.reset();
            CloseHandle(single_instance);
            CoUninitialize();
            return 1;
        }
        compact_width = static_cast<UINT>(MulDiv(client.right, 96, dpi));
        compact_height = static_cast<UINT>(MulDiv(client.bottom, 96, dpi));
    }
    if (smoke_material_test) {
        const auto result = app->smoke_window_material(args == L"--smoke-material-required");
        DestroyWindow(hwnd);
        app.reset();
        CloseHandle(single_instance);
        CoUninitialize();
        return result;
    }
    if (smoke_resize_test) {
        const auto result = app->smoke_live_resize();
        DestroyWindow(hwnd);
        app.reset();
        CloseHandle(single_instance);
        CoUninitialize();
        return result;
    }
    if (smoke_progress_test) {
        const auto result = app->smoke_progress_coalescing();
        DestroyWindow(hwnd);
        app.reset();
        CloseHandle(single_instance);
        CoUninitialize();
        return result;
    }
    if (smoke_cancel_scan_test) {
        const auto result = app->smoke_scan_cancellation();
        DestroyWindow(hwnd);
        app.reset();
        CloseHandle(single_instance);
        CoUninitialize();
        return result;
    }
    if (smoke_close_during_scan_test) {
        const auto result = app->smoke_close_during_scan();
        if (IsWindow(hwnd)) DestroyWindow(hwnd);
        app.reset();
        CloseHandle(single_instance);
        CoUninitialize();
        return result;
    }
    if (smoke_instance_choice_test) {
        const auto result = app->smoke_instance_choice();
        DestroyWindow(hwnd);
        app.reset();
        CloseHandle(single_instance);
        CoUninitialize();
        return result;
    }
    if (smoke_instance_card_test) {
        const auto result = app->smoke_instance_card_behavior();
        DestroyWindow(hwnd);
        app.reset();
        CloseHandle(single_instance);
        CoUninitialize();
        return result;
    }
    if (smoke_pointer_feedback_test) {
        const auto result = app->smoke_pointer_feedback();
        DestroyWindow(hwnd);
        app.reset();
        CloseHandle(single_instance);
        CoUninitialize();
        return result;
    }
    if (smoke_drawer_scrollbar_test) {
        const auto result = app->smoke_drawer_scrollbar_and_deactivation();
        DestroyWindow(hwnd);
        app.reset();
        CloseHandle(single_instance);
        CoUninitialize();
        return result;
    }
    if (smoke_session_end_test) {
        const auto result = app->smoke_session_end(args == L"--smoke-session-end-destroy");
        if (IsWindow(hwnd)) DestroyWindow(hwnd);
        app.reset();
        CloseHandle(single_instance);
        CoUninitialize();
        return result;
    }
    if (smoke_dpi_change_test) {
        const auto result = app->smoke_dpi_change();
        DestroyWindow(hwnd);
        app.reset();
        CloseHandle(single_instance);
        CoUninitialize();
        return result;
    }
    if (smoke_content_scrollbars_test) {
        const auto result = app->smoke_content_scrollbars();
        DestroyWindow(hwnd);
        app.reset();
        CloseHandle(single_instance);
        CoUninitialize();
        return result;
    }
    if (smoke_migration_close_test) {
        const auto result = app->smoke_migration_close_guard();
        DestroyWindow(hwnd);
        app.reset();
        CloseHandle(single_instance);
        CoUninitialize();
        return result;
    }
    if (smoke_migration_error_test) {
        const auto result = app->smoke_migration_error();
        DestroyWindow(hwnd);
        app.reset();
        CloseHandle(single_instance);
        CoUninitialize();
        return result;
    }
    if (smoke_real_migration_error_test) {
        const auto result = app->smoke_real_migration_error();
        DestroyWindow(hwnd);
        app.reset();
        CloseHandle(single_instance);
        CoUninitialize();
        return result;
    }
    if (smoke_real_migration_success_test) {
        const auto result = app->smoke_real_migration_success(
            args == L"--smoke-real-migration-success-en",
            args == L"--smoke-real-migration-keyboard");
        DestroyWindow(hwnd);
        app.reset();
        CloseHandle(single_instance);
        CoUninitialize();
        return result;
    }
    if (smoke_result_state_test) {
        const auto result = app->smoke_result_state();
        DestroyWindow(hwnd);
        app.reset();
        CloseHandle(single_instance);
        CoUninitialize();
        return result;
    }
    if (smoke_result_open_test) {
        const auto result = app->smoke_result_open_arguments();
        DestroyWindow(hwnd);
        app.reset();
        CloseHandle(single_instance);
        CoUninitialize();
        return result;
    }
    if (smoke_project_link_test) {
        const auto result = app->smoke_project_link();
        DestroyWindow(hwnd);
        app.reset();
        CloseHandle(single_instance);
        CoUninitialize();
        return result;
    }
    if (smoke_profile_rescan_test) {
        const auto result = app->smoke_profile_rescan();
        DestroyWindow(hwnd);
        app.reset();
        CloseHandle(single_instance);
        CoUninitialize();
        return result;
    }
    if (smoke_accessibility_test) {
        const auto result = app->smoke_accessibility();
        DestroyWindow(hwnd);
        app.reset();
        CloseHandle(single_instance);
        CoUninitialize();
        return result;
    }
    if (smoke_accessibility_pages_test) {
        const auto result = app->smoke_accessibility_pages();
        DestroyWindow(hwnd);
        app.reset();
        CloseHandle(single_instance);
        CoUninitialize();
        return result;
    }
    if (smoke_settings_write_test || smoke_settings_read_test || smoke_settings_invalid_test) {
        const auto result = smoke_settings_write_test ? app->smoke_settings_write() :
            smoke_settings_read_test ? app->smoke_settings_read() : app->smoke_settings_invalid();
        DestroyWindow(hwnd);
        app.reset();
        CloseHandle(single_instance);
        CoUninitialize();
        return result;
    }
    if (capture_result || capture_result_failed || capture_result_failed_min ||
        capture_result_failed_english) {
        const auto prefix = capture_result ? std::wstring_view(L"--capture-result=").size() :
            capture_result_failed ? std::wstring_view(L"--capture-result-failed=").size() :
            capture_result_failed_min ? std::wstring_view(L"--capture-result-failed-min=").size() :
                                        std::wstring_view(L"--capture-result-failed-en=").size();
        const auto okay = app->capture(fs::path(args.substr(prefix)), false, false, false,
                                      capture_result_failed_min ? compact_width : 1120U,
                                      capture_result_failed_min ? compact_height : 800U,
                                      capture_result_failed_english, false, false, false, false,
                                      true, !capture_result);
        DestroyWindow(hwnd);
        app.reset();
        CloseHandle(single_instance);
        CoUninitialize();
        return okay ? 0 : 1;
    }
    if (capture_post_result || capture_post_result_min) {
        const auto prefix = capture_post_result ?
            std::wstring_view(L"--capture-post-result=").size() :
            std::wstring_view(L"--capture-post-result-min=").size();
        const auto okay = app->capture(
            fs::path(args.substr(prefix)), false, true, false,
            capture_post_result_min ? compact_width : 1120U,
            capture_post_result_min ? compact_height : 800U,
            false, false, false, false, false,
            false, false, true);
        DestroyWindow(hwnd);
        app.reset();
        CloseHandle(single_instance);
        CoUninitialize();
        return okay ? 0 : 1;
    }
    if (capture_error || capture_error_min || capture_error_english) {
        const auto prefix = capture_error ? std::wstring_view(L"--capture-error=").size() :
            capture_error_min ? std::wstring_view(L"--capture-error-min=").size() :
                                std::wstring_view(L"--capture-error-en=").size();
        const auto compact = capture_error_min || capture_error_english;
        const auto okay = app->capture(fs::path(args.substr(prefix)), false, false, false,
                                      compact ? compact_width : 1120U, compact ? compact_height : 800U,
                                      capture_error_english, true);
        DestroyWindow(hwnd);
        app.reset();
        CloseHandle(single_instance);
        CoUninitialize();
        return okay ? 0 : 1;
    }
    if (capture_confirm || capture_confirm_min || capture_confirm_english ||
        capture_confirm_min_english) {
        const auto prefix = capture_confirm ? std::wstring_view(L"--capture-confirm=").size() :
            capture_confirm_min ? std::wstring_view(L"--capture-confirm-min=").size() :
            capture_confirm_english ? std::wstring_view(L"--capture-confirm-en=").size() :
                                      std::wstring_view(L"--capture-confirm-min-en=").size();
        const auto compact = capture_confirm_min || capture_confirm_min_english;
        const auto english = capture_confirm_english || capture_confirm_min_english;
        const auto okay = app->capture(fs::path(args.substr(prefix)), false, true, false,
                                      compact ? compact_width : 1120U, compact ? compact_height : 800U,
                                      english, false, true);
        DestroyWindow(hwnd);
        app.reset();
        CloseHandle(single_instance);
        CoUninitialize();
        return okay ? 0 : 1;
    }
    if (capture_splash || capture_splash_end || capture_splash_english ||
        capture_splash_start || capture_splash_benchmark || capture_splash_orbit || capture_splash_arrow) {
        const auto prefix = capture_splash ? std::wstring_view(L"--capture-splash=").size() :
            capture_splash_start ? std::wstring_view(L"--capture-splash-start=").size() :
            capture_splash_orbit ? std::wstring_view(L"--capture-splash-orbit=").size() :
            capture_splash_arrow ? std::wstring_view(L"--capture-splash-arrow=").size() :
            capture_splash_benchmark ? std::wstring_view(L"--capture-splash-benchmark=").size() :
            capture_splash_end ? std::wstring_view(L"--capture-splash-end=").size() :
                                 std::wstring_view(L"--capture-splash-en=").size();
        const auto okay = capture_startup_splash(fs::path(args.substr(prefix)),
            capture_splash_english || app->startup_english(),
            capture_splash_start ? 160.0f : capture_splash_orbit ? 680.0f :
            capture_splash_arrow ? 790.0f : capture_splash_end ? 1040.0f : 470.0f,
            capture_splash_benchmark);
        DestroyWindow(hwnd);
        app.reset();
        CloseHandle(single_instance);
        CoUninitialize();
        return okay ? 0 : 1;
    }
    if (capture_welcome) {
        app->begin_onboarding();
        const auto okay = app->capture(
            fs::path(args.substr(std::wstring_view(L"--capture-welcome=").size())),
            false, false, false, 1120U, 800U, false, false, false, false, false,
            false, false, false, true);
        DestroyWindow(hwnd);
        app.reset();
        CloseHandle(single_instance);
        CoUninitialize();
        return okay ? 0 : 1;
    }
    if (capture_search_edit) {
        app->prepare_search_visual_sample();
        const auto okay = app->capture(
            fs::path(args.substr(std::wstring_view(L"--capture-search-edit=").size())),
            false, true, false);
        DestroyWindow(hwnd);
        app.reset();
        CloseHandle(single_instance);
        CoUninitialize();
        return okay ? 0 : 1;
    }
    if (capture_drawer) {
        app->prepare_drawer_visual_sample();
        const auto okay = app->capture(
            fs::path(args.substr(std::wstring_view(L"--capture-drawer=").size())),
            false, false, false);
        DestroyWindow(hwnd);
        app.reset();
        CloseHandle(single_instance);
        CoUninitialize();
        return okay ? 0 : 1;
    }
    if (capture_close) {
        app->prepare_close_visual_sample();
        const auto okay = app->capture(
            fs::path(args.substr(std::wstring_view(L"--capture-close=").size())),
            false, false, false);
        DestroyWindow(hwnd);
        app.reset();
        CloseHandle(single_instance);
        CoUninitialize();
        return okay ? 0 : 1;
    }
    if (capture_settings_migration || capture_settings_about) {
        app->prepare_settings_visual_sample(capture_settings_migration ?
            SettingsSection::migration : SettingsSection::about);
        const auto prefix = capture_settings_migration ?
            std::wstring_view(L"--capture-settings-migration=").size() :
            std::wstring_view(L"--capture-settings-about=").size();
        const auto okay = app->capture(fs::path(args.substr(prefix)), true, false, false);
        DestroyWindow(hwnd);
        app.reset();
        CloseHandle(single_instance);
        CoUninitialize();
        return okay ? 0 : 1;
    }
    if (smoke_splash_test) {
        const auto okay = show_startup_splash(instance, hwnd, app->startup_english(), false,
                                              args == L"--smoke-splash-skip");
        DestroyWindow(hwnd);
        app.reset();
        CloseHandle(single_instance);
        CoUninitialize();
        return okay ? 0 : 1;
    }
    if (capture_main || capture_scan_progress || capture_scan_indeterminate ||
        capture_settings || capture_populated || capture_min ||
        capture_language || capture_settings_min || capture_min_english ||
        capture_populated_min_english || capture_settings_min_english) {
        const auto prefix_length = capture_main ? std::wstring_view(L"--capture=").size() :
            capture_scan_progress ? std::wstring_view(L"--capture-scan-progress=").size() :
            capture_scan_indeterminate ? std::wstring_view(L"--capture-scan-indeterminate=").size() :
            capture_settings ? std::wstring_view(L"--capture-settings=").size() :
                capture_populated ? std::wstring_view(L"--capture-populated=").size() :
                    capture_min ? std::wstring_view(L"--capture-min=").size() :
                        capture_language ? std::wstring_view(L"--capture-language=").size() :
                        capture_settings_min ? std::wstring_view(L"--capture-settings-min=").size() :
                        capture_min_english ? std::wstring_view(L"--capture-min-en=").size() :
                        capture_populated_min_english ? std::wstring_view(L"--capture-populated-min-en=").size() :
                            std::wstring_view(L"--capture-settings-min-en=").size();
        const auto okay = app->capture(fs::path(args.substr(prefix_length)),
                                      capture_settings || capture_language || capture_settings_min ||
                                          capture_settings_min_english,
                                      capture_populated || capture_min || capture_populated_min_english ||
                                          capture_scan_progress || capture_scan_indeterminate,
                                      capture_language,
                                      capture_min || capture_settings_min || capture_min_english ||
                                          capture_populated_min_english || capture_settings_min_english ? compact_width : 1120U,
                                      capture_min || capture_settings_min || capture_min_english ||
                                          capture_populated_min_english || capture_settings_min_english ? compact_height : 800U,
                                      capture_min_english || capture_populated_min_english ||
                                          capture_settings_min_english,
                                      false, false,
                                      capture_scan_progress || capture_scan_indeterminate,
                                      capture_scan_indeterminate);
        DestroyWindow(hwnd);
        app.reset();
        CloseHandle(single_instance);
        CoUninitialize();
        return okay ? 0 : 1;
    }
    const bool smoke_drop_ok = !smoke_drop_test ||
        smoke_test_folder_drop(hwnd, *app, args != L"--smoke-drop-target",
                               args == L"--smoke-drop-multiple");
    if (smoke_test || smoke_drop_test) PostMessageW(hwnd, WM_COMMAND, tray_exit_command, 0);
    else {
        if (show != SW_HIDE && app->startup_motion_enabled()) {
            app->set_startup_splash_active(true);
            show_startup_splash(instance, hwnd, app->startup_english());
            app->set_startup_splash_active(false);
        }
        ShowWindow(hwnd, show);
        UpdateWindow(hwnd);
        if (!update_health_token.empty() && update_health_token.size() <= 80 &&
            std::all_of(update_health_token.begin(), update_health_token.end(), [](wchar_t value) {
                return iswalnum(value) != 0;
            })) try {
                const auto health = update::update_data_root() / L"health" / update_health_token;
                fs::create_directories(health.parent_path());
                write_file_atomically(health, "ok");
            } catch (...) { }
        app->start_automatic_update_check();
    }
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    app.reset();
    CloseHandle(single_instance);
    CoUninitialize();
    return smoke_drop_ok ? static_cast<int>(message.wParam) : 1;
}
