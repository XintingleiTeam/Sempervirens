#pragma once

#include <windows.h>
#include <d2d1.h>
#include <wincodec.h>
#include <vector>
#include <cmath>
#include <algorithm>

namespace sempervirens {

// Full original wordmark for large brand placements; small shell icons stay A2.
// Preserve the supplied alpha silhouette, tint for the dark UI, and cache once.
class BrandingImage {
public:
    BrandingImage() = default;
    BrandingImage(const BrandingImage&) = delete;
    BrandingImage& operator=(const BrandingImage&) = delete;
    ~BrandingImage() { reset_target(); if (source_) source_->Release(); }

    void reset_target() {
        if (bitmap_) { bitmap_->Release(); bitmap_ = nullptr; }
        if (target_) { target_->Release(); target_ = nullptr; }
        bitmap_width_ = bitmap_height_ = 0;
    }

    bool draw(ID2D1RenderTarget* target, const D2D1_RECT_F& bounds, float opacity = 1.0f) {
        if (!target || (!source_ && !load())) return false;
        constexpr float aspect = 1765.0f / 861.0f;
        float width = bounds.right - bounds.left;
        float height = width / aspect;
        if (height > bounds.bottom - bounds.top) {
            height = bounds.bottom - bounds.top;
            width = height * aspect;
        }
        float dpi_x = 96, dpi_y = 96;
        target->GetDpi(&dpi_x, &dpi_y);
        const auto pixel_width = static_cast<UINT>(std::max(1.0f, std::ceil(width * dpi_x / 96)));
        const auto pixel_height = static_cast<UINT>(std::max(1.0f, std::ceil(height * dpi_y / 96)));
        if (target_ != target || bitmap_width_ != pixel_width || bitmap_height_ != pixel_height)
            reset_target();
        if (!bitmap_) {
            IWICImagingFactory* factory = nullptr;
            IWICBitmapClipper* clipper = nullptr;
            IWICBitmapScaler* scaler = nullptr;
            HRESULT result = CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
            WICRect crop{142, 142, 1765, 861};
            if (SUCCEEDED(result)) result = factory->CreateBitmapClipper(&clipper);
            if (SUCCEEDED(result)) result = clipper->Initialize(source_, &crop);
            if (SUCCEEDED(result)) result = factory->CreateBitmapScaler(&scaler);
            if (SUCCEEDED(result)) result = scaler->Initialize(clipper, pixel_width,
                pixel_height, WICBitmapInterpolationModeFant);
            if (SUCCEEDED(result)) result = target->CreateBitmapFromWicBitmap(scaler, nullptr, &bitmap_);
            if (scaler) scaler->Release();
            if (clipper) clipper->Release();
            if (factory) factory->Release();
            if (FAILED(result)) return false;
            target_ = target;
            target_->AddRef();
            bitmap_width_ = pixel_width;
            bitmap_height_ = pixel_height;
        }
        const auto destination = D2D1::RectF(bounds.left, bounds.top,
                                            bounds.left + width, bounds.top + height);
        target->DrawBitmap(bitmap_, &destination, opacity,
                           D2D1_BITMAP_INTERPOLATION_MODE_LINEAR, nullptr);
        return true;
    }

private:
    bool load() {
        const auto module = GetModuleHandleW(nullptr);
        const auto resource = FindResourceW(module, MAKEINTRESOURCEW(104), MAKEINTRESOURCEW(10));
        if (!resource) return false;
        const auto length = SizeofResource(module, resource);
        const auto loaded = LoadResource(module, resource);
        auto* data = static_cast<BYTE*>(LockResource(loaded));
        if (!data || !length) return false;
        IWICImagingFactory* factory = nullptr;
        IWICStream* stream = nullptr;
        IWICBitmapDecoder* decoder = nullptr;
        IWICBitmapFrameDecode* frame = nullptr;
        IWICFormatConverter* converter = nullptr;
        bool success = false;
        do {
            if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                        IID_PPV_ARGS(&factory)))) break;
            if (FAILED(factory->CreateStream(&stream))) break;
            if (FAILED(stream->InitializeFromMemory(data, length))) break;
            if (FAILED(factory->CreateDecoderFromStream(stream, nullptr, WICDecodeMetadataCacheOnLoad, &decoder))) break;
            if (FAILED(decoder->GetFrame(0, &frame))) break;
            if (FAILED(factory->CreateFormatConverter(&converter))) break;
            if (FAILED(converter->Initialize(frame, GUID_WICPixelFormat32bppPBGRA,
                       WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom))) break;
            UINT width = 0, height = 0;
            if (FAILED(converter->GetSize(&width, &height)) || width != 2048 || height != 1145) break;
            std::vector<BYTE> pixels(width * height * 4);
            if (FAILED(converter->CopyPixels(nullptr, width * 4,
                         static_cast<UINT>(pixels.size()), pixels.data()))) break;
            for (size_t i = 0; i < pixels.size(); i += 4)
                pixels[i] = pixels[i + 1] = pixels[i + 2] = pixels[i + 3];
            success = SUCCEEDED(factory->CreateBitmapFromMemory(width, height,
                GUID_WICPixelFormat32bppPBGRA, width * 4,
                static_cast<UINT>(pixels.size()), pixels.data(), &source_));
        } while (false);
        if (converter) converter->Release();
        if (frame) frame->Release();
        if (decoder) decoder->Release();
        if (stream) stream->Release();
        if (factory) factory->Release();
        return success;
    }

    IWICBitmap* source_ = nullptr;
    ID2D1RenderTarget* target_ = nullptr;
    ID2D1Bitmap* bitmap_ = nullptr;
    UINT bitmap_width_ = 0;
    UINT bitmap_height_ = 0;
};
}
