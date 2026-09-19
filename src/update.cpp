#include "update.hpp"
#include "update_public_key.hpp"
#include "atomic_file.hpp"
#include "text.hpp"

#include <nlohmann/json.hpp>
#include <miniz.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bcrypt.h>
#include <winhttp.h>
#include <shlobj.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstring>
#include <fstream>
#include <limits>
#include <stdexcept>

namespace sempervirens::update {
namespace {

template <class T> void close_handle(T& handle) {
    if (handle) { WinHttpCloseHandle(handle); handle = nullptr; }
}

std::vector<std::uint8_t> read_file_limited(const fs::path& path, std::uint64_t limit) {
    std::error_code error;
    const auto size = fs::file_size(path, error);
    if (error || size > limit || size > std::numeric_limits<std::size_t>::max())
        throw std::runtime_error("Update file is missing or too large");
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    std::ifstream input(path, std::ios::binary);
    if (!input || (size && !input.read(reinterpret_cast<char*>(bytes.data()),
                                       static_cast<std::streamsize>(size))))
        throw std::runtime_error("Update file could not be read");
    return bytes;
}

std::string hex(const std::uint8_t* bytes, std::size_t size) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string result(size * 2, '0');
    for (std::size_t i = 0; i < size; ++i) {
        result[i*2] = digits[bytes[i] >> 4];
        result[i*2+1] = digits[bytes[i] & 15];
    }
    return result;
}

std::array<std::uint8_t, 32> sha256_raw(const void* data, std::size_t size) {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    DWORD object_size = 0, returned = 0;
    std::vector<std::uint8_t> object;
    std::array<std::uint8_t, 32> digest{};
    NTSTATUS status = BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
    if (status >= 0) status = BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
        reinterpret_cast<PUCHAR>(&object_size), sizeof(object_size), &returned, 0);
    if (status >= 0) { object.resize(object_size); status = BCryptCreateHash(algorithm, &hash,
        object.data(), object_size, nullptr, 0, 0); }
    if (status >= 0 && size) status = BCryptHashData(hash,
        const_cast<PUCHAR>(static_cast<const UCHAR*>(data)), static_cast<ULONG>(size), 0);
    if (status >= 0) status = BCryptFinishHash(hash, digest.data(), digest.size(), 0);
    if (hash) BCryptDestroyHash(hash);
    if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
    if (status < 0) throw std::runtime_error("SHA-256 failed");
    return digest;
}

template <class T> T read_integer(const std::vector<std::uint8_t>& bytes, std::size_t& offset) {
    if (offset > bytes.size() || sizeof(T) > bytes.size() - offset)
        throw std::runtime_error("Delta is truncated");
    T value = 0;
    for (std::size_t i = 0; i < sizeof(T); ++i)
        value |= static_cast<T>(bytes[offset++]) << (i * 8);
    return value;
}

std::string required_string(const nlohmann::json& item, const char* key, std::size_t limit) {
    const auto at = item.find(key);
    if (at == item.end() || !at->is_string()) throw std::runtime_error("Update manifest field is invalid");
    auto result = at->get<std::string>();
    if (result.empty() || result.size() > limit) throw std::runtime_error("Update manifest field is invalid");
    return result;
}

bool allowed_update_url(std::wstring_view value) {
    URL_COMPONENTS parts{sizeof(parts)};
    parts.dwSchemeLength = parts.dwHostNameLength = parts.dwUrlPathLength =
        parts.dwExtraInfoLength = static_cast<DWORD>(-1);
    std::wstring copy(value);
    if (!WinHttpCrackUrl(copy.c_str(), 0, 0, &parts) || parts.nScheme != INTERNET_SCHEME_HTTPS)
        return false;
    const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
    return _wcsicmp(host.c_str(), L"api.xintinglei.cn") == 0 ||
           _wcsicmp(host.c_str(), L"github.com") == 0 ||
           _wcsicmp(host.c_str(), L"release-assets.githubusercontent.com") == 0 ||
           _wcsicmp(host.c_str(), L"objects.githubusercontent.com") == 0;
}

} // namespace

std::string sha256_bytes(const void* data, std::size_t size) {
    const auto digest = sha256_raw(data, size);
    return hex(digest.data(), digest.size());
}

std::string sha256_file(const fs::path& path) {
    const auto bytes = read_file_limited(path, 512ULL * 1024 * 1024);
    return sha256_bytes(bytes.data(), bytes.size());
}

