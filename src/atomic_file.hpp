#pragma once

#include "model.hpp"

#include <stop_token>
#include <string_view>
#ifdef SEMPERVIRENS_TESTING
#include <cstddef>
#include <functional>
#endif

namespace sempervirens {

void write_file_atomically(const fs::path& target, std::string_view bytes,
                           std::stop_token cancellation = {});

#ifdef SEMPERVIRENS_TESTING
void set_atomic_write_progress_observer_for_test(std::function<void(std::size_t, std::size_t)> observer);
#endif

} // namespace sempervirens
