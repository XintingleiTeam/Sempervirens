#include "execution.hpp"
#include "nbt.hpp"
#include "profile.hpp"
#include "scan.hpp"
#include "text.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>
#include <iostream>
#include <limits>

using namespace sempervirens;

namespace {
NbtTag string_tag(std::string text) {
    NbtTag value;
    value.type = NbtType::string;
    value.text = std::move(text);
    return value;
}

NbtTag server_tag(std::string name, std::string address, std::vector<std::uint8_t> icon) {
    NbtTag server;
    server.type = NbtType::compound;
    server.set("name", string_tag(std::move(name)));
    server.set("ip", string_tag(std::move(address)));
    NbtTag unknown;
    unknown.type = NbtType::byte_array;
    unknown.bytes = std::move(icon);
    server.set("unknownIcon", std::move(unknown));
    return server;
}

NbtTag server_document(bool source) {
    NbtTag root;
    root.type = NbtType::compound;
    root.set("unknownRootField", string_tag(source ? "source marker" : "target marker"));
    NbtTag list;
    list.type = NbtType::list;
    list.item_type = NbtType::compound;
    list.items.push_back(server_tag(source ? "Source name" : "Target name",
                                    "play.example.org", source ?
                                        std::vector<std::uint8_t>{1, 2, 3} :
                                        std::vector<std::uint8_t>{9, 8, 7}));
    if (source) list.items.push_back(server_tag("New server", "new.example.org", {4, 5}));
    list.items.push_back(server_tag("Same server", "same.example.org", {6}));
    root.set("servers", std::move(list));
    return root;
}

void write_invalid_utf8_server(const fs::path& path) {
    std::vector<std::uint8_t> bytes;
    const auto u16 = [&](std::uint16_t value) {
        bytes.push_back(static_cast<std::uint8_t>(value >> 8));
        bytes.push_back(static_cast<std::uint8_t>(value));
    };
    const auto u32 = [&](std::uint32_t value) {
        for (int shift = 24; shift >= 0; shift -= 8)
            bytes.push_back(static_cast<std::uint8_t>(value >> shift));
    };
    const auto text = [&](std::string_view value) {
        u16(static_cast<std::uint16_t>(value.size()));
        bytes.insert(bytes.end(), value.begin(), value.end());
    };
    bytes.push_back(static_cast<std::uint8_t>(NbtType::compound));
    text("");
    bytes.push_back(static_cast<std::uint8_t>(NbtType::list));
    text("servers");
    bytes.push_back(static_cast<std::uint8_t>(NbtType::compound));
    u32(1);
    bytes.push_back(static_cast<std::uint8_t>(NbtType::string));
    text("name");
    text(std::string_view("Bad\xFF", 4));
    bytes.push_back(static_cast<std::uint8_t>(NbtType::string));
    text("ip");
    text("invalid.example");
    bytes.push_back(static_cast<std::uint8_t>(NbtType::end));
    bytes.push_back(static_cast<std::uint8_t>(NbtType::end));
    fs::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary);
    output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!output) throw std::runtime_error("Could not write invalid UTF-8 NBT fixture");
}

nlohmann::json server_snapshot(const fs::path& path) {
    const auto root = read_nbt_compound(path);
    nlohmann::json servers = nlohmann::json::array();
    if (const auto* list = root.get("servers"))
        for (const auto& server : list->items) {
            const auto* icon = server.get("unknownIcon");
            servers.push_back({{"name", server.get_string("name")},
                               {"ip", server.get_string("ip")},
                               {"unknownIcon", icon ? icon->bytes : std::vector<std::uint8_t>{}}});
        }
    return {{"unknownRootField", root.get_string("unknownRootField")},
            {"servers", std::move(servers)}};
}
}