bool verify_manifest_signature(std::string_view bytes, const std::vector<std::uint8_t>& signature) {
    if (signature.size() != 64 ||
        std::all_of(signing_public_x.begin(), signing_public_x.end(), [](auto b) { return b == 0; }))
        return false;
    struct PublicBlob { BCRYPT_ECCKEY_BLOB header; std::array<std::uint8_t,64> coordinates; } blob{};
    blob.header.dwMagic = BCRYPT_ECDSA_PUBLIC_P256_MAGIC;
    blob.header.cbKey = 32;
    std::copy(signing_public_x.begin(), signing_public_x.end(), blob.coordinates.begin());
    std::copy(signing_public_y.begin(), signing_public_y.end(), blob.coordinates.begin()+32);
    const auto digest = sha256_raw(bytes.data(), bytes.size());
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_KEY_HANDLE key = nullptr;
    auto status = BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_ECDSA_P256_ALGORITHM, nullptr, 0);
    if (status >= 0) status = BCryptImportKeyPair(algorithm, nullptr, BCRYPT_ECCPUBLIC_BLOB, &key,
        reinterpret_cast<PUCHAR>(&blob), sizeof(blob), 0);
    if (status >= 0) status = BCryptVerifySignature(key, nullptr,
        const_cast<PUCHAR>(digest.data()), digest.size(),
        const_cast<PUCHAR>(signature.data()), static_cast<ULONG>(signature.size()), 0);
    if (key) BCryptDestroyKey(key);
    if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
    return status >= 0;
}

bool safe_version(std::string_view version) {
    if (version.empty() || version.size() > 32 || version.front()=='.' || version.back()=='.') return false;
    return std::all_of(version.begin(), version.end(), [](unsigned char value) {
        return (value>='0'&&value<='9') || value=='.' || value=='-';
    });
}

int compare_versions(std::string_view left, std::string_view right) {
    const auto parse = [](std::string_view value) {
        std::array<unsigned,4> parts{};
        std::size_t index = 0, start = 0;
        while (start < value.size() && index < parts.size()) {
            const auto end = value.find_first_of(".-", start);
            const auto token = value.substr(start, end == std::string_view::npos ? value.size()-start : end-start);
            unsigned number = 0;
            if (token.empty() || std::from_chars(token.data(),token.data()+token.size(),number).ec != std::errc{})
                break;
            parts[index++] = number;
            if (end == std::string_view::npos || value[end]=='-') break;
            start = end+1;
        }
        return parts;
    };
    const auto a=parse(left), b=parse(right);
    return a<b ? -1 : a>b ? 1 : 0;
}

Manifest parse_manifest(std::string_view bytes, std::string_view expected_channel) {
    if (bytes.size() > 128*1024) throw std::runtime_error("Update manifest is too large");
    const auto json = nlohmann::json::parse(bytes);
    Manifest result;
    result.schema = json.value("schema",0U);
    result.product = required_string(json,"product",64);
    result.channel = required_string(json,"channel",32);
    result.version = required_string(json,"version",32);
    result.serial = json.value("serial",0ULL);
    result.minimum_updater_protocol = json.value("minimumUpdaterProtocol",0U);
    result.published_utc = required_string(json,"publishedUtc",64);
    result.output_sha256 = required_string(json,"outputSha256",64);
    result.output_size = json.value("outputSize",0ULL);
    if (result.schema != 1 || result.product != product_id || result.channel != expected_channel ||
        !safe_version(result.version) || !result.serial || result.minimum_updater_protocol > updater_protocol ||
        result.output_sha256.size()!=64 || !result.output_size || result.output_size > 256ULL*1024*1024)
        throw std::runtime_error("Update manifest is not compatible with this app");
    const auto notes = json.find("notes");
    if (notes != json.end() && notes->is_object()) {
        if (const auto zh=notes->find("zh-CN"); zh!=notes->end() && zh->is_string())
            result.notes_zh=utf8_to_wide(zh->get<std::string>());
        if (const auto en=notes->find("en"); en!=notes->end() && en->is_string())
            result.notes_en=utf8_to_wide(en->get<std::string>());
    }
    const auto packages=json.find("packages");
    if (packages==json.end() || !packages->is_array() || packages->empty() || packages->size()>16)
        throw std::runtime_error("Update package list is invalid");
    for (const auto& item:*packages) {
        Package package;
        package.kind=required_string(item,"kind",16);
        package.from_version=item.value("fromVersion","");
        package.from_sha256=item.value("fromSha256","");
        if (const auto urls=item.find("urls"); urls!=item.end() && urls->is_array()) {
            if (urls->empty() || urls->size()>4) throw std::runtime_error("Update mirrors are invalid");
            for (const auto& url:*urls) {
                if (!url.is_string() || url.get_ref<const std::string&>().empty() ||
                    url.get_ref<const std::string&>().size()>2048)
                    throw std::runtime_error("Update mirror is invalid");
                package.urls.push_back(url.get<std::string>());
            }
        } else package.urls.push_back(required_string(item,"url",2048));
        package.size=item.value("size",0ULL);
        package.sha256=required_string(item,"sha256",64);
        if ((package.kind!="delta"&&package.kind!="full") ||
            (package.kind=="delta"&&(!safe_version(package.from_version)||package.from_sha256.size()!=64)) ||
            package.sha256.size()!=64 || !package.size || package.size>128ULL*1024*1024 ||
            package.urls.empty() || std::any_of(package.urls.begin(),package.urls.end(),[](const auto& url) {
                return !allowed_update_url(utf8_to_wide(url));
            }))
            throw std::runtime_error("Update package entry is invalid");
        result.packages.push_back(std::move(package));
    }
    return result;
}

