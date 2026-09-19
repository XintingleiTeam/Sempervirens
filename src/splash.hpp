#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <filesystem>

namespace sempervirens {

bool show_startup_splash(HINSTANCE instance, HWND owner, bool english, bool visible = true,
                         bool skip_for_test = false);
bool capture_startup_splash(const std::filesystem::path& output, bool english, float elapsed_ms,
                           bool benchmark = false);

} // namespace sempervirens
