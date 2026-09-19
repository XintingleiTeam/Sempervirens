#include "atomic_file.hpp"
#include "config_difference.hpp"
#include "execution.hpp"
#include "instance_discovery.hpp"
#include "nbt.hpp"
#include "path_safety.hpp"
#include "preferences.hpp"
#include "presentation.hpp"
#include "profile.hpp"
#include "scan.hpp"
#include "text.hpp"

#include <nlohmann/json.hpp>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <array>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>

using namespace sempervirens;

static void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

static void write_text(const fs::path& path, const std::string& text) {
    fs::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary);
    output << text;
    if (!output) throw std::runtime_error("Could not write test fixture");
}

int main() {
    const auto root = fs::temp_directory_path() /
        (L"SempervirensCppTest-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    fs::create_directories(root);
    try {
        require(utf8_to_wide(wide_to_utf8(L"迁移设置")) == L"迁移设置", "Unicode conversion failed");
        require(utf8_with_replacement(std::string("Bad\xFF", 4)) == "Bad\xEF\xBF\xBD",
                "Malformed NBT UTF-8 did not use .NET replacement semantics");
        require(display_profile_name("Sempervirens-新亭泪社区专用迁移器", true) ==
                    L"Xintinglei community profile" &&
                display_profile_name("Custom migration rules", true) == L"Custom migration rules",
                "English profile display name or custom profile name was changed");
        require(is_safe_relative_pattern("config/itemscroller.json"), "Safe relative path rejected");
        require(!is_safe_relative_pattern("../outside.txt"), "Traversal accepted");
        require(!is_safe_relative_pattern("C:\\outside.txt"), "Absolute drive path accepted");
        require(!is_safe_relative_pattern("config/../outside.txt"), "Nested traversal accepted");
        require(unicode_blank("\u3000") && !is_safe_relative_pattern("\u3000"),
                "Unicode-only whitespace path was accepted");
        require(is_within(root, root / "child"), "Child path rejected");
        require(!is_within(root, root.parent_path() / "SempervirensCppTest-other"), "Sibling path accepted");
        require(!is_within(root, root, false), "Root accepted when strict child required");
        const auto real_directory = root / "real-directory";
        const auto directory_link = root / "linked-directory";
        fs::create_directories(real_directory);
        if (CreateSymbolicLinkW(directory_link.c_str(), real_directory.c_str(),
                                SYMBOLIC_LINK_FLAG_DIRECTORY | 0x2)) {
            bool link_rejected = false;
            try { ensure_no_reparse_points_between(root, directory_link / "payload.txt"); }
            catch (const std::exception&) { link_rejected = true; }
            require(link_rejected, "A directory symlink was accepted inside an instance");
        } else std::cout << "Symlink fixture unavailable; testing the system junction instead\n";
        const auto system_junction = fs::path(L"C:\\Documents and Settings");
        const auto junction_attributes = GetFileAttributesW(system_junction.c_str());
        if (junction_attributes != INVALID_FILE_ATTRIBUTES &&
            (junction_attributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
            bool junction_rejected = false;
            try { ensure_no_reparse_points_between(fs::path(L"C:\\"),
                                                    system_junction / "blocked.txt"); }
            catch (const std::exception&) { junction_rejected = true; }
            require(junction_rejected, "An existing Windows junction was accepted");
            const auto alternate_case = fs::path(L"c:\\Documents and Settings\\blocked.txt");
            require(is_within(fs::path(L"C:\\"), alternate_case),
                    "A case-insensitive Windows path was not recognized");
            bool mixed_case_junction_rejected = false;
            try { ensure_no_reparse_points_between(fs::path(L"C:\\"), alternate_case); }
            catch (const std::exception&) { mixed_case_junction_rejected = true; }
            require(mixed_case_junction_rejected,
                    "A junction escaped inspection when the drive letter case differed");
        }

        const auto probe_collision = root / "write-probe" / "existing.tmp";
        write_text(probe_collision, "keep existing content");
        require(!try_probe_writable_for_test(probe_collision),
                "The destination write probe did not reject an existing file");
        std::ifstream collision_input(probe_collision, std::ios::binary);
        const std::string collision_content(std::istreambuf_iterator<char>{collision_input}, {});
        require(collision_content == "keep existing content",
                "The destination write probe changed an existing file");
        const auto fresh_probe = root / "write-probe" / "fresh.tmp";
        require(try_probe_writable_for_test(fresh_probe) && !fs::exists(fresh_probe),
                "The destination write probe was not removed after a successful check");

        const auto atomic_folder = root / "atomic-write";
        const auto atomic_target = atomic_folder / "settings.txt";
        write_text(atomic_target, "previous settings");
        std::string large_settings(1024 * 1024, 'n');
        std::stop_source atomic_stop;
        bool observed_partial_write = false;
        set_atomic_write_progress_observer_for_test([&](std::size_t written, std::size_t total) {
            if (written > 0 && written < total) {
                observed_partial_write = true;
                atomic_stop.request_stop();
            }
        });
        bool atomic_cancelled = false;
        try { write_file_atomically(atomic_target, large_settings, atomic_stop.get_token()); }
        catch (const std::exception&) { atomic_cancelled = atomic_stop.stop_requested(); }
        set_atomic_write_progress_observer_for_test({});
        std::ifstream preserved_settings(atomic_target, std::ios::binary);
        const std::string preserved_text(std::istreambuf_iterator<char>{preserved_settings}, {});
        require(observed_partial_write && atomic_cancelled && preserved_text == "previous settings",
                "Cancelling an atomic settings write changed the destination");
        for (const auto& entry : fs::directory_iterator(atomic_folder))
            require(!entry.path().filename().wstring().starts_with(L".sempervirens-tmp-"),
                    "Cancelling an atomic settings write left a partial file");

        const auto legacy_settings = root / "original-settings" / "settings.json";
        const auto preview_settings = root / "preview-settings" / "settings.json";
        const auto custom_profile_path = root / L"自定义规则.json";
        write_text(legacy_settings, nlohmann::json{
            {"Language", "TraditionalChineseHongKong"},
            {"PromptOnClose", false},
            {"CloseAction", "MinimizeToTray"},
            {"MigrationProfilePath", wide_to_utf8(custom_profile_path.wstring())}
        }.dump());
        const auto imported = read_preferences(legacy_settings);
        require(imported && !imported->english && !imported->prompt_on_close &&
                imported->close_to_tray && imported->migration_profile_path == custom_profile_path,
                "Original .NET settings were not imported correctly");
        auto edited_preferences = *imported;
        edited_preferences.english = true;
        edited_preferences.use_minecraft_profile = true;
        const auto legacy_preferences_temp = fs::path(preview_settings.wstring() + L".tmp-" +
            std::to_wstring(GetCurrentProcessId()));
        write_text(legacy_preferences_temp, "previous temporary data");
        write_preferences(preview_settings, edited_preferences);
        {
            std::ifstream old_temp(legacy_preferences_temp, std::ios::binary);
            require(std::string(std::istreambuf_iterator<char>{old_temp}, {}) ==
                    "previous temporary data", "Saving preferences overwrote an old temporary file");
        }
        const auto persisted = read_preferences(preview_settings);
        require(persisted && persisted->english && !persisted->prompt_on_close &&
                persisted->close_to_tray && persisted->migration_profile_path == custom_profile_path &&
                persisted->use_minecraft_profile,
                "Preview settings did not survive a save and reload");
        const auto original_after_save = read_preferences(legacy_settings);
        require(original_after_save && !original_after_save->english,
                "Saving preview settings changed the original settings");
        write_text(root / "invalid-settings.json", "{\"Language\":42}");
        require(!read_preferences(root / "invalid-settings.json"),
                "Malformed preferences were accepted");

        NbtTag server_root;
        server_root.type = NbtType::compound;
        NbtTag list;
        list.type = NbtType::list;
        list.item_type = NbtType::compound;
        NbtTag server;
        server.type = NbtType::compound;
        NbtTag server_name;
        server_name.type = NbtType::string;
        server_name.text = "测试服务器";
        server.set("name", server_name);
        NbtTag server_address;
        server_address.type = NbtType::string;
        server_address.text = "play.example.org";
        server.set("ip", server_address);
        NbtTag icon;
        icon.type = NbtType::byte_array;
        icon.bytes = {0, 1, 2, 255};
        server.set("unknownIcon", icon);
        list.items.push_back(server);
        server_root.set("servers", list);
        NbtTag marker;
        marker.type = NbtType::long_array;
        marker.long_integers = {-1, 0, 123456789};
        server_root.set("unknownRootField", marker);
        const auto nbt_path = root / "servers.dat";
        const auto legacy_nbt_temp = fs::path(nbt_path.wstring() + L".sempervirens-tmp-" +
            std::to_wstring(GetCurrentProcessId()));
        write_text(legacy_nbt_temp, "previous temporary NBT data");
        write_nbt_compound(nbt_path, server_root);
        {
            std::ifstream old_temp(legacy_nbt_temp, std::ios::binary);
            require(std::string(std::istreambuf_iterator<char>{old_temp}, {}) ==
                    "previous temporary NBT data", "Writing NBT overwrote an old temporary file");
        }
        const auto nbt_roundtrip = read_nbt_compound(nbt_path);
        require(nbt_roundtrip.get("servers") && nbt_roundtrip.get("servers")->items.size() == 1,
                "NBT server list did not round-trip");
        require(nbt_roundtrip.get("servers")->items[0].get_string("name") == "测试服务器",
                "NBT UTF-8 name was lost");
        require(nbt_roundtrip.get("servers")->items[0].get("unknownIcon") &&
                nbt_roundtrip.get("servers")->items[0].get("unknownIcon")->bytes == icon.bytes,
                "NBT unknown byte array was lost");
        require(nbt_roundtrip.get("unknownRootField") &&
                nbt_roundtrip.get("unknownRootField")->long_integers == marker.long_integers,
                "NBT unknown root field was lost");
        NbtTag float_nan_left;
        float_nan_left.type = NbtType::floating;
        float_nan_left.decimal = std::numeric_limits<float>::quiet_NaN();
        auto float_nan_right = float_nan_left;
        NbtTag double_nan_left;
        double_nan_left.type = NbtType::double_precision;
        double_nan_left.decimal = std::numeric_limits<double>::quiet_NaN();
        auto double_nan_right = double_nan_left;
        require(nbt_equal(float_nan_left, float_nan_right) &&
                nbt_equal(double_nan_left, double_nan_right),
                "NBT NaN values diverged from .NET equality semantics");
        auto nan_server_left = server;
        auto nan_server_right = server;
        nan_server_left.set("unknownFloat", float_nan_left);
        nan_server_right.set("unknownFloat", float_nan_right);
        require(nbt_server_other_fields_equal(nan_server_left, nan_server_right),
                "Matching NaN server metadata was reported as different");
        double_nan_right.decimal = 1.0;
        require(!nbt_equal(double_nan_left, double_nan_right),
                "A NaN NBT value was treated as an ordinary number");
        write_text(root / "broken-servers.dat", std::string("\x1f\x8b", 2));
        bool malformed_nbt_rejected = false;
        try { (void)read_nbt_compound(root / "broken-servers.dat"); }
        catch (const std::exception&) { malformed_nbt_rejected = true; }
        require(malformed_nbt_rejected, "Malformed gzip NBT was accepted");
        require(invariant_lower_utf8("BÜCHER.example") == "bücher.example",
                "Unicode server address casing was not normalized");
        const auto unicode_source = root / "unicode-server-source";
        const auto unicode_target = root / "unicode-server-target";
        fs::create_directories(unicode_source);
        fs::create_directories(unicode_target);
        auto unicode_source_document = server_root;
        auto unicode_target_document = server_root;
        auto unicode_source_address = server_address;
        unicode_source_address.text = "BÜCHER.example";
        unicode_source_document.get("servers")->items[0].set("ip", unicode_source_address);
        auto unicode_target_address = server_address;
        unicode_target_address.text = "bücher.example";
        unicode_target_document.get("servers")->items[0].set("ip", unicode_target_address);
        auto unicode_target_name = server_name;
        unicode_target_name.text = "目标中的名称";
        unicode_target_document.get("servers")->items[0].set("name", unicode_target_name);
        write_nbt_compound(unicode_source / "servers.dat", unicode_source_document);
        write_nbt_compound(unicode_target / "servers.dat", unicode_target_document);
        MigrationProfile unicode_server_profile;
        unicode_server_profile.name = "Unicode server identity";
        unicode_server_profile.rules = {{"servers", "servers", "Servers", "", RuleKind::builtin,
            BuiltinKind::servers, "", "servers.dat", "servers.dat", true, true}};
        const auto unicode_server_plan = build_plan(unicode_server_profile, unicode_source, unicode_target);
        require(unicode_server_plan.operations.size() == 1 &&
                unicode_server_plan.operations[0].status == ScanStatus::conflict,
                "Unicode-case server addresses were incorrectly treated as different servers");
        const auto unicode_server_result = execute_migration(unicode_server_plan,
            OverwriteStrategy::backup_and_replace);
        const auto unicode_after = read_nbt_compound(unicode_target / "servers.dat");
        require(unicode_server_result.failed_count == 0 &&
                unicode_after.get("servers") && unicode_after.get("servers")->items.size() == 1 &&
                unicode_after.get("servers")->items[0].get_string("name") == "目标中的名称",
                "Unicode server merge duplicated an address or lost the destination name");
        const auto stale_source = root / "stale-server-source";
        const auto stale_target = root / "stale-server-target";
        fs::create_directories(stale_source);
        fs::create_directories(stale_target);
        write_nbt_compound(stale_source / "servers.dat", unicode_source_document);
        write_nbt_compound(stale_target / "servers.dat", unicode_target_document);
        const auto stale_plan = build_plan(unicode_server_profile, stale_source, stale_target);
        require(stale_plan.operations.size() == 1 && stale_plan.operations[0].selected,
                "Stale-server fixture did not select its source entry");
        auto source_without_selection = unicode_source_document;
        source_without_selection.get("servers")->items.clear();
        write_nbt_compound(stale_source / "servers.dat", source_without_selection);
        const auto stale_result = execute_migration(stale_plan, OverwriteStrategy::replace);
        const auto stale_destination = read_nbt_compound(stale_target / "servers.dat");
        require(stale_result.failed_count == 1 && stale_result.files_copied == 0 &&
                stale_destination.get("servers") && stale_destination.get("servers")->items.size() == 1 &&
                stale_destination.get("servers")->items[0].get_string("name") == "目标中的名称",
                "A source server removed after scanning deleted or rewrote the destination entry");
        auto duplicate_server_document = unicode_after;
        auto duplicate_server_entry = duplicate_server_document.get("servers")->items[0];
        duplicate_server_entry.set("ip", unicode_source_address);
        duplicate_server_document.get("servers")->items.push_back(std::move(duplicate_server_entry));
        write_nbt_compound(unicode_target / "servers.dat", duplicate_server_document);
        const auto duplicate_server_plan = build_plan(unicode_server_profile,
            unicode_source, unicode_target);
        require(duplicate_server_plan.operations.size() == 1 &&
                duplicate_server_plan.operations[0].status == ScanStatus::config_error &&
                !duplicate_server_plan.operations[0].selected,
                "Duplicate destination server identity was not rejected during scanning");
        const auto duplicate_server_explanation = explain_migration(
            duplicate_server_plan.operations[0], duplicate_server_plan.profile,
            OverwriteStrategy::replace, false);
        require(duplicate_server_explanation.technical.find(L"重复地址") != std::wstring::npos &&
                duplicate_server_explanation.technical.find(L"Duplicate server identity") == std::wstring::npos,
                "Configuration error exposed an internal exception instead of recovery guidance");
        const auto stale_server_result = execute_migration(unicode_server_plan,
            OverwriteStrategy::replace);
        require(stale_server_result.failed_count == 1 &&
                read_nbt_compound(unicode_target / "servers.dat").get("servers")->items.size() == 2,
                "Duplicate destination server identity was not rejected during migration");
        const auto blank_server_source = root / "blank-server-source";
        const auto blank_server_target = root / "blank-server-target";
        fs::create_directories(blank_server_source);
        fs::create_directories(blank_server_target);
        auto blank_server_document = server_root;
        auto empty_server_text = server_name;
        empty_server_text.text.clear();
        blank_server_document.get("servers")->items[0].set("name", empty_server_text);
        blank_server_document.get("servers")->items[0].set("ip", empty_server_text);
        write_nbt_compound(blank_server_source / "servers.dat", blank_server_document);
        const auto blank_server_plan = build_plan(unicode_server_profile,
            blank_server_source, blank_server_target);
        require(blank_server_plan.operations.size() == 1 &&
                blank_server_plan.operations[0].status == ScanStatus::found &&
                std::get<ServerEntry>(blank_server_plan.operations[0].payload).identity.empty() &&
                std::get<ServerEntry>(blank_server_plan.operations[0].payload).name == "Server 1",
                "Blank server address was given the display fallback as its identity");
        const auto blank_server_result = execute_migration(blank_server_plan,
            OverwriteStrategy::replace);
        const auto blank_server_output = read_nbt_compound(blank_server_target / "servers.dat");
        require(blank_server_result.success_count == 1 && blank_server_result.failed_count == 0 &&
                blank_server_output.get("servers")->items.size() == 1 &&
                blank_server_output.get("servers")->items[0].get_string("ip").empty(),
                "A blank-address server failed to migrate");
        const auto duplicate_source = root / "duplicate-server-source";
        const auto duplicate_target = root / "duplicate-server-target";
        fs::create_directories(duplicate_source);
        fs::create_directories(duplicate_target);
        write_nbt_compound(duplicate_source / "servers.dat", duplicate_server_document);
        const auto duplicate_source_plan = build_plan(unicode_server_profile,
            duplicate_source, duplicate_target);
        require(duplicate_source_plan.operations.size() == 2 &&
                duplicate_source_plan.operations[0].selected &&
                duplicate_source_plan.operations[1].selected,
                "Duplicate source servers were not offered by scanning");
        const auto duplicate_source_result = execute_migration(duplicate_source_plan,
            OverwriteStrategy::replace);
        require(duplicate_source_result.failed_count == 1 &&
                !fs::exists(duplicate_target / "servers.dat"),
                "Duplicate source servers were silently collapsed during migration");
        const auto unicode_world_source = root / "unicode-world-source";
        const auto unicode_world_target = root / "unicode-world-target";
        write_text(unicode_world_source / "saves" / "World" / fs::path(L"Ä.txt"), "same");
        write_text(unicode_world_target / "saves" / "World" / fs::path(L"ä.txt"), "same");
        MigrationProfile unicode_world_profile;
        unicode_world_profile.name = "Unicode world paths";
        unicode_world_profile.rules = {{"worlds", "worlds", "Worlds", "", RuleKind::builtin,
            BuiltinKind::worlds, "", "saves", "saves", true, true}};
        bool world_percent_started = false;
        bool target_check_indeterminate = false;
        const auto unicode_world_plan = build_plan(unicode_world_profile,
            unicode_world_source, unicode_world_target, {}, [&](const ScanProgress& value) {
                if (value.percent && *value.percent > 0) world_percent_started = true;
                if (!value.percent && world_percent_started) target_check_indeterminate = true;
            });
        require(unicode_world_plan.operations.size() == 1 &&
                unicode_world_plan.operations[0].status == ScanStatus::identical,
                "Unicode-case world filenames produced a false destination-only difference");
        require(target_check_indeterminate,
                "Target world comparison reported completion before the destination was checked");
        const auto empty_world_source = root / "empty-world-source";
        const auto empty_world_target = root / "empty-world-target";
        fs::create_directories(empty_world_source / "saves" / "Empty");
        fs::create_directories(empty_world_target);
        const auto empty_world_found = build_plan(unicode_world_profile,
            empty_world_source, empty_world_target);
        require(empty_world_found.operations.size() == 1 &&
                empty_world_found.operations[0].status == ScanStatus::found &&
                empty_world_found.operations[0].selected,
                "An empty world missing from the destination was hidden from migration");
        const auto empty_world_result = execute_migration(empty_world_found, OverwriteStrategy::replace);
        require(empty_world_result.success_count == 1 && empty_world_result.skipped_count == 0 &&
                empty_world_result.items.size() == 1 &&
                empty_world_result.items[0].status == "Skipped",
                "Empty world result counters differ from the original application");
        fs::create_directories(empty_world_target / "saves" / "Empty");
        const auto empty_world_identical = build_plan(unicode_world_profile,
            empty_world_source, empty_world_target);
        require(empty_world_identical.operations.size() == 1 &&
                empty_world_identical.operations[0].status == ScanStatus::identical,
                "Matching empty world folders were not marked identical");
        write_text(empty_world_target / "saves" / "Empty" / "target-only.txt", "target");
        const auto empty_world_conflict = build_plan(unicode_world_profile,
            empty_world_source, empty_world_target);
        require(empty_world_conflict.operations.size() == 1 &&
                empty_world_conflict.operations[0].status == ScanStatus::conflict &&
                std::get<FolderComparisonInfo>(empty_world_conflict.operations[0].comparison)
                    .examples[0].kind == FolderDifferenceKind::target_only,
                "Destination content in an empty source world was not reported as a conflict");
        const auto identical_source = root / "identical-source";
        const auto identical_target = root / "identical-target";
        write_text(identical_source / "same.txt", "same");
        write_text(identical_target / "same.txt", "same");
        MigrationProfile identical_profile;
        identical_profile.name = "Identical file";
        identical_profile.rules = {{"same", "test", "Same", "", RuleKind::file,
            std::nullopt, "", "same.txt", "same.txt", true, true}};
        const auto identical_plan = build_plan(identical_profile, identical_source, identical_target);
        const auto identical_result = execute_migration(identical_plan, OverwriteStrategy::replace);
        require(identical_result.success_count == 1 && identical_result.skipped_count == 0 &&
                identical_result.files_copied == 0 && identical_result.items.size() == 1 &&
                identical_result.items[0].status == "Skipped",
                "Identical file result counters differ from the original application");
        const auto readonly_source = root / "readonly-source";
        const auto readonly_target = root / "readonly-target";
        write_text(readonly_source / "locked.txt", "incoming");
        write_text(readonly_target / "locked.txt", "existing");
        MigrationProfile readonly_profile;
        readonly_profile.name = "Read-only destination";
        readonly_profile.rules = {{"locked", "test", "Locked", "", RuleKind::file,
            std::nullopt, "", "locked.txt", "locked.txt", true, true}};
        const auto readonly_plan = build_plan(readonly_profile, readonly_source, readonly_target);
        const auto locked_path = readonly_target / "locked.txt";
        const auto original_attributes = GetFileAttributesW(locked_path.c_str());
        require(original_attributes != INVALID_FILE_ATTRIBUTES &&
                SetFileAttributesW(locked_path.c_str(), original_attributes | FILE_ATTRIBUTE_READONLY),
                "Could not prepare read-only destination fixture");
        const auto readonly_result = execute_migration(readonly_plan, OverwriteStrategy::backup_and_replace);
        SetFileAttributesW(locked_path.c_str(), original_attributes);
        if (readonly_result.backup_root) {
            const auto backup = *readonly_result.backup_root / "locked.txt";
            SetFileAttributesW(backup.c_str(), FILE_ATTRIBUTE_NORMAL);
        }
        std::ifstream locked_input(locked_path, std::ios::binary);
        const std::string locked_contents(std::istreambuf_iterator<char>{locked_input}, {});
        require(readonly_result.failed_count == 1 && readonly_result.files_failed == 1 &&
                readonly_result.backup_root &&
                fs::exists(*readonly_result.backup_root / "locked.txt") &&
                readonly_result.items.size() == 1 && readonly_result.items[0].status == "Failed" &&
                locked_contents == "existing",
                "Read-only destination was changed or its failed copy was not recorded");
        for (const auto& entry : fs::recursive_directory_iterator(readonly_target))
            require(!entry.path().filename().wstring().starts_with(L".sempervirens-copy-"),
                    "A failed copy left its temporary file in the destination instance");

        const auto cancelled_copy_source = root / "cancelled-copy-source";
        const auto cancelled_copy_target = root / "cancelled-copy-target";
        fs::create_directories(cancelled_copy_source);
        write_text(cancelled_copy_target / "big.bin", "previous target data");
        {
            std::ofstream large_file(cancelled_copy_source / "big.bin", std::ios::binary);
            std::array<char, 64 * 1024> block{};
            block.fill('s');
            for (int index = 0; index < 256; ++index) large_file.write(block.data(), block.size());
            require(static_cast<bool>(large_file), "Could not create large cancellation fixture");
        }
        MigrationProfile cancelled_copy_profile;
        cancelled_copy_profile.name = "Cancelled large copy";
        cancelled_copy_profile.rules = {{"large", "test", "Large file", "", RuleKind::file,
            std::nullopt, "", "big.bin", "big.bin", true, true}};
        const auto cancelled_copy_plan = build_plan(cancelled_copy_profile,
            cancelled_copy_source, cancelled_copy_target);
        std::stop_source copy_stop;
        bool observed_partial_copy = false;
        set_copy_progress_observer_for_test([&](std::uint64_t copied, std::uint64_t total) {
            if (copied > 0 && copied < total) {
                observed_partial_copy = true;
                copy_stop.request_stop();
            }
        });
        bool copy_cancelled = false;
        try {
            (void)execute_migration(cancelled_copy_plan, OverwriteStrategy::replace,
                                    copy_stop.get_token());
        } catch (const std::exception&) {
            copy_cancelled = copy_stop.stop_requested();
        }
        set_copy_progress_observer_for_test({});
        std::ifstream cancelled_target_file(cancelled_copy_target / "big.bin", std::ios::binary);
        const std::string cancelled_target_contents(std::istreambuf_iterator<char>{cancelled_target_file}, {});
        require(observed_partial_copy && copy_cancelled &&
                cancelled_target_contents == "previous target data",
                "Cancelling a partially copied file changed the destination");
        for (const auto& entry : fs::directory_iterator(cancelled_copy_target))
            require(!entry.path().filename().wstring().starts_with(L".sempervirens-copy-"),
                    "Cancelling a partially copied file left a temporary file");
        fs::copy_file(cancelled_copy_source / "big.bin", cancelled_copy_target / "big.bin",
                      fs::copy_options::overwrite_existing);
        write_text(cancelled_copy_source / "big.bin", "incoming replacement");
        const auto cancelled_backup_plan = build_plan(cancelled_copy_profile,
            cancelled_copy_source, cancelled_copy_target);
        std::stop_source backup_stop;
        bool observed_partial_backup = false;
        set_copy_progress_observer_for_test([&](std::uint64_t copied, std::uint64_t total) {
            if (copied > 0 && copied < total) {
                observed_partial_backup = true;
                backup_stop.request_stop();
            }
        });
        bool backup_cancelled = false;
        try {
            (void)execute_migration(cancelled_backup_plan, OverwriteStrategy::backup_and_replace,
                                    backup_stop.get_token());
        } catch (const std::exception&) {
            backup_cancelled = backup_stop.stop_requested();
        }
        set_copy_progress_observer_for_test({});
        require(observed_partial_backup && backup_cancelled &&
                fs::file_size(cancelled_copy_target / "big.bin") == 16U * 1024U * 1024U,
                "Cancelling a partially copied backup changed the destination");
        for (const auto& entry : fs::recursive_directory_iterator(cancelled_copy_target)) {
            require(!entry.path().filename().wstring().starts_with(L".sempervirens-copy-"),
                    "Cancelling a backup left a temporary file");
            if (entry.is_regular_file() && entry.path() != cancelled_copy_target / "big.bin")
                require(false, "Cancelling a backup left an incomplete backup file");
        }

        write_text(root / "custom.json", R"json({
            // Comment and trailing commas are supported by the source app.
            "SchemaVersion": 1,
            "Name": "测试规则",
            "RULES": [{"ID":"test", "NAME":"文件", "KIND":"file", "Source":"a.txt", "Target":"b.txt", "category":null,}],
        })json");
        const auto custom = load_profile(root / "custom.json");
        require(custom.rules.size() == 1 && custom.rules[0].target == "b.txt", "Custom profile failed");
        write_text(root / "numeric-enums.json",
            R"({"rules":[{"id":"server","name":"Server","kind":" 3 ","builtin":"1","source":"servers.dat","target":"servers.dat"}]})");
        const auto numeric_enums = load_profile(root / "numeric-enums.json");
        require(numeric_enums.rules.size() == 1 &&
                numeric_enums.rules[0].kind == RuleKind::builtin &&
                numeric_enums.rules[0].builtin == BuiltinKind::servers,
                "Valid numeric enum strings from the original profile format were rejected");
        write_text(root / "undefined-numeric-enum.json",
            R"({"rules":[{"id":"bad","name":"Bad","kind":"5","source":"a","target":"b"}]})");
        bool undefined_enum_rejected = false;
        try { (void)load_profile(root / "undefined-numeric-enum.json"); }
        catch (const std::exception&) { undefined_enum_rejected = true; }
        require(undefined_enum_rejected, "Undefined numeric rule kind was accepted");
        write_text(root / "duplicate-fields.json",
            R"json({"NAME":"Earlier","name":"Later","rules":[{"id":"one","name":"One","SOURCE":"old.txt","source":"new.txt","target":"copy.txt"}]})json");
        const auto duplicate_fields = load_profile(root / "duplicate-fields.json");
        require(duplicate_fields.name == "Later" && duplicate_fields.rules.size() == 1 &&
                duplicate_fields.rules[0].source == "new.txt",
                "Profile property casing changed which source path is migrated");
        const auto encoded_profile_text = std::string(
            R"json({"name":"Encoded","rules":[{"id":"one","name":"One","source":"a.txt","target":"b.txt"}]})json");
        std::string encoded_profile("\xFF\xFE", 2);
        for (const char character : encoded_profile_text) {
            encoded_profile.push_back(character);
            encoded_profile.push_back('\0');
        }
        write_text(root / "profile-utf16.json", encoded_profile);
        const auto utf16_profile = load_profile(root / "profile-utf16.json");
        require(utf16_profile.name == "Encoded" && utf16_profile.rules.size() == 1 &&
                utf16_profile.rules[0].target == "b.txt",
                "UTF-16 migration profile could not be loaded");
        write_text(root / "bad.json", R"json({"name":"Bad","rules":[{"id":"x","name":"X","source":"../bad","target":"ok"}]})json");
        bool unsafe_rejected = false;
        try { (void)load_profile(root / "bad.json"); }
        catch (const std::exception&) { unsafe_rejected = true; }
        require(unsafe_rejected, "Unsafe profile path accepted");
        write_text(root / "duplicate-unicode-id.json", R"json({"name":"Duplicate IDs","rules":[
            {"id":"RÜLE","name":"First","source":"first.txt","target":"first.txt"},
            {"id":"rüle","name":"Second","source":"second.txt","target":"second.txt"}
        ]})json");
        bool duplicate_unicode_id_rejected = false;
        try { (void)load_profile(root / "duplicate-unicode-id.json"); }
        catch (const std::exception&) { duplicate_unicode_id_rejected = true; }
        require(duplicate_unicode_id_rejected,
                "Custom profile accepted Unicode-case duplicate rule IDs");
        write_text(root / "blank-profile-name.json",
            R"json({"name":"\u3000","rules":[{"id":"one","name":"One","source":"a","target":"b"}]})json");
        bool blank_profile_name_rejected = false;
        try { (void)load_profile(root / "blank-profile-name.json"); }
        catch (const std::exception&) { blank_profile_name_rejected = true; }
        require(blank_profile_name_rejected,
                "Custom profile accepted a whitespace-only Unicode name");
        const std::array invalid_profile_types{
            R"({"schemaVersion":1.5,"rules":[{"id":"one","name":"One","source":"a","target":"b"}]})",
            R"({"schemaVersion":1e2,"rules":[{"id":"one","name":"One","source":"a","target":"b"}]})",
            R"({"schemaVersion":2147483648,"rules":[{"id":"one","name":"One","source":"a","target":"b"}]})",
            R"({"schemaVersion":null,"rules":[{"id":"one","name":"One","source":"a","target":"b"}]})",
            R"({"name":null,"rules":[{"id":"one","name":"One","source":"a","target":"b"}]})",
            R"({"rules":[{"id":"one","name":"One","kind":null,"source":"a","target":"b"}]})",
            R"({"rules":[{"id":"one","name":"One","source":"a","target":"b","risk":null}]})",
            R"({"rules":[{"id":"one","name":"One","source":"a","target":"b","recursive":null}]})",
            R"({"rules":[{"id":"one","name":"One","source":"a","target":"b","defaultSelected":null}]})"
        };
        for (std::size_t index = 0; index < invalid_profile_types.size(); ++index) {
            const auto path = root / ("invalid-profile-type-" + std::to_string(index) + ".json");
            write_text(path, invalid_profile_types[index]);
            bool rejected = false;
            try { (void)load_profile(path); }
            catch (const std::exception&) { rejected = true; }
            require(rejected, "A migration profile with an invalid field type was accepted");
        }

        const auto default_profile = load_default_profile(fs::current_path());
        const auto default_directory = root / "default-profile-order";
        write_text(default_directory / "profiles" / "vanilla.json",
            R"({"name":"Nested rules","rules":[{"id":"one","name":"One","source":"a","target":"b"}]})");
        write_text(default_directory / "vanilla.json",
            R"({"name":"Adjacent rules","rules":[{"id":"two","name":"Two","source":"a","target":"b"}]})");
        require(load_default_profile(default_directory).name == "Nested rules",
                "Nested default rules did not override adjacent rules");
        fs::remove(default_directory / "profiles" / "vanilla.json");
        require(load_default_profile(default_directory).name == "Adjacent rules",
                "Adjacent default rules were not loaded when nested rules were absent");
        write_text(default_directory / "profiles" / "vanilla.json", "{invalid JSON");
        bool malformed_default_rejected = false;
        try { (void)load_default_profile(default_directory); }
        catch (const std::exception&) { malformed_default_rejected = true; }
        require(malformed_default_rejected,
                "A malformed nested default profile silently fell back to another rule file");
        const auto special_source = root / "special-source";
        const auto special_target = root / "special-target";
        fs::create_directories(special_source);
        fs::create_directories(special_target);
        std::size_t special_rule_count = 0;
        for (const auto& rule : default_profile.rules) {
            if (!rule.id.starts_with("sempervirens_")) continue;
            ++special_rule_count;
            const auto path = special_source / fs::path(utf8_to_wide(rule.source));
            if (rule.kind == RuleKind::directory)
                write_text(path / "sample.data", rule.id);
            else
                write_text(path, rule.id);
        }
        require(special_rule_count > 0, "Default profile has no community-specific rules");
        const auto special_plan = build_plan(default_profile, special_source, special_target);
        std::size_t found_special_rules = 0;
        for (const auto& operation : special_plan.operations)
            if (operation.id.starts_with("sempervirens_")) {
                ++found_special_rules;
                require(operation.status == ScanStatus::found,
                        "A community-specific rule was not found");
            }
        require(found_special_rules == special_rule_count,
                "Community-specific rule count differs from default profile");

        const auto minecraft = root / ".minecraft";
        write_text(minecraft / "versions" / L"1.21.4-模组" / "options.txt", "fov:90\n");
        write_text(minecraft / "versions" / L"1.21.4-模组" / "screenshots" / "one.PNG", "image");
        write_text(minecraft / "versions" / L"1.21.4-模组" / "saves" / "World" / "level.dat", "save");
        write_text(minecraft / "options.txt", "fov:70\n");
        const auto found = discover_instances(minecraft, L"默认 Minecraft 实例", L"当前文件夹");
        require(found.candidates.size() == 2, "Direct and version instances were not found");
        require(!found.candidates[0].version_isolated, "Direct instance was not first");
        require(found.candidates[1].version_isolated && found.candidates[1].screenshot_count == 1 &&
                found.candidates[1].world_count == 1, "Version instance counts were wrong");
        require(found.candidates[1].display_name == L"1.21.4-模组", "Unicode instance name was lost");
        const auto glob_source = root / "glob-safety-source";
        const auto glob_target = root / "glob-safety-target";
        write_text(glob_source / "mods" / "v1" / "file.dat", "safe wildcard directory");
        fs::create_directories(glob_target);
        MigrationProfile glob_profile;
        glob_profile.rules = {{"glob", "test", "Wildcard directory", "", RuleKind::glob,
            std::nullopt, "", "mods/v*/file.dat", "copied", true, true}};
        const auto glob_plan = build_plan(glob_profile, glob_source, glob_target);
        const auto expected_glob_target = glob_target / "copied" / "v1" / "file.dat";
        require(glob_plan.valid() && glob_plan.operations.size() == 1 &&
                glob_plan.operations[0].target_path == expected_glob_target &&
                is_within(glob_target, expected_glob_target, true),
                "Wildcard directory did not preserve a safe relative destination");
        const auto glob_result = execute_migration(glob_plan, OverwriteStrategy::replace);
        std::ifstream glob_output(expected_glob_target, std::ios::binary);
        const std::string glob_contents(std::istreambuf_iterator<char>{glob_output}, {});
        require(glob_result.failed_count == 0 && glob_result.files_copied == 1 &&
                glob_contents == "safe wildcard directory",
                "Wildcard directory migration wrote outside its intended relative path");
        for (int index = 0; index < 63; ++index)
            write_text(minecraft / "versions" / L"1.21.4-模组" / "screenshots" /
                       ("extra-" + std::to_string(index) + ".png"), "image");
        std::vector<std::wstring> english_discovery_progress;
        const auto english_selection = discover_instances(minecraft, L"Default Minecraft instance",
            L"Current folder", {}, [&](const ScanProgress& value) {
                english_discovery_progress.push_back(value.message);
            }, true);
        require(english_selection.candidates.size() == 2 &&
                english_selection.candidates[1].screenshot_count == 64 &&
                !english_discovery_progress.empty() &&
                english_discovery_progress.front() == L"Finding instances" &&
                std::any_of(english_discovery_progress.begin(), english_discovery_progress.end(),
                    [](const std::wstring& value) {
                        return value.starts_with(L"Checking instance: ");
                    }) &&
                std::any_of(english_discovery_progress.begin(), english_discovery_progress.end(),
                    [](const std::wstring& value) {
                        return value.find(L" files checked") != std::wstring::npos;
                    }), "English instance discovery progress was not localized");

        const auto scan_source = root / "scan-source";
        const auto scan_target = root / "scan-target";
        write_text(scan_source / "options.txt", "key_key.forward:key.keyboard.w\nfov:90\n");
        write_text(scan_target / "options.txt", "key_key.forward:key.keyboard.up\nfov:70\n");
        write_text(scan_source / "config" / "same.txt", "same");
        write_text(scan_target / "config" / "same.txt", "same");
        write_text(scan_source / "config" / "changed.txt", "new");
        write_text(scan_target / "config" / "changed.txt", "old");
        write_text(scan_source / "config" / "added.txt", "added");
        write_text(scan_source / "config" / "details.json", R"json({
            // A supported configuration can explain individual changes.
            "enabled": true,
            "hotkeys": {"openMenu": "key.keyboard.g"},
            "newOption": null,
        })json");
        write_text(scan_target / "config" / "details.json", R"json({
            "enabled": false,
            "hotkeys": {"openMenu": "key.keyboard.h"}
        })json");
        const auto config_changes = compare_configuration_files(
            scan_source / "config" / "details.json", scan_target / "config" / "details.json");
        require(config_changes && config_changes->size() == 3,
                "Configuration fields were not compared");
        require((*config_changes)[0].key == "enabled" &&
                (*config_changes)[0].target_value == "False" &&
                (*config_changes)[0].source_value == "True",
                "Boolean configuration difference was lost");
        require((*config_changes)[2].key == "newOption" &&
                !(*config_changes)[2].target_value &&
                (*config_changes)[2].source_value == std::string(1, '\0'),
                "Missing and null configuration values were confused");
        const auto utf16_config = [](std::string_view ascii) {
            std::string result("\xFF\xFE", 2);
            for (const char character : ascii) {
                result.push_back(character);
                result.push_back('\0');
            }
            return result;
        };
        const auto encoded_source = root / "encoded-config-source";
        const auto encoded_target = root / "encoded-config-target";
        write_text(encoded_source / "utf16.json", utf16_config("{\"enabled\":true}"));
        write_text(encoded_target / "utf16.json", utf16_config("{\"enabled\":false}"));
        const auto utf16_changes = compare_configuration_files(
            encoded_source / "utf16.json", encoded_target / "utf16.json");
        require(utf16_changes && utf16_changes->size() == 1 &&
                (*utf16_changes)[0].key == "enabled" &&
                (*utf16_changes)[0].target_value == "False" &&
                (*utf16_changes)[0].source_value == "True",
                "UTF-16 configuration fields were not decoded");
        write_text(encoded_source / "unicode.cfg", "Äction=source\n");
        write_text(encoded_target / "unicode.cfg", "äction=target\n");
        const auto unicode_changes = compare_configuration_files(
            encoded_source / "unicode.cfg", encoded_target / "unicode.cfg");
        require(unicode_changes && unicode_changes->size() == 1 &&
                (*unicode_changes)[0].key == "Äction" &&
                (*unicode_changes)[0].target_value == "target" &&
                (*unicode_changes)[0].source_value == "source",
                "Unicode configuration field casing was not matched");
        write_text(encoded_source / "numbers.json",
            R"({"exponent":1e3,"negativeZero":-0,"huge":1e999999})");
        write_text(encoded_target / "numbers.json",
            R"({"exponent":1000,"negativeZero":0,"huge":1e999998})");
        const auto number_changes = compare_configuration_files(
            encoded_source / "numbers.json", encoded_target / "numbers.json");
        const auto has_number = [&](std::string_view key, std::string_view target,
                                    std::string_view source) {
            return number_changes && std::any_of(number_changes->begin(), number_changes->end(),
                [&](const SettingDifference& change) {
                    return change.key == key && change.target_value == target &&
                           change.source_value == source;
                });
        };
        require(number_changes && number_changes->size() == 3 &&
                has_number("exponent", "1000", "1e3") &&
                has_number("negativeZero", "0", "-0") &&
                has_number("huge", "1e999998", "1e999999"),
                "JSON number spelling was not preserved in configuration differences");
        write_text(encoded_source / "invalid-number.json", R"({"value":01})");
        write_text(encoded_target / "invalid-number.json", R"({"value":1})");
        require(!compare_configuration_files(encoded_source / "invalid-number.json",
                encoded_target / "invalid-number.json"),
                "Malformed JSON number was accepted as a configuration field");
        write_text(encoded_source / "duplicate-case.json", R"({"Name":"first","name":"source"})");
        write_text(encoded_target / "duplicate-case.json", R"({"Name":"target"})");
        const auto duplicate_case_changes = compare_configuration_files(
            encoded_source / "duplicate-case.json", encoded_target / "duplicate-case.json");
        require(duplicate_case_changes && duplicate_case_changes->size() == 1 &&
                (*duplicate_case_changes)[0].key == "Name" &&
                (*duplicate_case_changes)[0].target_value == "target" &&
                (*duplicate_case_changes)[0].source_value == "source",
                "JSON duplicate-case property did not retain its first display name and last value");
        require(!compare_configuration_files(scan_source / "config" / "changed.txt",
                scan_target / "config" / "changed.txt"),
                "Unsupported file format was parsed as settings");
        write_text(scan_source / "mods" / "one.jar", "jar");
        write_text(scan_source / "mods" / "alpha.jar", "jar-alpha");
        NbtTag world_root;
        world_root.type = NbtType::compound;
        NbtTag world_data;
        world_data.type = NbtType::compound;
        NbtTag world_name;
        world_name.type = NbtType::string;
        world_name.text = "山谷";
        world_data.set("LevelName", world_name);
        world_root.set("Data", world_data);
        fs::create_directories(scan_source / "saves" / fs::path(L"测试世界"));
        write_nbt_compound(scan_source / "saves" / fs::path(L"测试世界") / "level.dat", world_root);
        NbtTag source_servers = server_root;
        NbtTag second_server = server;
        NbtTag second_address = server_address;
        second_address.text = "another.example.org";
        second_server.set("ip", second_address);
        source_servers.get("servers")->items.push_back(second_server);
        write_nbt_compound(scan_source / "servers.dat", source_servers);
        NbtTag target_servers = server_root;
        NbtTag renamed = server_name;
        renamed.text = "我的服务器";
        target_servers.get("servers")->items[0].set("name", renamed);
        write_nbt_compound(scan_target / "servers.dat", target_servers);
        MigrationProfile scan_profile;
        scan_profile.rules = {
            {"keys", "settings", "Keys", "", RuleKind::builtin, BuiltinKind::options,
                "bindings", "options.txt", "options.txt", true, true},
            {"config", "settings", "Config", "", RuleKind::directory, std::nullopt,
                "", "config", "config", true, true},
            {"mods", "mods", "Mods", "", RuleKind::glob, std::nullopt,
                "", "mods/a*.jar", "mods", true, true},
            {"worlds", "saves", "Worlds", "", RuleKind::builtin, BuiltinKind::worlds,
                "", "saves", "saves", true, true},
            {"servers", "servers", "Servers", "", RuleKind::builtin, BuiltinKind::servers,
                "", "servers.dat", "servers.dat", true, true},
        };
        int last_progress = -1;
        bool saw_indeterminate_scan = false;
        std::vector<int> scan_progress_values;
        const auto plan = build_plan(scan_profile, scan_source, scan_target, {},
            [&](const ScanProgress& progress) {
                if (progress.percent) {
                    last_progress = *progress.percent;
                    scan_progress_values.push_back(*progress.percent);
                } else saw_indeterminate_scan = true;
            });
        require(plan.valid() && plan.operations.size() == 6, "Scan plan was not built");
        require(plan.operations[0].status == ScanStatus::conflict, "Option conflict was missed");
        const auto option_explanation = explain_migration(plan.operations[0], scan_profile,
            OverwriteStrategy::backup_and_replace, false);
        require(option_explanation.difference.find(L"前进：上方向键 → W") != std::wstring::npos,
                "Binding difference did not show the action and both keys");
        require(explain_migration(plan.operations[0], scan_profile,
                    OverwriteStrategy::backup_and_replace, true).difference.find(
                    L"Move forward: Up arrow → W") != std::wstring::npos,
                "English binding difference retained Chinese punctuation or lost its key names");
        require(option_explanation.scope.find(L"其他选项保持原样") != std::wstring::npos,
                "Option migration scope is unclear");
        require(plan.operations[1].status == ScanStatus::conflict, "Directory conflict was missed");
        MigrationProfile config_profile;
        config_profile.rules = {{"details", "settings", "Details", "", RuleKind::file,
            std::nullopt, "", "config/details.json", "config/details.json", true, true}};
        const auto config_plan = build_plan(config_profile, scan_source, scan_target, {});
        require(config_plan.operations.size() == 1 &&
                std::holds_alternative<FileComparisonInfo>(config_plan.operations[0].comparison),
                "Scan did not retain configuration field differences");
        const auto config_explanation = explain_migration(config_plan.operations[0], config_profile,
            OverwriteStrategy::backup_and_replace, false);
        require(config_explanation.outcome.find(L"不逐项合并") != std::wstring::npos &&
                explain_migration(config_plan.operations[0], config_profile,
                    OverwriteStrategy::backup_and_replace, true).outcome.find(L"not merged") != std::wstring::npos,
                "Compact explanation lost the whole-file replacement warning");
        require(config_explanation.difference.find(L"是否启用: 关闭 → 开启") != std::wstring::npos,
                "Configuration explanation did not describe the changed value");
        require(plan.operations[2].status == ScanStatus::found, "Globbed mod was missed");
        require(plan.operations[2].target_path.filename() == L"alpha.jar", "Glob target name was truncated");
        require(plan.operations[3].status == ScanStatus::found, "World was missed");
        require(plan.operations[3].name == "Worlds", "World rule name was lost");
        require(std::get<WorldEntry>(plan.operations[3].payload).world_name == L"山谷",
                "World LevelName was not read");
        require(display_operation_name(plan.operations[3], false).find(L"山谷  ·  测试世界") !=
                    std::wstring::npos,
                "World list did not show its game name and folder name");
        require(plan.operations[4].status == ScanStatus::conflict, "Server conflict was missed");
        require(display_operation_name(plan.operations[4], false) == L"测试服务器",
                "Server list did not display the selected server name");
        const auto server_explanation = explain_migration(plan.operations[4], scan_profile,
            OverwriteStrategy::backup_and_replace, false);
        require(server_explanation.difference.find(L"迁移后保留目标名称") != std::wstring::npos,
                "Server name retention was not explained");
        require(std::get<ServerComparisonInfo>(plan.operations[4].comparison).target_name == "我的服务器",
                "Server name comparison was lost");
        require(plan.operations[5].status == ScanStatus::conflict,
                "New server status differed from the original list-file conflict behavior");
        require(last_progress == 100, "Scan progress did not finish");
        require(saw_indeterminate_scan &&
                std::find(scan_progress_values.begin(), scan_progress_values.end(), 25) !=
                    scan_progress_values.end() &&
                std::is_sorted(scan_progress_values.begin(), scan_progress_values.end()),
                "Scan did not show indeterminate enumeration and monotonic per-file progress");
        std::vector<std::wstring> english_scan_progress;
        const auto english_plan = build_plan(scan_profile, scan_source, scan_target, {},
            [&](const ScanProgress& value) { english_scan_progress.push_back(value.message); }, true);
        require(english_plan.operations.size() == plan.operations.size() &&
                !english_scan_progress.empty() &&
                english_scan_progress.front() == L"Scanning data" &&
                english_scan_progress.back() == L"Scan complete" &&
                std::any_of(english_scan_progress.begin(), english_scan_progress.end(),
                    [](const std::wstring& value) {
                        return value.find(L" files checked") != std::wstring::npos;
                    }) &&
                std::any_of(english_scan_progress.begin(), english_scan_progress.end(),
                    [](const std::wstring& value) {
                        return value.find(L" worlds checked") != std::wstring::npos;
                    }), "English scan progress was not localized");
        const auto skip_target = root / "scan-target-skip";
        const auto replace_target = root / "scan-target-replace";
        fs::copy(scan_target, skip_target, fs::copy_options::recursive);
        fs::copy(scan_target, replace_target, fs::copy_options::recursive);
        const auto skip_plan = build_plan(scan_profile, scan_source, skip_target);
        const auto replace_plan = build_plan(scan_profile, scan_source, replace_target);
        std::vector<std::wstring> english_migration_progress;
        const auto migrated = execute_migration(plan, OverwriteStrategy::backup_and_replace, {},
            [&](const ScanProgress& value) { english_migration_progress.push_back(value.message); }, true);
        require(migrated.failed_count == 0 && migrated.files_copied >= 5,
                "Backup-and-replace migration did not complete");
        require(!english_migration_progress.empty() &&
                english_migration_progress.front().starts_with(L"Processing: ") &&
                english_migration_progress.back() == L"Migration complete. Writing the record…",
                "English migration progress was not localized");
        require(migrated.backup_root && fs::exists(*migrated.backup_root / "options.txt") &&
                fs::exists(*migrated.backup_root / "config" / "changed.txt") &&
                fs::exists(*migrated.backup_root / "servers.dat"),
                "Original destination files were not backed up");
        require(fs::exists(migrated.log_path), "Migration record was not written");
        std::ifstream migrated_record_file(migrated.log_path, std::ios::binary);
        const std::string migrated_record(std::istreambuf_iterator<char>{migrated_record_file}, {});
        require(migrated_record.starts_with("\xEF\xBB\xBF" "Sempervirens migration record\r\n") &&
                migrated_record.find("Created: ") != std::string::npos &&
                migrated_record.find("Item summary: ") != std::string::npos &&
                migrated_record.find("File summary: ") != std::string::npos &&
                migrated_record.find("\r\nItems:\r\n") != std::string::npos,
                "Migration record format differs from the original application");
        const auto migrated_options = [&] {
            std::ifstream input(scan_target / "options.txt");
            return std::string(std::istreambuf_iterator<char>(input), {});
        }();
        require(migrated_options.find("key_key.forward:key.keyboard.w") != std::string::npos &&
                migrated_options.find("fov:70") != std::string::npos,
                "Selected options did not merge while unrelated settings stayed intact");
        const auto merged_servers = read_nbt_compound(scan_target / "servers.dat");
        const auto find_server = [](const NbtTag& document, const std::string& address) -> const NbtTag* {
            const auto* list = document.get("servers");
            if (!list) return nullptr;
            for (const auto& entry : list->items) if (entry.get_string("ip") == address) return &entry;
            return nullptr;
        };
        const auto* preserved_server = find_server(merged_servers, "play.example.org");
        require(merged_servers.get("servers") && merged_servers.get("servers")->items.size() == 2 &&
                preserved_server && preserved_server->get_string("name") == "我的服务器" &&
                preserved_server->get("unknownIcon"),
                "Server merge lost destination name or unknown fields");
        require(merged_servers.get("servers")->items[0].get_string("ip") == "play.example.org" &&
                merged_servers.get("servers")->items[1].get_string("ip") == "another.example.org" &&
                merged_servers.get("unknownRootField") &&
                merged_servers.get("unknownRootField")->long_integers == marker.long_integers,
                "Server merge changed entry order or discarded unknown root data");
        require(fs::exists(scan_target / "mods" / "alpha.jar") &&
                fs::exists(scan_target / "saves" / fs::path(L"测试世界") / "level.dat"),
                "Mod or world files were not copied");

        const auto skipped = execute_migration(skip_plan, OverwriteStrategy::skip);
        require(skipped.failed_count == 0 && skipped.skipped_count > 0 && !skipped.backup_root,
                "Skip strategy did not preserve destination conflicts");
        std::ifstream skipped_record_file(skipped.log_path, std::ios::binary);
        const std::string skipped_record(std::istreambuf_iterator<char>{skipped_record_file}, {});
        require(skipped_record.find("Backup folder: No backup was created\r\n") != std::string::npos,
                "Migration record did not explain that no backup was created");
        const auto skip_options = [&] {
            std::ifstream input(skip_target / "options.txt");
            return std::string(std::istreambuf_iterator<char>(input), {});
        }();
        require(skip_options.find("key_key.forward:key.keyboard.up") != std::string::npos,
                "Skip strategy changed destination bindings");
        require(read_nbt_compound(skip_target / "servers.dat").get("servers")->items.size() == 2,
                "Skip strategy did not append the new server");

        const auto replaced = execute_migration(replace_plan, OverwriteStrategy::replace);
        const auto replaced_servers = read_nbt_compound(replace_target / "servers.dat");
        const auto* replaced_preserved = find_server(replaced_servers, "play.example.org");
        require(replaced.failed_count == 0 && !replaced.backup_root &&
                replaced_preserved && replaced_preserved->get_string("name") == "我的服务器",
                "Replace strategy lost destination server name or created a backup");
        std::stop_source cancellation;
        cancellation.request_stop();
        bool cancelled = false;
        try { (void)execute_migration(replace_plan, OverwriteStrategy::replace, cancellation.get_token()); }
        catch (const std::exception&) { cancelled = true; }
        require(cancelled, "A cancelled migration was executed");
        const auto expect_overlap_rejected = [&](const fs::path& source, const fs::path& target,
                                                  bool english, const std::string& expected) {
            try {
                (void)build_plan(scan_profile, source, target, {}, {}, english);
            } catch (const std::exception& error) {
                require(error.what() == expected, "Overlapping path message was not localized");
                return;
            }
            throw std::runtime_error("Overlapping source and target were accepted");
        };
        expect_overlap_rejected(scan_source, scan_source, false,
                                "迁出和迁入实例不能相同，也不能互相包含。");
        expect_overlap_rejected(scan_source, scan_source / "config", false,
                                "迁出和迁入实例不能相同，也不能互相包含。");
        expect_overlap_rejected(scan_source / "config", scan_source, true,
            "The source and destination cannot be the same folder or contain one another.");

        auto changed_path_plan = replace_plan;
        changed_path_plan.target_root = changed_path_plan.source_root;
        bool changed_path_rejected = false;
        try {
            (void)execute_migration(changed_path_plan, OverwriteStrategy::replace,
                                    {}, {}, false);
        } catch (const std::exception& error) {
            changed_path_rejected = true;
            require(std::string(error.what()) ==
                        "扫描后迁出或迁入实例发生了变化，请重新扫描后再迁移。",
                    "Changed-instance message was not localized");
        }
        require(changed_path_rejected, "Changed instance paths were accepted during migration");

        bool missing_instance_rejected = false;
        try {
            (void)build_plan(scan_profile, root / "missing-instance", scan_target,
                             {}, {}, false);
        } catch (const std::exception& error) {
            missing_instance_rejected = true;
            require(std::string(error.what()) == "迁出和迁入实例必须都是现有文件夹。",
                    "Missing-instance message was not localized");
        }
        require(missing_instance_rejected, "A missing instance folder was accepted");

        const auto comparison_source = root / "comparison-source";
        const auto comparison_target = root / "comparison-target";
        write_text(comparison_source / "settings.json",
            R"({"hotkeys":{"openMenu":"K"},"enabled":true,"nullable":null})");
        write_text(comparison_target / "settings.json",
            R"({"hotkeys":{"openMenu":"M"},"enabled":false,"nullable":"null"})");
        write_text(comparison_source / "tweaks.cfg", "dragMode=true\n");
        write_text(comparison_target / "tweaks.cfg", "dragMode=false\n");
        write_text(comparison_source / "data.dat", "source");
        write_text(comparison_target / "data.dat", "target");
        write_text(comparison_source / "screenshots" / "same.png", "source image");
        write_text(comparison_target / "screenshots" / "same.png", "target image");
        fs::create_directories(comparison_source / "saves" / "Example");
        fs::create_directories(comparison_target / "saves" / "Example");
        write_nbt_compound(comparison_source / "saves" / "Example" / "level.dat", world_root);
        write_nbt_compound(comparison_target / "saves" / "Example" / "level.dat", world_root);
        write_text(comparison_target / "saves" / "Example" / "notes.txt", "destination only");
        MigrationProfile comparison_profile;
        comparison_profile.name = "Comparison test";
        comparison_profile.rules = {
            {"json", "test", "JSON", "保存菜单与常用设置。", RuleKind::file, std::nullopt,
                "", "settings.json", "settings.json"},
            {"cfg", "test", "CFG", "", RuleKind::file, std::nullopt,
                "", "tweaks.cfg", "tweaks.cfg"},
            {"binary", "test", "Binary", "", RuleKind::file, std::nullopt,
                "", "data.dat", "data.dat"},
            {"folder", "test", "Images", "保存游戏内截图。", RuleKind::directory, std::nullopt,
                "", "screenshots", "screenshots"},
            {"worlds", "test", "Worlds", "", RuleKind::builtin, BuiltinKind::worlds,
                "", "saves", "saves"}
        };
        const auto comparisons = build_plan(comparison_profile, comparison_source, comparison_target);
        require(comparisons.operations.size() == 5, "Comparison fixture produced the wrong number of items");
        const auto& json_comparison = comparisons.operations[0];
        const auto* json_fields = std::get_if<FileComparisonInfo>(&json_comparison.comparison);
        require(json_fields && json_fields->settings && json_fields->settings->size() == 3,
                "JSON field comparison did not match the original self-test");
        require(explain_migration(json_comparison, comparison_profile,
                OverwriteStrategy::backup_and_replace, false).difference.find(L"M → K") != std::wstring::npos,
                "JSON hotkey difference was not explained");
        const auto described_file = explain_migration(json_comparison, comparison_profile,
            OverwriteStrategy::backup_and_replace, false);
        require(described_file.scope.starts_with(L"保存菜单与常用设置。\n复制整个文件；"),
                "Chinese file explanation omitted the migration profile description");
        require(described_file.difference.find(L"• 快捷键 / 打开菜单: M → K") != std::wstring::npos,
                "Nested configuration key was not explained by purpose and hierarchy");
        const auto english_file = explain_migration(json_comparison, comparison_profile,
            OverwriteStrategy::backup_and_replace, true);
        require(english_file.difference.find(L"• Hotkeys / Open menu: M → K") != std::wstring::npos,
                "English nested configuration key lost its readable hierarchy");
        require(english_file.scope.starts_with(L"This file is copied as a whole") &&
                english_file.scope.find(L"保存菜单") == std::wstring::npos,
                "English file scope differs from the original description policy");
        require(explain_migration(comparisons.operations[1], comparison_profile,
                OverwriteStrategy::backup_and_replace, false).difference.find(L"关闭 → 开启") != std::wstring::npos,
                "CFG switch difference was not explained");
        const auto* binary_comparison = std::get_if<FileComparisonInfo>(&comparisons.operations[2].comparison);
        require(binary_comparison && !binary_comparison->settings,
                "Unsupported binary content was incorrectly parsed as settings");
        require(explain_migration(comparisons.operations[3], comparison_profile,
                OverwriteStrategy::backup_and_replace, false).difference.find(L"same.png") != std::wstring::npos,
                "Changed screenshot was omitted from directory explanation");
        require(explain_migration(comparisons.operations[3], comparison_profile,
                OverwriteStrategy::backup_and_replace, false).scope.starts_with(
                    L"保存游戏内截图。\n复制这个文件夹中的文件；"),
                "Chinese directory explanation omitted the migration profile description");
        require(comparisons.operations[4].status == ScanStatus::conflict &&
                explain_migration(comparisons.operations[4], comparison_profile,
                OverwriteStrategy::backup_and_replace, false).difference.find(L"notes.txt") != std::wstring::npos,
                "Destination-only world content was not explained");
        MigrationProfile unknown_option_profile;
        unknown_option_profile.rules = {{"other-options", "test", "Other options", "",
            RuleKind::builtin, BuiltinKind::options, "other", "options.txt", "options.txt"}};
        PlannedOperation unknown_option;
        unknown_option.rule_index = 0;
        unknown_option.status = ScanStatus::found;
        unknown_option.is_builtin = true;
        unknown_option.source_path = comparison_source / "options.txt";
        unknown_option.target_path = comparison_target / "options.txt";
        require(explain_migration(unknown_option, unknown_option_profile,
                OverwriteStrategy::replace, false).scope == L"只合并所选的这一组设置。" &&
                explain_migration(unknown_option, unknown_option_profile,
                OverwriteStrategy::replace, true).scope ==
                    L"Only the selected group of settings is merged.",
                "Unknown options category lacked its generic migration scope");

        const auto option_source = root / "option-source";
        const auto option_target = root / "option-target";
        write_text(option_source / "options.txt", "key_key.forward:key.keyboard.w\n"
            "soundCategory_master:0.5\nsoundCategory_friendly:0.7\nfov:90\nparticles:2\n");
        write_text(option_target / "options.txt", "key_key.forward:key.keyboard.w\n"
            "soundCategory_master:0.2\nsoundCategory_friendly:0.4\nfov:70\nparticles:0\n");
        MigrationProfile option_profile;
        option_profile.name = "Category test";
        option_profile.rules = {
            {"bindings", "settings", "Bindings", "", RuleKind::builtin, BuiltinKind::options,
                "bindings", "options.txt", "options.txt", true, true},
            {"audio", "settings", "Audio", "", RuleKind::builtin, BuiltinKind::options,
                "audio", "options.txt", "options.txt", true, true},
            {"video", "settings", "Video", "", RuleKind::builtin, BuiltinKind::options,
                "video", "options.txt", "options.txt", true, false}
        };
        const auto option_plan = build_plan(option_profile, option_source, option_target);
        require(option_plan.operations.size() == 3 &&
                option_plan.operations[0].status == ScanStatus::identical &&
                option_plan.operations[1].status == ScanStatus::conflict &&
                option_plan.operations[2].status == ScanStatus::conflict,
                "Independent option categories were not scanned correctly");
        const auto audio_description = explain_migration(option_plan.operations[1], option_profile,
            OverwriteStrategy::backup_and_replace, false).difference;
        const auto english_audio_description = explain_migration(option_plan.operations[1], option_profile,
            OverwriteStrategy::backup_and_replace, true).difference;
        const auto video_description = explain_migration(option_plan.operations[2], option_profile,
            OverwriteStrategy::backup_and_replace, false).difference;
        require(audio_description.find(L"20% → 50%") != std::wstring::npos &&
                audio_description.find(L"友好生物音量：40% → 70%") != std::wstring::npos &&
                english_audio_description.find(L"Friendly creatures: 40% → 70%") != std::wstring::npos &&
                video_description.find(L"全部 → 最少") != std::wstring::npos,
                "Audio or video difference labels diverged from the original");
        require(display_binding_value("30", false) == L"按键码 30" &&
                display_binding_value("30", true) == L"Key code 30",
                "Legacy numeric key binding exposed an unexplained raw value");
        const auto option_result = execute_migration(option_plan, OverwriteStrategy::backup_and_replace);
        std::ifstream merged_option_file(option_target / "options.txt", std::ios::binary);
        const std::string option_contents(std::istreambuf_iterator<char>{merged_option_file}, {});
        require(option_result.failed_count == 0 && option_result.backup_root &&
                fs::exists(*option_result.backup_root / "options.txt") &&
                option_contents.find("soundCategory_master:0.5") != std::string::npos &&
                option_contents.find("fov:70") != std::string::npos &&
                option_contents.find("particles:0") != std::string::npos,
                "Selected audio settings changed unselected video settings or missed backup");
        const auto duplicate_option_target = root / "duplicate-option-target";
        const std::string duplicate_option_contents =
            "key_key.forward:key.keyboard.up\nfov:70\nFOV:90\n";
        write_text(duplicate_option_target / "options.txt", duplicate_option_contents);
        const auto duplicate_option_plan = build_plan(option_profile,
            option_source, duplicate_option_target);
        require(duplicate_option_plan.operations.size() == 3 &&
                duplicate_option_plan.operations[0].status == ScanStatus::conflict,
                "Duplicate target option keys prevented an otherwise valid scan");
        const auto duplicate_option_result = execute_migration(duplicate_option_plan,
            OverwriteStrategy::replace);
        std::ifstream duplicate_option_file(duplicate_option_target / "options.txt", std::ios::binary);
        const std::string duplicate_option_after(std::istreambuf_iterator<char>{duplicate_option_file}, {});
        require(duplicate_option_result.failed_count == 1 &&
                duplicate_option_result.files_failed == 1 &&
                duplicate_option_after == duplicate_option_contents,
                "Duplicate target option keys were silently merged or modified");
        const auto unicode_option_source = root / "unicode-option-source";
        const auto unicode_option_target = root / "unicode-option-target";
        write_text(unicode_option_source / "options.txt", "key_mod.BÜCHER:key.keyboard.w\n");
        write_text(unicode_option_target / "options.txt", "key_mod.bücher:key.keyboard.w\n");
        MigrationProfile unicode_option_profile;
        unicode_option_profile.name = "Unicode option key";
        unicode_option_profile.rules = {option_profile.rules[0]};
        const auto matching_unicode_option = build_plan(unicode_option_profile,
            unicode_option_source, unicode_option_target);
        require(matching_unicode_option.operations.size() == 1 &&
                matching_unicode_option.operations[0].status == ScanStatus::identical,
                "Unicode-case option keys were treated as separate settings");
        write_text(unicode_option_source / "options.txt", "key_mod.BÜCHER:key.keyboard.x\n");
        const auto changed_unicode_option = build_plan(unicode_option_profile,
            unicode_option_source, unicode_option_target);
        require(changed_unicode_option.operations.size() == 1 &&
                changed_unicode_option.operations[0].status == ScanStatus::conflict,
                "Changed Unicode-case option key was not identified as a conflict");
        const auto unicode_option_result = execute_migration(changed_unicode_option,
            OverwriteStrategy::replace);
        std::ifstream unicode_option_file(unicode_option_target / "options.txt", std::ios::binary);
        const std::string unicode_option_after(std::istreambuf_iterator<char>{unicode_option_file}, {});
        require(unicode_option_result.failed_count == 0 &&
                unicode_option_after == "key_mod.BÜCHER:key.keyboard.x\r\n",
                "Unicode-case option key was duplicated instead of replaced");

        std::error_code ignored;
        fs::remove_all(root, ignored);
        std::cout << "C++ core tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "C++ core test failed: " << error.what() << '\n';
        std::error_code ignored;
        fs::remove_all(root, ignored);
        return 1;
    }
}
