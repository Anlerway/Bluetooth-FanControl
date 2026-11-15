#pragma once
// 轻量 Direct2D 渲染层：负责设备资源、画刷缓存、文字与基础图形，不含任何布局逻辑。
#include <d2d1.h>
#include <dwrite.h>
#include <windows.h>
#include <wincodec.h>

#include <string>
#include <unordered_map>

#include "theme.h"

namespace fc {

D2D1_RECT_F Rect(float x, float y, float w, float h);
bool        Contains(const D2D1_RECT_F& r, float x, float y);

class Gfx {
public:
    bool Init(HWND hwnd, ID2D1Factory* d2d, IDWriteFactory* dw);
    void Shutdown();
    void Resize(UINT pixelWidth, UINT pixelHeight);
    // 整体缩放系数：1.0 表示 1 DIP = 1/96 英寸；小于 1 会把整套界面按比例缩小。
    void  SetUiScale(float scale) { uiScale_ = scale > 0.0f ? scale : 1.0f; }
    float scale() const { return dpi_ * uiScale_; }

    bool Begin(const D2D1_COLOR_F& clearColor);
    bool End();

    Fonts&             fonts() { return fonts_; }
    const Theme&       theme() const { return theme_; }
    void               SetTheme(const Theme& theme) { theme_ = theme; }
    ID2D1HwndRenderTarget* target() { return rt_; }
    float              dpi() const { return dpi_; }
    float              width() const { return width_; }    // 逻辑像素（DIP）
    float              height() const { return height_; }

    void FillRect(const D2D1_RECT_F& r, const D2D1_COLOR_F& color);
    void FillRounded(const D2D1_RECT_F& r, float radius, const D2D1_COLOR_F& color);
    void StrokeRounded(const D2D1_RECT_F& r, float radius, const D2D1_COLOR_F& color, float width = 1.0f);
    void DrawLine(float x1, float y1, float x2, float y2, const D2D1_COLOR_F& color, float width = 1.0f);
    void FillCircle(float cx, float cy, float radius, const D2D1_COLOR_F& color);
    void StrokeCircle(float cx, float cy, float radius, const D2D1_COLOR_F& color, float width = 1.5f);
    void FillEllipse(float cx, float cy, float rx, float ry, const D2D1_COLOR_F& color);

    // 应用图标：LoadIcon 只记住句柄，首次绘制时才经 WIC 转成 D2D 位图（设备丢失后自动重建）。
    void LoadIcon(HICON icon);
    void DrawIcon(const D2D1_RECT_F& r);
    bool hasIcon() const { return icon_ != nullptr; }
    long iconError() const { return static_cast<long>(iconError_); }

    void Text(const std::wstring& text, const D2D1_RECT_F& box, IDWriteTextFormat* format,
              const D2D1_COLOR_F& color, DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING,
              DWRITE_PARAGRAPH_ALIGNMENT valign = DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    float Measure(const std::wstring& text, IDWriteTextFormat* format);

    void PushClip(const D2D1_RECT_F& r);
    void PopClip();

private:
    ID2D1SolidColorBrush* Brush(const D2D1_COLOR_F& color);
    void                  DropTarget();
    bool                  CreateTarget();
    bool                  BuildIconBitmap();

    HWND                                    hwnd_ = nullptr;
    ID2D1Factory*                           d2d_ = nullptr;
    IDWriteFactory*                         dw_ = nullptr;
    ID2D1HwndRenderTarget*                  rt_ = nullptr;
    IWICImagingFactory*                     wic_ = nullptr;
    ID2D1Bitmap*                            icon_ = nullptr;
    HICON                                   iconSrc_ = nullptr;
    HRESULT                                 iconError_ = S_OK;
    std::unordered_map<uint32_t, ID2D1SolidColorBrush*> brushes_;
    Fonts                                   fonts_;
    Theme                                   theme_{};
    float                                   dpi_ = 1.0f;
    float                                   uiScale_ = 1.0f;
    float                                   width_ = 0.0f;
    float                                   height_ = 0.0f;
};

}  // namespace fc