std::vector<std::uint8_t> download_https(std::wstring_view url, std::size_t limit,
                                         std::stop_token stop, Progress progress) {
    if (!allowed_update_url(url)) throw std::runtime_error("Update URL is not allowed");
    URL_COMPONENTS parts{sizeof(parts)};
    parts.dwSchemeLength=parts.dwHostNameLength=parts.dwUrlPathLength=parts.dwExtraInfoLength=DWORD(-1);
    std::wstring copy(url);
    if (!WinHttpCrackUrl(copy.c_str(),0,0,&parts)) throw std::runtime_error("Update URL is invalid");
    std::wstring host(parts.lpszHostName,parts.dwHostNameLength);
    std::wstring path(parts.lpszUrlPath,parts.dwUrlPathLength);
    if (parts.dwExtraInfoLength) path.append(parts.lpszExtraInfo,parts.dwExtraInfoLength);
    HINTERNET session=WinHttpOpen(L"Sempervirens/0.1.0.0",WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                  WINHTTP_NO_PROXY_NAME,WINHTTP_NO_PROXY_BYPASS,0);
    HINTERNET connection=nullptr,request=nullptr;
    if (!session) throw std::runtime_error("Update connection could not be created");
    WinHttpSetTimeouts(session,5000,5000,15000,15000);
    connection=WinHttpConnect(session,host.c_str(),parts.nPort,0);
    if (connection) request=WinHttpOpenRequest(connection,L"GET",path.c_str(),nullptr,
        WINHTTP_NO_REFERER,WINHTTP_DEFAULT_ACCEPT_TYPES,WINHTTP_FLAG_SECURE);
    DWORD redirect_policy=WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP;
    if (request) WinHttpSetOption(request,WINHTTP_OPTION_REDIRECT_POLICY,&redirect_policy,sizeof(redirect_policy));
    bool okay=request && WinHttpSendRequest(request,WINHTTP_NO_ADDITIONAL_HEADERS,0,
        WINHTTP_NO_REQUEST_DATA,0,0,0) && WinHttpReceiveResponse(request,nullptr);
    DWORD status=0,status_size=sizeof(status);
    if (okay) okay=WinHttpQueryHeaders(request,WINHTTP_QUERY_STATUS_CODE|WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX,&status,&status_size,WINHTTP_NO_HEADER_INDEX) && status==200;
    std::vector<std::uint8_t> result;
    while (okay) {
        if (stop.stop_requested()) { okay=false; SetLastError(ERROR_CANCELLED); break; }
        DWORD available=0;
        if (!WinHttpQueryDataAvailable(request,&available)) { okay=false; break; }
        if (!available) break;
        if (available>limit-result.size()) { okay=false; SetLastError(ERROR_FILE_TOO_LARGE); break; }
        const auto old=result.size(); result.resize(old+available);
        DWORD read=0;
        if (!WinHttpReadData(request,result.data()+old,available,&read)) { okay=false; break; }
        result.resize(old+read);
        if (progress) progress(result.size(),0);
    }
    close_handle(request); close_handle(connection); close_handle(session);
    if (!okay) throw std::runtime_error(GetLastError()==ERROR_CANCELLED ? "Update was cancelled" :
                                         "Update download failed");
    return result;
}

