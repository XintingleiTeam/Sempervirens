#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace sempervirens::update {

namespace fs = std::filesystem;
inline constexpr std::string_view product_id = "sempervirens-windows-x64";
inline constexpr std::string_view current_version = "0.1.0.0";
inline constexpr unsigned updater_protocol = 1;

struct Package {
    std::string kind;
    std::string from_version;
    std::string from_sha256;
    std::vector<std::string> urls;
    std::uint64_t size = 0;
    std::string sha256;
};

struct ChannelSource {
    std::wstring manifest_url;
    std::wstring signature_url;
};

inline const std::array<ChannelSource, 2> stable_sources{{
    {L"https://github.com/XintingleiTeam/sempervirens/releases/latest/download/stable.json",
     L"https://github.com/XintingleiTeam/sempervirens/releases/latest/download/stable.json.sig"},
    {L"https://api.xintinglei.cn/api/sempervirens/update/stable.json",
     L"https://api.xintinglei.cn/api/sempervirens/update/stable.json.sig"}
}};

struct Manifest {
    unsigned schema = 0;
    std::string product;
    std::string channel;
    std::string version;
    std::uint64_t serial = 0;
    unsigned minimum_updater_protocol = 0;
    std::string published_utc;
    std::wstring notes_zh;
    std::wstring notes_en;
    std::string output_sha256;
    std::uint64_t output_size = 0;
    std::vector<Package> packages;
};

using Progress = std::function<void(std::uint64_t, std::uint64_t)>;

std::string sha256_bytes(const void* data, std::size_t size);
std::string sha256_file(const fs::path& path);
bool verify_manifest_signature(std::string_view bytes, const std::vector<std::uint8_t>& signature);
Manifest parse_manifest(std::string_view bytes, std::string_view expected_channel);
int compare_versions(std::string_view left, std::string_view right);
std::vector<std::uint8_t> download_https(std::wstring_view url, std::size_t limit,
                                         std::stop_token stop = {}, Progress progress = {});
void apply_delta(const fs::path& old_file, const std::vector<std::uint8_t>& patch,
                 const fs::path& output, std::string_view expected_output_sha256,
                 std::uint64_t expected_output_size, std::stop_token stop = {},
                 Progress progress = {});
void write_verified_file(const std::vector<std::uint8_t>& bytes, const fs::path& output,
                         std::string_view expected_sha256, std::uint64_t expected_size);
bool safe_version(std::string_view version);
fs::path installation_root(const fs::path& module_directory);
fs::path update_data_root();

} // namespace sempervirens::update
