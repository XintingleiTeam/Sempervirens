#include "atomic_file.hpp"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <stdexcept>
#include <system_error>

namespace sempervirens {

namespace {

std::atomic_uint64_t next_write_id{0};

#ifdef SEMPERVIRENS_TESTING
std::function<void(std::size_t, std::size_t)> progress_observer_for_test;
#endif

void check_cancelled(std::stop_token token) {
    if (token.stop_requested()) throw std::runtime_error("Migration cancelled");
}

} // namespace

void write_file_atomically(const fs::path& target, std::string_view bytes,
                           std::stop_token cancellation) {
    check_cancelled(cancellation);
    for (int attempt = 0; attempt < 32; ++attempt) {
        const auto temporary = target.parent_path() /
            (L".sempervirens-tmp-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
             std::to_wstring(GetTickCount64()) + L"-" +
             std::to_wstring(next_write_id.fetch_add(1, std::memory_order_relaxed)));
        HANDLE handle = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                    FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (handle == INVALID_HANDLE_VALUE) {
            const auto error = GetLastError();
            if (error == ERROR_FILE_EXISTS || error == ERROR_ALREADY_EXISTS) continue;
            throw std::system_error(static_cast<int>(error), std::system_category(),
                                    "Cannot create temporary file");
        }
        try {
            std::size_t position = 0;
            while (position < bytes.size()) {
                check_cancelled(cancellation);
                const auto chunk = static_cast<DWORD>(std::min<std::size_t>(bytes.size() - position, 64 * 1024));
                DWORD written = 0;
                if (!WriteFile(handle, bytes.data() + position, chunk, &written, nullptr) || written != chunk)
                    throw std::system_error(static_cast<int>(GetLastError()), std::system_category(),
                                            "Cannot write temporary file");
                position += written;
#ifdef SEMPERVIRENS_TESTING
                if (progress_observer_for_test) progress_observer_for_test(position, bytes.size());
#endif
            }
            check_cancelled(cancellation);
            if (!FlushFileBuffers(handle))
                throw std::system_error(static_cast<int>(GetLastError()), std::system_category(),
                                        "Cannot flush temporary file");
            if (!CloseHandle(handle)) {
                handle = INVALID_HANDLE_VALUE;
                throw std::runtime_error("Cannot close temporary file");
            }
            handle = INVALID_HANDLE_VALUE;
            check_cancelled(cancellation);
            if (!MoveFileExW(temporary.c_str(), target.c_str(),
                             MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
                throw std::system_error(static_cast<int>(GetLastError()), std::system_category(),
                                        "Cannot replace destination file");
        } catch (...) {
            if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
            DeleteFileW(temporary.c_str());
            throw;
        }
        return;
    }
    throw std::runtime_error("Could not reserve a temporary file path");
}

#ifdef SEMPERVIRENS_TESTING
void set_atomic_write_progress_observer_for_test(std::function<void(std::size_t, std::size_t)> observer) {
    progress_observer_for_test = std::move(observer);
}
#endif

} // namespace sempervirens
