#include "ui.h"

#include <algorithm>
#include <cmath>

namespace fc {

D2D1_RECT_F Rect(float x, float y, float w, float h) { return D2D1::RectF(x, y, x + w, y + h); }

bool Contains(const D2D1_RECT_F& r, float x, float y) {
    return x >= r.left && x <= r.right && y >= r.top && y <= r.bottom;
}

bool Gfx::Init(HWND hwnd, ID2D1Factory* d2d, IDWriteFactory* dw) {
    hwnd_ = hwnd;
    d2d_ = d2d;
    dw_ = dw;
    if (!fonts_.Create(dw_)) return false;
    theme_ = MakeTheme(!SystemUsesLightTheme());
    return CreateTarget();
}

void Gfx::Shutdown() {
    DropTarget();
    fonts_.Release();
    dw_ = nullptr;
    d2d_ = nullptr;
    hwnd_ = nullptr;
}

void Gfx::DropTarget() {
    for (auto& entry : brushes_) entry.second->Release();
    brushes_.clear();
    if (rt_) {
        rt_->Release();
        rt_ = nullptr;
    }
}

bool Gfx::CreateTarget() {
    if (!d2d_ || !hwnd_) return false;
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    const float scale = GetDpiForWindow(hwnd_) / 96.0f;
    dpi_ = scale > 0.0f ? scale : 1.0f;
    const D2D1_SIZE_U size = D2D1::SizeU(static_cast<UINT32>(std::max<LONG>(1, rc.right - rc.left)),
                                         static_cast<UINT32>(std::max<LONG>(1, rc.bottom - rc.top)));
    if (!rt_) {
        D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties();
        // 软件光栅：安装器只是几块卡片和一行进度条，硬件管线要多背几十 MB 显存和驱动线程。
        props.type = D2D1_RENDER_TARGET_TYPE_SOFTWARE;
        props.dpiX = 96.0f * dpi_;
        props.dpiY = 96.0f * dpi_;
        const D2D1_HWND_RENDER_TARGET_PROPERTIES hwndProps = D2D1::HwndRenderTargetProperties(hwnd_, size);
        if (FAILED(d2d_->CreateHwndRenderTarget(props, hwndProps, &rt_))) return false;
    } else {
        rt_->SetDpi(96.0f * dpi_, 96.0f * dpi_);
        rt_->Resize(size);
    }
    width_ = static_cast<float>(size.width) / dpi_;
    height_ = static_cast<float>(size.height) / dpi_;
    return true;
}

void Gfx::Resize(UINT pixelWidth, UINT pixelHeight) {
    if (!rt_) {
        CreateTarget();
        return;
    }
    const float scale = GetDpiForWindow(hwnd_) / 96.0f;
    dpi_ = scale > 0.0f ? scale : 1.0f;
    rt_->SetDpi(96.0f * dpi_, 96.0f * dpi_);
    rt_->Resize(D2D1::SizeU(std::max<UINT>(1, pixelWidth), std::max<UINT>(1, pixelHeight)));
    width_ = static_cast<float>(std::max<UINT>(1, pixelWidth)) / dpi_;
    height_ = static_cast<float>(std::max<UINT>(1, pixelHeight)) / dpi_;
}

bool Gfx::Begin(const D2D1_COLOR_F& clearColor) {
    if (!rt_ && !CreateTarget()) return false;
    rt_->BeginDraw();
    rt_->SetTransform(D2D1::IdentityMatrix());
    rt_->Clear(clearColor);
    return true;
}

bool Gfx::End() {
    if (!rt_) return false;
    const HRESULT hr = rt_->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET) {
        DropTarget();
        return CreateTarget();
    }
    return SUCCEEDED(hr);
}

ID2D1SolidColorBrush* Gfx::Brush(const D2D1_COLOR_F& color) {
    if (!rt_) return nullptr;
    const uint32_t key = (static_cast<uint32_t>(color.r * 255.0f + 0.5f) << 24) |
                         (static_cast<uint32_t>(color.g * 255.0f + 0.5f) << 16) |
                         (static_cast<uint32_t>(color.b * 255.0f + 0.5f) << 8) |
                         static_cast<uint32_t>(color.a * 255.0f + 0.5f);
    auto it = brushes_.find(key);
    if (it != brushes_.end()) return it->second;
    ID2D1SolidColorBrush* brush = nullptr;
    if (FAILED(rt_->CreateSolidColorBrush(color, &brush)) || !brush) return nullptr;
    brushes_.emplace(key, brush);
    return brush;
}

void Gfx::FillRect(const D2D1_RECT_F& r, const D2D1_COLOR_F& color) {
    if (ID2D1SolidColorBrush* b = Brush(color)) rt_->FillRectangle(r, b);
}

void Gfx::FillRounded(const D2D1_RECT_F& r, float radius, const D2D1_COLOR_F& color) {
    if (ID2D1SolidColorBrush* b = Brush(color)) rt_->FillRoundedRectangle(D2D1::RoundedRect(r, radius, radius), b);
}

void Gfx::StrokeRounded(const D2D1_RECT_F& r, float radius, const D2D1_COLOR_F& color, float width) {
    if (ID2D1SolidColorBrush* b = Brush(color)) rt_->DrawRoundedRectangle(D2D1::RoundedRect(r, radius, radius), b, width);
}

void Gfx::DrawLine(float x1, float y1, float x2, float y2, const D2D1_COLOR_F& color, float width) {
    if (ID2D1SolidColorBrush* b = Brush(color)) rt_->DrawLine(D2D1::Point2F(x1, y1), D2D1::Point2F(x2, y2), b, width);
}

void Gfx::FillCircle(float cx, float cy, float radius, const D2D1_COLOR_F& color) {
    if (ID2D1SolidColorBrush* b = Brush(color)) rt_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), radius, radius), b);
}

void Gfx::StrokeCircle(float cx, float cy, float radius, const D2D1_COLOR_F& color, float width) {
    if (ID2D1SolidColorBrush* b = Brush(color))
        rt_->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), radius, radius), b, width);
}

void Gfx::FillEllipse(float cx, float cy, float rx, float ry, const D2D1_COLOR_F& color) {
    if (ID2D1SolidColorBrush* b = Brush(color)) rt_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), rx, ry), b);
}

void Gfx::Text(const std::wstring& text, const D2D1_RECT_F& box, IDWriteTextFormat* format,
               const D2D1_COLOR_F& color, DWRITE_TEXT_ALIGNMENT align, DWRITE_PARAGRAPH_ALIGNMENT valign) {
    if (!rt_ || !format || text.empty()) return;
    ID2D1SolidColorBrush* b = Brush(color);
    if (!b) return;
    format->SetTextAlignment(align);
    format->SetParagraphAlignment(valign);
    rt_->DrawTextW(text.c_str(), static_cast<UINT32>(text.size()), format, box, b, D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

float Gfx::Measure(const std::wstring& text, IDWriteTextFormat* format) {
    if (!dw_ || !format || text.empty()) return 0.0f;
    IDWriteTextLayout* layout = nullptr;
    if (FAILED(dw_->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()), format, 4096.0f, 4096.0f,
                                     &layout)) ||
        !layout) {
        return 0.0f;
    }
    DWRITE_TEXT_METRICS metrics{};
    layout->GetMetrics(&metrics);
    layout->Release();
    return metrics.widthIncludingTrailingWhitespace;
}

void Gfx::PushClip(const D2D1_RECT_F& r) {
    if (rt_) rt_->PushAxisAlignedClip(r, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
}

void Gfx::PopClip() {
    if (rt_) rt_->PopAxisAlignedClip();
}

}  // namespace fc
