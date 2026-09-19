#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace sempervirens::win_compat {

void enable_best_dpi_awareness();
UINT window_dpi(HWND window);
BOOL adjust_window_rect(RECT* rectangle, DWORD style, BOOL has_menu,
                        DWORD extended_style, UINT dpi);

}
