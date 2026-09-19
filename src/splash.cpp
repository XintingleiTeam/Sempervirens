#include "splash.hpp"
#include "win_compat.hpp"

#include <d2d1.h>
#include <d2d1helper.h>
#include <dwrite.h>
#include <dwmapi.h>
#include <wincodec.h>
#include "branding.hpp"
#include "startup_model.hpp"

#include <algorithm>
#include <cmath>
#include <vector>
#include <iterator>
#include <fstream>
#include <array>

namespace sempervirens {

namespace {
constexpr wchar_t splash_class[] = L"SempervirensStartupWindow";
constexpr UINT_PTR splash_timer = 1;
constexpr float splash_width = 680;
constexpr float splash_height = 280;
constexpr ULONGLONG splash_duration_ms = 1180;

template <typename T> void release(T*& pointer) {
    if (pointer) { pointer->Release(); pointer = nullptr; }
}

D2D1_COLOR_F ink(unsigned hex, float alpha = 1) {
    return D2D1::ColorF(hex, alpha);
}

float ease_out(float progress) {
    const auto remaining = 1.0f - std::clamp(progress, 0.0f, 1.0f);
    return 1.0f - remaining * remaining * remaining;
}

float smooth_reveal(float time, float start, float duration) {
    const float t = std::clamp((time-start)/duration, 0.0f, 1.0f);
    return t*t*(3-2*t);
}

// Fixed, front-facing silhouette from the original designer's 3DM. Reveal
// coordinates follow the original arc boundaries, not a horizontal wipe.
// No camera motion, scale change, rotation, moving light or letter movement.
class StartupMark {
    struct Point { D2D1_POINT_2F position; float reveal; };
    using Face = std::array<Point,3>;
public:
    StartupMark() {
        batch_.reserve(std::size(startup_model::triangles));
        for (const auto indices : startup_model::triangles) {
            const auto a = startup_model::vertices[indices.a];
            const auto b = startup_model::vertices[indices.b];
            const auto c = startup_model::vertices[indices.c];
            // Front-facing projected area; the camera never changes.
            if ((b.x-a.x)*(c.y-a.y)-(b.y-a.y)*(c.x-a.x) <= 1e-10f) continue;
            const auto project = [](const auto& v) {
                return Point{D2D1::Point2F(340+396*v.x,117+396*v.y),v.reveal};
            };
            parts_[indices.part].push_back({project(a),project(b),project(c)});
        }
    }
    ~StartupMark() { reset(); }
    void reset() {
        for (auto*& mesh : meshes_) release(mesh);
        release(brush_); release(surface_); release(owner_);
    }
    static std::array<float,4> timeline(float time) {
        return {smooth_reveal(time,0,180), smooth_reveal(time,180,350),
                smooth_reveal(time,530,260), smooth_reveal(time,790,100)};
    }
    bool draw(ID2D1RenderTarget* canvas, float elapsed_ms) {
        if (owner_ != canvas) reset();
        if (!surface_) {
            float dpi_x = 96, dpi_y = 96;
            canvas->GetDpi(&dpi_x, &dpi_y);
            const auto pixels = D2D1::SizeU(
                static_cast<UINT32>(std::ceil(splash_width*dpi_x/96*2)),
                static_cast<UINT32>(std::ceil(splash_height*dpi_y/96*2)));
            if (FAILED(canvas->CreateCompatibleRenderTarget(D2D1::SizeF(splash_width, splash_height),
                pixels, &surface_))) return false;
            if (FAILED(surface_->CreateSolidColorBrush(ink(0xFFFFFF), &brush_))) {
                reset(); return false;
            }
            owner_ = canvas; owner_->AddRef();
            for (std::size_t part=0; part<parts_.size(); ++part) {
                batch_.clear();
                for (const auto& face : parts_[part])
                    batch_.push_back({face[0].position,face[1].position,face[2].position});
                if (!create_mesh(&meshes_[part])) { reset(); return false; }
            }
        }
        const auto progress = timeline(elapsed_ms);
        surface_->BeginDraw();
        surface_->Clear(D2D1::ColorF(0,0,0,0));
        surface_->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED);
        bool okay = true;
        for (std::size_t part=0; part<parts_.size(); ++part) {
            if (progress[part] <= 0) continue;
            const bool fade = part==0 || part==3;
            brush_->SetColor(ink(0xF7F9FA,fade ? progress[part] : 1.0f));
            if (fade || progress[part] >= 1) surface_->FillMesh(meshes_[part],brush_);
            else {
                batch_.clear();
                for (const auto& face : parts_[part]) append_revealed(face,progress[part]);
                if (batch_.empty()) continue;
                ID2D1Mesh* partial = nullptr;
                if (create_mesh(&partial)) surface_->FillMesh(partial,brush_);
                else okay = false;
                release(partial);
            }
        }
        if (FAILED(surface_->EndDraw())) okay = false;
        ID2D1Bitmap* bitmap = nullptr;
        if (okay && SUCCEEDED(surface_->GetBitmap(&bitmap))) {
            canvas->DrawBitmap(bitmap, D2D1::RectF(0,0,splash_width,splash_height), 1,
                D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        } else okay = false;
        release(bitmap);
        return okay;
    }
private:
    bool create_mesh(ID2D1Mesh** mesh) {
        if (FAILED(surface_->CreateMesh(mesh))) return false;
        ID2D1TessellationSink* sink = nullptr;
        if (FAILED((*mesh)->Open(&sink))) { release(*mesh); return false; }
        sink->AddTriangles(batch_.data(),static_cast<UINT32>(batch_.size()));
        const auto result = sink->Close();
        release(sink);
        if (FAILED(result)) { release(*mesh); return false; }
        return true;
    }
    void append_revealed(const Face& face,float progress) {
        // Clip triangles at the moving arc-length boundary rather than popping
        // whole triangles in. Adjacent faces share the exact same intersection.
        std::array<D2D1_POINT_2F,4> polygon{};
        std::size_t count = 0;
        for (std::size_t i=0; i<3; ++i) {
            const auto& a=face[i];
            const auto& b=face[(i+1)%3];
            const bool inside_a=a.reveal<=progress, inside_b=b.reveal<=progress;
            if (inside_a) polygon[count++]=a.position;
            if (inside_a != inside_b) {
                const float t=(progress-a.reveal)/(b.reveal-a.reveal);
                polygon[count++]=D2D1::Point2F(a.position.x+(b.position.x-a.position.x)*t,
                                             a.position.y+(b.position.y-a.position.y)*t);
            }
        }
        for (std::size_t i=1; i+1<count; ++i)
            batch_.push_back({polygon[0],polygon[i],polygon[i+1]});
    }
    std::array<std::vector<Face>,4> parts_;
    std::array<ID2D1Mesh*,4> meshes_{};
    std::vector<D2D1_TRIANGLE> batch_;
    ID2D1RenderTarget* owner_ = nullptr;
    ID2D1BitmapRenderTarget* surface_ = nullptr;
    ID2D1SolidColorBrush* brush_ = nullptr;
};

class SplashGraphics {
public:
    ~SplashGraphics() {
        release(window_brush_);
        release(window_target_);
        release(caption_);
        release(write_factory_);
        release(factory_);
    }

