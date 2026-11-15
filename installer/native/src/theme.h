#pragma once
// 主题：与主程序 FanControl.UI.Cpp 的调色板保持一致。
#include <d2d1.h>
#include <dwrite.h>

namespace fc {

D2D1_COLOR_F Col(unsigned rgb, float alpha = 1.0f);

struct Theme {
    D2D1_COLOR_F windowBg;
    D2D1_COLOR_F cardBg;
    D2D1_COLOR_F cardBorder;
    D2D1_COLOR_F divider;
    D2D1_COLOR_F textPrimary;
    D2D1_COLOR_F textSecondary;
    D2D1_COLOR_F textTertiary;
    D2D1_COLOR_F accent;
    D2D1_COLOR_F accentText;
    D2D1_COLOR_F accentHover;
    D2D1_COLOR_F controlBg;
    D2D1_COLOR_F controlBorder;
    D2D1_COLOR_F controlHover;
    D2D1_COLOR_F track;          // 开关/进度条底槽
    D2D1_COLOR_F danger;
    D2D1_COLOR_F dangerHover;
    D2D1_COLOR_F dangerText;
    D2D1_COLOR_F ok;
};

Theme MakeTheme(bool dark);
bool  SystemUsesLightTheme();

// 与主程序一致的字体组合：Segoe UI Variable Display/Text + Fluent 图标
struct Fonts {
    IDWriteTextFormat* display24 = nullptr;
    IDWriteTextFormat* title20 = nullptr;
    IDWriteTextFormat* h16 = nullptr;
    IDWriteTextFormat* body14 = nullptr;
    IDWriteTextFormat* strong14 = nullptr;
    IDWriteTextFormat* small12 = nullptr;
    IDWriteTextFormat* glyph = nullptr;

    bool Create(IDWriteFactory* dw);
    void Release();
};

}  // namespace fc
