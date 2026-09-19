#include "atomic_file.hpp"
#include "text.hpp"
#include "update.hpp"

#include <nlohmann/json.hpp>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>

#include <fstream>
#include <string>

namespace fs = std::filesystem;
using namespace sempervirens;

namespace {
fs::path module_path() {
    std::wstring value(MAX_PATH,L'\0');
    for (;;) {
        const auto count=GetModuleFileNameW(nullptr,value.data(),static_cast<DWORD>(value.size()));
        if (!count) throw std::runtime_error("Cannot locate updater");
        if (count<value.size()) { value.resize(count); return value; }
        value.resize(value.size()*2);
    }
}
std::vector<std::uint8_t> read_bytes(const fs::path& path,std::uint64_t limit) {
    const auto size=fs::file_size(path);
    if (size>limit) throw std::runtime_error("Update metadata is too large");
    std::vector<std::uint8_t> result(static_cast<std::size_t>(size));
    std::ifstream input(path,std::ios::binary);
    if (!input || (size&&!input.read(reinterpret_cast<char*>(result.data()),size)))
        throw std::runtime_error("Update metadata could not be read");
    return result;
}
std::wstring quote(const fs::path& value) { return L"\""+value.wstring()+L"\""; }
void launch(const fs::path& executable) {
    STARTUPINFOW startup{sizeof(startup)}; PROCESS_INFORMATION process{};
    auto command=quote(executable);
    if (!CreateProcessW(executable.c_str(),command.data(),nullptr,nullptr,FALSE,0,nullptr,
                        executable.parent_path().c_str(),&startup,&process))
        throw std::runtime_error("Launcher could not restart");
    CloseHandle(process.hThread); CloseHandle(process.hProcess);
}
} // namespace

int WINAPI wWinMain(HINSTANCE,HINSTANCE,PWSTR,int) {
    try {
        int count=0; auto** values=CommandLineToArgvW(GetCommandLineW(),&count);
        if (!values || count!=6 || std::wstring_view(values[1])!=L"--commit") return 2;
        const fs::path staging=values[2];
        const std::string version=wide_to_utf8(values[3]);
        const DWORD parent=std::stoul(values[4]);
        const std::string token=wide_to_utf8(values[5]);
        LocalFree(values);
        if (!update::safe_version(version) || token.size()<16 || token.size()>80) return 3;
        const auto allowed=fs::weakly_canonical(update::update_data_root()/L"staging");
        const auto source=fs::weakly_canonical(staging);
        const auto relative=source.lexically_relative(allowed);
        if (relative.empty() || relative.native().starts_with(L"..") || source.filename()!=utf8_to_wide(version)) return 4;
        if (HANDLE process=OpenProcess(SYNCHRONIZE,FALSE,parent)) {
            WaitForSingleObject(process,60000); CloseHandle(process);
        }
        const auto manifest_bytes=read_bytes(source/L"manifest.json",128*1024);
        const auto signature=read_bytes(source/L"manifest.json.sig",1024);
        const std::string_view manifest_text(reinterpret_cast<const char*>(manifest_bytes.data()),manifest_bytes.size());
        if (!update::verify_manifest_signature(manifest_text,signature)) throw std::runtime_error("Signature check failed");
        const auto manifest=update::parse_manifest(manifest_text,"stable");
        if (manifest.version!=version) throw std::runtime_error("Prepared version changed");
        const auto staged_app=source/L"SempervirensApp.exe";
        if (update::sha256_file(staged_app)!=manifest.output_sha256) throw std::runtime_error("Prepared app changed");
        const auto root=module_path().parent_path();
        const auto versions=root/L"versions";
        const auto incoming=versions/(L".incoming-"+utf8_to_wide(token));
        const auto target=versions/utf8_to_wide(version);
        fs::create_directories(incoming);
        fs::copy_file(staged_app,incoming/L"SempervirensApp.exe",fs::copy_options::overwrite_existing);
        if (update::sha256_file(incoming/L"SempervirensApp.exe")!=manifest.output_sha256)
            throw std::runtime_error("Installed update changed while copying");
        nlohmann::json old_state;
        { std::ifstream input(root/L"current.json",std::ios::binary); input>>old_state; }
        const auto previous=old_state.value("current","");
        if (!update::safe_version(previous)) throw std::runtime_error("Current version state is invalid");
        if (!fs::exists(target)) fs::rename(incoming,target);
        else if (fs::is_regular_file(target/L"SempervirensApp.exe") &&
                 update::sha256_file(target/L"SempervirensApp.exe")==manifest.output_sha256) {
            fs::remove(incoming/L"SempervirensApp.exe"); fs::remove(incoming);
        } else {
            if (version==previous) throw std::runtime_error("Refusing to replace the active version");
            fs::remove_all(target); fs::rename(incoming,target);
        }
        const nlohmann::json next{{"schema",1},{"current",version},{"previous",previous},
            {"pending",true},{"attempted",false},{"token",token}};
        write_file_atomically(root/L"current.json",next.dump(2));
        launch(root/L"Sempervirens.exe");
        return 0;
    } catch (...) {
        try { launch(module_path().parent_path()/L"Sempervirens.exe"); } catch (...) {}
        return 1;
    }
}
