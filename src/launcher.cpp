#include "atomic_file.hpp"
#include "text.hpp"
#include "update.hpp"

#include <nlohmann/json.hpp>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>

#include <chrono>
#include <fstream>
#include <random>
#include <string>
#include <thread>

namespace fs = std::filesystem;
using namespace sempervirens;

namespace {
fs::path module_path() {
    std::wstring value(MAX_PATH,L'\0');
    for (;;) {
        const auto count=GetModuleFileNameW(nullptr,value.data(),static_cast<DWORD>(value.size()));
        if (!count) throw std::runtime_error("Cannot locate launcher");
        if (count<value.size()) { value.resize(count); return value; }
        value.resize(value.size()*2);
    }
}

nlohmann::json read_state(const fs::path& path) {
    std::ifstream input(path,std::ios::binary);
    if (!input) throw std::runtime_error("Missing version state");
    auto state=nlohmann::json::parse(input);
    if (!state.is_object()) throw std::runtime_error("Invalid version state");
    return state;
}

void write_state(const fs::path& path,const nlohmann::json& state) {
    write_file_atomically(path,state.dump(2));
}

bool valid_component(const std::string& value) { return update::safe_version(value); }

std::wstring quote(const fs::path& value) {
    std::wstring result=L"\"";
    for (const auto character:value.wstring()) {
        if (character==L'\"') result+=L'\\';
        result+=character;
    }
    return result+L"\"";
}

bool launch(const fs::path& executable,const std::wstring& arguments,PROCESS_INFORMATION& process) {
    STARTUPINFOW startup{sizeof(startup)};
    auto command=quote(executable)+(arguments.empty()?L"":L" "+arguments);
    return CreateProcessW(executable.c_str(),command.data(),nullptr,nullptr,FALSE,0,nullptr,
                          executable.parent_path().c_str(),&startup,&process)!=FALSE;
}
} // namespace

int WINAPI wWinMain(HINSTANCE,HINSTANCE,PWSTR,int) {
    try {
        const auto root=module_path().parent_path();
        const auto state_path=root/L"current.json";
        auto state=read_state(state_path);
        auto current=state.value("current","");
        auto previous=state.value("previous","");
        bool pending=state.value("pending",false);
        bool attempted=state.value("attempted",false);
        auto token=state.value("token","");
        if (!valid_component(current) || (pending && (!valid_component(previous)||token.size()<16||token.size()>80)))
            throw std::runtime_error("Unsafe version state");
        const auto health=update::update_data_root()/L"health"/utf8_to_wide(token);
        if (pending && fs::is_regular_file(health)) {
            state["pending"]=false; state["attempted"]=false; state["previous"]=""; state["token"]="";
            write_state(state_path,state); pending=false;
        } else if (pending && attempted) {
            state["current"]=previous; state["previous"]=""; state["pending"]=false;
            state["attempted"]=false; state["token"]="";
            write_state(state_path,state); current=previous; pending=false;
        }
        const auto executable=root/L"versions"/utf8_to_wide(current)/L"SempervirensApp.exe";
        if (!fs::is_regular_file(executable)) throw std::runtime_error("Version payload is missing");
        std::wstring arguments;
        if (pending) {
            state["attempted"]=true; write_state(state_path,state);
            arguments=L"--update-health="+utf8_to_wide(token);
        }
        PROCESS_INFORMATION process{};
        if (!launch(executable,arguments,process)) throw std::runtime_error("Could not start app");
        CloseHandle(process.hThread);
        if (!pending) { CloseHandle(process.hProcess); return 0; }
        for (int iteration=0; iteration<300; ++iteration) {
            if (fs::is_regular_file(health)) {
                state["pending"]=false; state["attempted"]=false; state["previous"]=""; state["token"]="";
                write_state(state_path,state); CloseHandle(process.hProcess); return 0;
            }
            if (WaitForSingleObject(process.hProcess,100)==WAIT_OBJECT_0) break;
        }
        if (!fs::is_regular_file(health) && WaitForSingleObject(process.hProcess,0)==WAIT_OBJECT_0) {
            state["current"]=previous; state["previous"]=""; state["pending"]=false;
            state["attempted"]=false; state["token"]=""; write_state(state_path,state);
            const auto fallback=root/L"versions"/utf8_to_wide(previous)/L"SempervirensApp.exe";
            PROCESS_INFORMATION replacement{};
            if (fs::is_regular_file(fallback) && launch(fallback,L"",replacement)) {
                CloseHandle(replacement.hThread); CloseHandle(replacement.hProcess);
            }
        }
        CloseHandle(process.hProcess);
        return 0;
    } catch (...) {
        MessageBoxW(nullptr,L"Sempervirens 无法启动。请重新安装应用或联系项目维护者。",
                    L"Sempervirens",MB_OK|MB_ICONERROR);
        return 1;
    }
}