void write_verified_file(const std::vector<std::uint8_t>& bytes, const fs::path& output,
                         std::string_view expected_sha256, std::uint64_t expected_size) {
    if (bytes.size()!=expected_size || sha256_bytes(bytes.data(),bytes.size())!=expected_sha256)
        throw std::runtime_error("Downloaded update failed verification");
    write_file_atomically(output,std::string_view(reinterpret_cast<const char*>(bytes.data()),bytes.size()));
}

void apply_delta(const fs::path& old_file, const std::vector<std::uint8_t>& patch,
                 const fs::path& output, std::string_view expected_output_sha256,
                 std::uint64_t expected_output_size, std::stop_token stop, Progress progress) {
    constexpr std::string_view magic("SVDIFF1\0",8);
    if (patch.size()<168 || std::memcmp(patch.data(),magic.data(),magic.size())!=0)
        throw std::runtime_error("Delta header is invalid");
    std::size_t at=magic.size();
    const auto old_size=read_integer<std::uint64_t>(patch,at);
    const auto new_size=read_integer<std::uint64_t>(patch,at);
    const std::string old_hash(reinterpret_cast<const char*>(patch.data()+at),64); at+=64;
    const std::string new_hash(reinterpret_cast<const char*>(patch.data()+at),64); at+=64;
    const auto stream_size=read_integer<std::uint64_t>(patch,at);
    const auto compressed_size=read_integer<std::uint64_t>(patch,at);
    if (new_size!=expected_output_size || new_hash!=expected_output_sha256 ||
        stream_size>512ULL*1024*1024 || compressed_size!=patch.size()-at)
        throw std::runtime_error("Delta metadata is invalid");
    auto old=read_file_limited(old_file,256ULL*1024*1024);
    if (old.size()!=old_size || sha256_bytes(old.data(),old.size())!=old_hash)
        throw std::runtime_error("Installed version does not match this delta");
    std::vector<std::uint8_t> stream(static_cast<std::size_t>(stream_size));
    mz_ulong actual=static_cast<mz_ulong>(stream.size());
    if (mz_uncompress(stream.data(),&actual,patch.data()+at,static_cast<mz_ulong>(compressed_size))!=MZ_OK ||
        actual!=stream.size()) throw std::runtime_error("Delta data is damaged");
    std::vector<std::uint8_t> result;
    result.reserve(static_cast<std::size_t>(new_size));
    at=0;
    while (at<stream.size()) {
        if (stop.stop_requested()) throw std::runtime_error("Update was cancelled");
        const auto opcode=stream[at++];
        if (opcode==255) break;
        if (opcode==0) {
            const auto offset=read_integer<std::uint64_t>(stream,at);
            const auto length=read_integer<std::uint32_t>(stream,at);
            if (offset>old.size() || length>old.size()-offset) throw std::runtime_error("Delta copy is invalid");
            result.insert(result.end(),old.begin()+offset,old.begin()+offset+length);
        } else if (opcode==1) {
            const auto length=read_integer<std::uint32_t>(stream,at);
            if (at>stream.size() || length>stream.size()-at) throw std::runtime_error("Delta literal is invalid");
            result.insert(result.end(),stream.begin()+at,stream.begin()+at+length); at+=length;
        } else throw std::runtime_error("Delta opcode is invalid");
        if (result.size()>new_size) throw std::runtime_error("Delta output is too large");
        if (progress) progress(result.size(),new_size);
    }
    if (result.size()!=new_size || sha256_bytes(result.data(),result.size())!=new_hash)
        throw std::runtime_error("Rebuilt update failed verification");
    write_file_atomically(output,std::string_view(reinterpret_cast<const char*>(result.data()),result.size()));
}

fs::path installation_root(const fs::path& module_directory) {
    if (module_directory.parent_path().filename()==L"versions")
        return module_directory.parent_path().parent_path();
    return module_directory;
}

fs::path update_data_root() {
    PWSTR local=nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData,0,nullptr,&local))) return {};
    fs::path result=fs::path(local)/L"XintingleiTeam"/L"Sempervirens"/L"updates";
    CoTaskMemFree(local);
    return result;
}

} // namespace sempervirens::update