    bool initialize() {
        if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &factory_))) return false;
        if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                       reinterpret_cast<IUnknown**>(&write_factory_)))) return false;
        if (FAILED(write_factory_->CreateTextFormat(L"Microsoft YaHei UI", nullptr,
            DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL, 13, L"zh-CN", &caption_))) return false;
        return true;
    }

    void draw(ID2D1RenderTarget* canvas, ID2D1SolidColorBrush* brush, bool english, float elapsed_ms) {
        canvas->Clear(ink(0x0B0D10));
        brush->SetColor(ink(0x52616B, 0.58f));
        canvas->DrawRoundedRectangle(D2D1::RoundedRect(
            D2D1::RectF(0.5f, 0.5f, splash_width - 0.5f, splash_height - 0.5f), 21, 21), brush, 1);

        model_rendered_ = model_.draw(canvas, elapsed_ms);
        if (!model_rendered_)
            branding_.draw(canvas, D2D1::RectF(142, 20, 538, 213));
        auto draw_text = [&](const wchar_t* text, IDWriteTextFormat* format,
                             const D2D1_RECT_F& bounds, unsigned value) {
            brush->SetColor(ink(value, ease_out((elapsed_ms-380)/240)));
            canvas->DrawTextW(text, static_cast<UINT32>(wcslen(text)), format, bounds, brush,
                              D2D1_DRAW_TEXT_OPTIONS_CLIP);
        };
        caption_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        draw_text(english ? L"Bring your settings and play history to a new instance" :
                  L"把个人设置和游玩记录带到新实例",
                  caption_, D2D1::RectF(45, 232, 635, 255), 0xAAB6C1);
    }

    bool paint(HWND hwnd, UINT dpi, bool english, float elapsed_ms) {
        if (!window_target_) {
            RECT client{};
            GetClientRect(hwnd, &client);
            const auto properties = D2D1::HwndRenderTargetProperties(hwnd,
                D2D1::SizeU(std::max(1L, client.right), std::max(1L, client.bottom)));
            if (FAILED(factory_->CreateHwndRenderTarget(D2D1::RenderTargetProperties(),
                                                        properties, &window_target_))) return false;
            window_target_->SetDpi(static_cast<float>(dpi), static_cast<float>(dpi));
            if (FAILED(window_target_->CreateSolidColorBrush(ink(0xFFFFFF), &window_brush_))) {
                release(window_target_);
                return false;
            }
        }
        window_target_->BeginDraw();
        draw(window_target_, window_brush_, english, elapsed_ms);
        const auto result = window_target_->EndDraw();
        if (result == D2DERR_RECREATE_TARGET) {
            branding_.reset_target();
            model_.reset();
            release(window_brush_);
            release(window_target_);
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return SUCCEEDED(result) || result == D2DERR_RECREATE_TARGET;
    }

    bool capture(const std::filesystem::path& output, bool english, float elapsed_ms, bool benchmark) {
        IWICImagingFactory* imaging = nullptr;
        IWICBitmap* bitmap = nullptr;
        ID2D1RenderTarget* canvas = nullptr;
        ID2D1SolidColorBrush* brush = nullptr;
        IWICBitmapEncoder* encoder = nullptr;
        IWICStream* stream = nullptr;
        IWICBitmapFrameEncode* frame = nullptr;
        IPropertyBag2* properties = nullptr;
        bool saved = false;
        do {
            if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                        IID_PPV_ARGS(&imaging)))) break;
            if (FAILED(imaging->CreateBitmap(static_cast<UINT>(splash_width),
                                             static_cast<UINT>(splash_height),
                                             GUID_WICPixelFormat32bppPBGRA,
                                             WICBitmapCacheOnLoad, &bitmap))) break;
            const auto target_properties = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_SOFTWARE,
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
            if (FAILED(factory_->CreateWicBitmapRenderTarget(bitmap, target_properties, &canvas))) break;
            canvas->SetDpi(96, 96);
            if (FAILED(canvas->CreateSolidColorBrush(ink(0xFFFFFF), &brush))) break;
            bool rendered = true;
            std::vector<double> frame_times;
            LARGE_INTEGER frequency{};
            QueryPerformanceFrequency(&frequency);
            for (int index=0; index<(benchmark ? 76 : 1); ++index) {
                LARGE_INTEGER start{}, end{};
                QueryPerformanceCounter(&start);
                canvas->BeginDraw();
                draw(canvas, brush, english, benchmark ? index*(splash_duration_ms/75.0f) : elapsed_ms);
                const auto result = canvas->EndDraw();
                QueryPerformanceCounter(&end);
                if (FAILED(result) || !model_rendered_) { rendered = false; break; }
                frame_times.push_back(1000.0*(end.QuadPart-start.QuadPart)/frequency.QuadPart);
            }
            if (!rendered) break;
            if (benchmark) {
                auto log_path = output;
                log_path.replace_extension(".csv");
                std::ofstream log(log_path);
                log << "frame,software_render_ms\n";
                for (std::size_t i=0; i<frame_times.size(); ++i) log << i << ',' << frame_times[i] << '\n';
                if (!log) break;
            }
            if (FAILED(imaging->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder))) break;
            if (FAILED(imaging->CreateStream(&stream))) break;
            if (FAILED(stream->InitializeFromFilename(output.c_str(), GENERIC_WRITE))) break;
            if (FAILED(encoder->Initialize(stream, WICBitmapEncoderNoCache))) break;
            if (FAILED(encoder->CreateNewFrame(&frame, &properties))) break;
            if (FAILED(frame->Initialize(properties))) break;
            if (FAILED(frame->SetSize(static_cast<UINT>(splash_width),
                                      static_cast<UINT>(splash_height)))) break;
            WICPixelFormatGUID format = GUID_WICPixelFormat32bppPBGRA;
            if (FAILED(frame->SetPixelFormat(&format))) break;
            if (FAILED(frame->WriteSource(bitmap, nullptr))) break;
            if (FAILED(frame->Commit()) || FAILED(encoder->Commit())) break;
            saved = true;
        } while (false);
        release(properties); release(frame); release(stream); release(encoder);
        release(brush); release(canvas); release(bitmap); release(imaging);
        return saved;
    }

