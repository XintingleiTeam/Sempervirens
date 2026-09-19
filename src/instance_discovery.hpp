#pragma once

#include "model.hpp"

#include <functional>
#include <stop_token>

namespace sempervirens {

using ScanProgressCallback = std::function<void(const ScanProgress&)>;

InstanceSelection discover_instances(const fs::path& selected_path,
                                     std::wstring_view default_name,
                                     std::wstring_view direct_name,
                                     std::stop_token cancellation = {},
                                     const ScanProgressCallback& progress = {}, bool english = false);

} // namespace sempervirens