int wmain(int argc, wchar_t** argv) {
    if (argc == 4 && std::wstring_view(argv[1]) == L"--fixture-invalid-utf8-servers") {
        try {
            write_invalid_utf8_server(fs::path(argv[2]));
            write_invalid_utf8_server(fs::path(argv[3]));
            return 0;
        } catch (const std::exception& error) {
            std::cerr << "Invalid UTF-8 server fixture failed: " << error.what() << '\n';
            return 1;
        }
    }
    if (argc == 4 && std::wstring_view(argv[1]) == L"--fixture-nan-servers") {
        try {
            auto document = server_document(false);
            NbtTag unknown_float;
            unknown_float.type = NbtType::floating;
            unknown_float.decimal = std::numeric_limits<float>::quiet_NaN();
            document.get("servers")->items.front().set("unknownFloat", unknown_float);
            write_nbt_compound(fs::path(argv[2]), document);
            write_nbt_compound(fs::path(argv[3]), document);
            return 0;
        } catch (const std::exception& error) {
            std::cerr << "NaN server fixture failed: " << error.what() << '\n';
            return 1;
        }
    }
    if (argc == 3 && std::wstring_view(argv[1]) == L"--fixture-blank-server") {
        try {
            auto document = server_document(true);
            auto* list = document.get("servers");
            list->items.clear();
            list->items.push_back(server_tag("", "", {7}));
            write_nbt_compound(fs::path(argv[2]), document);
            return 0;
        } catch (const std::exception& error) {
            std::cerr << "Blank server fixture failed: " << error.what() << '\n';
            return 1;
        }
    }
    if (argc == 3 && std::wstring_view(argv[1]) == L"--fixture-duplicate-servers") {
        try {
            auto document = server_document(false);
            document.get("servers")->items.push_back(
                server_tag("Duplicate name", "PLAY.EXAMPLE.ORG", {3, 2, 1}));
            write_nbt_compound(fs::path(argv[2]), document);
            return 0;
        } catch (const std::exception& error) {
            std::cerr << "Duplicate server fixture failed: " << error.what() << '\n';
            return 1;
        }
    }
    if (argc == 4 && std::wstring_view(argv[1]) == L"--fixture-servers") {
        try {
            write_nbt_compound(fs::path(argv[2]) / "servers.dat", server_document(true));
            write_nbt_compound(fs::path(argv[3]) / "servers.dat", server_document(false));
            return 0;
        } catch (const std::exception& error) {
            std::cerr << "Server fixture failed: " << error.what() << '\n';
            return 1;
        }
    }
    if (argc == 3 && std::wstring_view(argv[1]) == L"--nbt-json") {
        try {
            std::cout << server_snapshot(fs::path(argv[2])).dump() << '\n';
            return 0;
        } catch (const std::exception& error) {
            std::cerr << "NBT inspection failed: " << error.what() << '\n';
            return 1;
        }
    }
    if (argc != 4 && argc != 5) {
        std::cerr << "Usage: parity-migrate [backup|replace|skip] <profile> <source> <destination>\n";
        return 2;
    }
    try {
        const auto offset = argc == 5 ? 1 : 0;
        auto strategy = OverwriteStrategy::backup_and_replace;
        if (offset) {
            const auto choice = std::wstring_view(argv[1]);
            if (choice == L"replace") strategy = OverwriteStrategy::replace;
            else if (choice == L"skip") strategy = OverwriteStrategy::skip;
            else if (choice != L"backup") throw std::runtime_error("Unknown conflict strategy");
        }
        const auto profile = load_profile(fs::path(argv[offset + 1]));
        const auto plan = build_plan(profile, fs::path(argv[offset + 2]), fs::path(argv[offset + 3]));
        const auto result = execute_migration(plan, strategy);
        nlohmann::json items = nlohmann::json::array();
        for (const auto& item : result.items) {
            auto target = wide_to_utf8(item.target_path.lexically_relative(result.target_root).wstring());
            std::replace(target.begin(), target.end(), '\\', '/');
            items.push_back({{"name", item.name}, {"target", target}, {"status", item.status},
                             {"copied", item.files_copied}, {"overwritten", item.files_overwritten},
                             {"backedUp", item.files_backed_up}});
        }
        std::cout << nlohmann::json{
            {"success", result.success_count}, {"skipped", result.skipped_count},
            {"failed", result.failed_count}, {"copied", result.files_copied},
            {"overwritten", result.files_overwritten}, {"backedUp", result.files_backed_up},
            {"filesFailed", result.files_failed}, {"items", std::move(items)}
        }.dump() << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Migration parity probe failed: " << error.what() << '\n';
        return 1;
    }
}
