#include "update.hpp"
#include <miniz.h>
#include <nlohmann/json.hpp>
#include <windows.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <vector>

namespace fs = std::filesystem;
using namespace sempervirens;

template <class T> void append_integer(std::vector<std::uint8_t>& out, T value) {
    for (std::size_t i=0;i<sizeof(T);++i) out.push_back(static_cast<std::uint8_t>(value>>(i*8)));
}
void write(const fs::path& path, const std::vector<std::uint8_t>& bytes) {
    std::ofstream output(path,std::ios::binary); output.write(reinterpret_cast<const char*>(bytes.data()),bytes.size());
    if (!output) throw std::runtime_error("fixture write failed");
}

std::vector<std::uint8_t> read(const fs::path& path) {
    std::ifstream input(path,std::ios::binary);
    return {std::istreambuf_iterator<char>(input),std::istreambuf_iterator<char>()};
}

int main(int argc,char** argv) try {
    if (argc==3) {
        const auto manifest=read(fs::path(argv[1])), signature=read(fs::path(argv[2]));
        const std::string_view text(reinterpret_cast<const char*>(manifest.data()),manifest.size());
        if (!update::verify_manifest_signature(text,signature)) return 20;
        const auto parsed=update::parse_manifest(text,"stable");
        return parsed.version=="0.1.0.0" ? 0 : 21;
    }
    if (update::compare_versions("0.1.0.0","0.1.0.1")>=0 ||
        update::compare_versions("1.0.0.0","0.9.9.9")<=0 ||
        !update::safe_version("0.1.0.0") || update::safe_version("../bad")) return 2;
    const std::vector<std::uint8_t> old{'o','l','d','-','p','a','y','l','o','a','d'};
    const std::vector<std::uint8_t> fresh{'n','e','w','-','p','a','y','l','o','a','d','-','0','1'};
    std::vector<std::uint8_t> stream{1}; append_integer<std::uint32_t>(stream,static_cast<std::uint32_t>(fresh.size()));
    stream.insert(stream.end(),fresh.begin(),fresh.end()); stream.push_back(255);
    std::vector<std::uint8_t> compressed(mz_compressBound(stream.size())); mz_ulong compressed_size=compressed.size();
    if (mz_compress2(compressed.data(),&compressed_size,stream.data(),stream.size(),MZ_BEST_COMPRESSION)!=MZ_OK) return 3;
    compressed.resize(compressed_size);
    const auto old_hash=update::sha256_bytes(old.data(),old.size());
    const auto new_hash=update::sha256_bytes(fresh.data(),fresh.size());
    std::vector<std::uint8_t> patch{'S','V','D','I','F','F','1',0};
    append_integer<std::uint64_t>(patch,old.size()); append_integer<std::uint64_t>(patch,fresh.size());
    patch.insert(patch.end(),old_hash.begin(),old_hash.end()); patch.insert(patch.end(),new_hash.begin(),new_hash.end());
    append_integer<std::uint64_t>(patch,stream.size()); append_integer<std::uint64_t>(patch,compressed.size());
    patch.insert(patch.end(),compressed.begin(),compressed.end());
    const auto root=fs::temp_directory_path()/(L"sempervirens-update-test-"+std::to_wstring(GetCurrentProcessId()));
    fs::create_directories(root); const auto old_path=root/L"old.exe", output=root/L"new.exe";
    write(old_path,old); update::apply_delta(old_path,patch,output,new_hash,fresh.size());
    if (update::sha256_file(output)!=new_hash) return 4;
    patch.back()^=0x55; bool rejected=false;
    try { update::apply_delta(old_path,patch,output,new_hash,fresh.size()); } catch (...) { rejected=true; }
    if (!rejected) return 5;
    const nlohmann::json manifest{{"schema",1},{"product","sempervirens-windows-x64"},{"channel","stable"},
        {"version","0.1.0.1"},{"serial",1},{"minimumUpdaterProtocol",1},{"publishedUtc","2026-09-19T00:00:00Z"},
        {"outputSha256",new_hash},{"outputSize",fresh.size()},{"packages",nlohmann::json::array({{
            {"kind","full"},{"urls",nlohmann::json::array({"https://github.com/XintingleiTeam/sempervirens/releases/download/v0.1.0.1/SempervirensApp.exe","https://api.xintinglei.cn/api/sempervirens/update/releases/0.1.0.1/SempervirensApp.exe"})},
            {"size",fresh.size()},{"sha256",new_hash}}})}};
    const auto parsed=update::parse_manifest(manifest.dump(),"stable");
    if (parsed.packages.size()!=1 || parsed.packages[0].urls.size()!=2) return 6;
    auto rejected_manifest=manifest;
    rejected_manifest["packages"][0]["urls"][1]="https://xintinglei.cn/api/sempervirens/update/releases/0.1.0.1/SempervirensApp.exe";
    bool rejected_old_domain=false;
    try { update::parse_manifest(rejected_manifest.dump(),"stable"); }
    catch (...) { rejected_old_domain=true; }
    if (!rejected_old_domain) return 7;
    std::error_code ignored; fs::remove_all(root,ignored);
    std::cout << "Internal update delta and manifest tests passed.\n"; return 0;
} catch (const std::exception& error) { std::cerr<<error.what()<<'\n'; return 1; }