private:
    StartupMark model_;
    bool model_rendered_ = false;
    BrandingImage branding_;
    ID2D1Factory* factory_ = nullptr;
    IDWriteFactory* write_factory_ = nullptr;
    IDWriteTextFormat* caption_ = nullptr;
    ID2D1HwndRenderTarget* window_target_ = nullptr;
    ID2D1SolidColorBrush* window_brush_ = nullptr;
};

class SplashWindow {
public:
    SplashWindow(bool english, UINT dpi) : english_(english), dpi_(dpi) {}
    bool closed() const { return closed_; }
    bool rendered() const { return rendered_; }
    bool layered_ready() const { return layered_ready_; }
    bool opacity_transition_observed() const { return opacity_transition_observed_; }
    void render_hidden_test_frame() {
        rendered_ = graphics_.paint(hwnd_, dpi_, english_, 410.0f);
    }

    void update_opacity() {
        const auto elapsed = static_cast<float>(GetTickCount64() - started_);
        const auto fade = std::clamp(std::min(elapsed / 150.0f,
            (static_cast<float>(splash_duration_ms) - elapsed) / 150.0f), 0.0f, 1.0f);
        const auto alpha = static_cast<BYTE>(std::lround(fade * 255.0f));
        if (SetLayeredWindowAttributes(hwnd_, 0, alpha, LWA_ALPHA)) {
            layered_ready_ = true;
            if (alpha > 0 && alpha < 255) opacity_transition_observed_ = true;
        }
    }

