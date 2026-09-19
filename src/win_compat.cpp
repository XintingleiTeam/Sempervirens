#include "win_compat.hpp"

#include <wingdi.h>

namespace sempervirens::win_compat {

namespace {

HMODULE user32() {
    static const auto module = GetModuleHandleW(L"user32.dll");
    return module;
}

template <typename Function>
Function user32_function(const char* name) {
    const auto module = user32();
    return module ? reinterpret_cast<Function>(GetProcAddress(module, name)) : nullptr;
}

}

void enable_best_dpi_awareness() {
    using SetContext = BOOL (WINAPI*)(DPI_AWARENESS_CONTEXT);
    if (const auto set_context = user32_function<SetContext>("SetProcessDpiAwarenessContext")) {
        if (set_context(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) return;
        if (set_context(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE)) return;
    }
    SetProcessDPIAware();
}

UINT window_dpi(HWND window) {
    using GetDpi = UINT (WINAPI*)(HWND);
    if (const auto get_dpi = user32_function<GetDpi>("GetDpiForWindow")) {
        const auto dpi = get_dpi(window);
        if (dpi) return dpi;
    }
    const auto context = GetDC(window);
    if (!context) return 96;
    const auto dpi = GetDeviceCaps(context, LOGPIXELSX);
    ReleaseDC(window, context);
    return dpi > 0 ? static_cast<UINT>(dpi) : 96;
}

BOOL adjust_window_rect(RECT* rectangle, DWORD style, BOOL has_menu,
                        DWORD extended_style, UINT dpi) {
    using AdjustForDpi = BOOL (WINAPI*)(LPRECT, DWORD, BOOL, DWORD, UINT);
    if (const auto adjust = user32_function<AdjustForDpi>("AdjustWindowRectExForDpi"))
        return adjust(rectangle, style, has_menu, extended_style, dpi);
    return AdjustWindowRectEx(rectangle, style, has_menu, extended_style);
}

}