    static LRESULT CALLBACK procedure(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
        auto* state = reinterpret_cast<SplashWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            state = static_cast<SplashWindow*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
            state->hwnd_ = hwnd;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
            return TRUE;
        }
        if (!state) return DefWindowProcW(hwnd, message, wparam, lparam);
        switch (message) {
        case WM_CREATE: {
            if (!state->graphics_.initialize()) return -1;
            const BOOL dark = TRUE;
            DwmSetWindowAttribute(hwnd, 20, &dark, sizeof(dark));
            const DWORD rounded = 2;
            DwmSetWindowAttribute(hwnd, 33, &rounded, sizeof(rounded));
            RECT client{};
            GetClientRect(hwnd, &client);
            const auto radius = MulDiv(23, state->dpi_, 96) * 2;
            const auto region = CreateRoundRectRgn(0, 0, client.right + 1, client.bottom + 1,
                                                   radius, radius);
            if (region && !SetWindowRgn(hwnd, region, TRUE)) DeleteObject(region);
            state->started_ = GetTickCount64();
            state->update_opacity();
            SetTimer(hwnd, splash_timer, 16, nullptr);
            return 0;
        }
        case WM_ERASEBKGND: return 1;
        case WM_PAINT: {
            PAINTSTRUCT paint{};
            BeginPaint(hwnd, &paint);
            state->rendered_ = state->graphics_.paint(hwnd, state->dpi_, state->english_,
                static_cast<float>(GetTickCount64() - state->started_));
            EndPaint(hwnd, &paint);
            return 0;
        }
        case WM_TIMER:
            if (wparam == splash_timer) {
                if (GetTickCount64() - state->started_ >= splash_duration_ms) DestroyWindow(hwnd);
                else {
                    state->update_opacity();
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
            }
            return 0;
        case WM_LBUTTONDOWN:
        case WM_KEYDOWN:
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            KillTimer(hwnd, splash_timer);
            state->closed_ = true;
            state->hwnd_ = nullptr;
            return 0;
        default: return DefWindowProcW(hwnd, message, wparam, lparam);
        }
    }

private:
    HWND hwnd_ = nullptr;
    SplashGraphics graphics_;
    bool english_ = false;
    UINT dpi_ = 96;
    ULONGLONG started_ = 0;
    bool rendered_ = false;
    bool closed_ = false;
    bool layered_ready_ = false;
    bool opacity_transition_observed_ = false;
};
} // namespace

bool show_startup_splash(HINSTANCE instance, HWND owner, bool english, bool visible,
                         bool skip_for_test) {
    const auto dpi = win_compat::window_dpi(owner);
    const auto width = MulDiv(static_cast<int>(splash_width), dpi, 96);
    const auto height = MulDiv(static_cast<int>(splash_height), dpi, 96);
    MONITORINFO monitor{sizeof(monitor)};
    if (!GetMonitorInfoW(MonitorFromWindow(owner, MONITOR_DEFAULTTONEAREST), &monitor)) return false;
    const auto x = monitor.rcWork.left + (monitor.rcWork.right - monitor.rcWork.left - width) / 2;
    const auto y = monitor.rcWork.top + (monitor.rcWork.bottom - monitor.rcWork.top - height) / 2;
    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.hInstance = instance;
    window_class.lpfnWndProc = SplashWindow::procedure;
    window_class.lpszClassName = splash_class;
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    window_class.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1));
    if (!RegisterClassExW(&window_class)) return false;
    SplashWindow state(english, dpi);
    const auto hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_LAYERED, splash_class, L"Sempervirens",
        WS_POPUP, x, y, width, height, nullptr, nullptr, instance, &state);
    if (!hwnd) { UnregisterClassW(splash_class, instance); return false; }
    if (visible) {
        ShowWindow(hwnd, SW_SHOW);
        SetForegroundWindow(hwnd);
        UpdateWindow(hwnd);
    } else state.render_hidden_test_frame();
    if (skip_for_test) PostMessageW(hwnd, WM_KEYDOWN, VK_ESCAPE, 0);
    MSG message{};
    while (!state.closed() && GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    if (!state.closed()) DestroyWindow(hwnd);
    UnregisterClassW(splash_class, instance);
    return state.rendered() && state.closed() && state.layered_ready() &&
        (skip_for_test || state.opacity_transition_observed());
}

bool capture_startup_splash(const std::filesystem::path& output, bool english, float elapsed_ms,
                           bool benchmark) {
    SplashGraphics graphics;
    return graphics.initialize() && graphics.capture(output, english, elapsed_ms, benchmark);
}

} // namespace sempervirens
