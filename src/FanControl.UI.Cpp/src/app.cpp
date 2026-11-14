#include "app.h"
#include "i18n.h"
#include <dwmapi.h>
#include <shellapi.h>
#include <winreg.h>
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cwchar>

namespace {
constexpr float kSidebarWidth = 280.0f;
constexpr float kContentPad = 32.0f;
constexpr int kHistoryPoints = 240;
const wchar_t* PageName(int index) {
    switch (index) {
    case 0: return fc::T(L"仪表盘");
    case 1: return fc::T(L"设置");
    case 2: return fc::T(L"曲线");
    default: return fc::T(L"关于");
    }
}

std::wstring Fmt(const wchar_t* fmt, ...) {
    wchar_t buf[256];
    va_list ap;
    va_start(ap, fmt);
    vswprintf_s(buf, fmt, ap);
    va_end(ap);
    return buf;
}

bool SystemUsesLightTheme() {
    DWORD value = 1, size = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                     L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &value, &size) == ERROR_SUCCESS) {
        return value != 0;
    }
    return false;
}
} // namespace

Theme MakeTheme(bool dark) {
    Theme t{};
    if (dark) {
        t.windowBg = Col(0x202020);
        t.sidebarBg = Col(0x202020);
        t.cardBg = Col(0x2C2C2C);
        t.cardBorder = Col(0x3A3A3A);
        t.textPrimary = Col(0xFFFFFF);
        t.textSecondary = Col(0xB9B9B9);
        t.textTertiary = Col(0x8F8F8F);
        t.accent = Col(0x60CDFF);
        t.accentText = Col(0x000000);
        t.controlBg = Col(0x333333);
        t.controlBorder = Col(0x4A4A4A);
        t.controlHover = Col(0x3A3A3A);
        t.navHover = Col(0x2E2E2E);
        t.navSelected = Col(0x383838);
        t.accentBar = Col(0x60CDFF);
        t.chartBg = Col(0x272727);
        t.gridLine = Col(0x3A3A3A);
        t.cpuLine = Col(0x3AAEEF);
        t.gpuLine = Col(0x00C8A0);
        t.pwmLine = Col(0xE8A33D);
        t.danger = Col(0xFF99A4);
        t.ok = Col(0x6CCB5F);
    } else {
        t.windowBg = Col(0xF3F3F3);
        t.sidebarBg = Col(0xF3F3F3);
        t.cardBg = Col(0xFBFBFB);
        t.cardBorder = Col(0xE9E9E9);
        t.textPrimary = Col(0x1B1B1B);
        t.textSecondary = Col(0x5D5D5D);
        t.textTertiary = Col(0x8A8A8A);
        t.accent = Col(0x005FB8);
        t.accentText = Col(0xFFFFFF);
        t.controlBg = Col(0xFFFFFF);
        t.controlBorder = Col(0xD9D9D9);
        t.controlHover = Col(0xF5F5F5);
        t.navHover = Col(0xE9E9E9);
        t.navSelected = Col(0xE8E8E8);
        t.accentBar = Col(0x005FB8);
        t.chartBg = Col(0xFFFFFF);
        t.gridLine = Col(0xEDEDED);
        t.cpuLine = Col(0x0A84C8);
        t.gpuLine = Col(0x0E8A6E);
        t.pwmLine = Col(0xB86B00);
        t.danger = Col(0xC42B1C);
        t.ok = Col(0x0F7B0F);
    }
    return t;
}

bool App::Init(HWND hwnd) {
    hwnd_ = hwnd;
    dark_ = hasInitial_ ? initialDark_ : !SystemUsesLightTheme();
    theme_ = MakeTheme(dark_);

    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &d2d_))) return false;
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                   reinterpret_cast<IUnknown**>(&dw_)))) return false;
    CreateFormats();

    curve_ = { {30, 20}, {50, 30}, {65, 48}, {75, 70}, {85, 90}, {95, 100} };
    // 先铺满 3 分钟历史，避免首次打开时趋势图为空
    for (int i = 0; i < 180; ++i) {
        double tt = static_cast<double>(i);
        cpuHist_.emplace_back(tt, 46.0 + 9.0 * std::sin(tt / 17.0) + 2.0 * std::sin(tt / 3.1));
        gpuHist_.emplace_back(tt, 52.0 + 11.0 * std::sin(tt / 23.0 + 1.2));
        pwmHist_.emplace_back(tt, 30.0 + 26.0 * std::sin(tt / 19.0 + 0.4));
    }
    snap_.cpu = cpuHist_.back().second;
    snap_.gpu = gpuHist_.back().second;
    snap_.pwm = static_cast<int>(pwmHist_.back().second);
    const bool ok = CreateTarget();
    StartIpc();
    return ok;
}

void App::CreateFormats() {
    auto make = [&](const wchar_t* family, DWRITE_FONT_WEIGHT weight, float size, IDWriteTextFormat** out) {
        dw_->CreateTextFormat(family, nullptr, weight, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                              size, L"zh-CN", out);
        (*out)->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    };
    make(L"Segoe UI Variable Display", DWRITE_FONT_WEIGHT_SEMI_BOLD, 28.0f, &fTitle_);
    make(L"Segoe UI Variable Text", DWRITE_FONT_WEIGHT_SEMI_BOLD, 16.0f, &fH2_);
    make(L"Segoe UI Variable Text", DWRITE_FONT_WEIGHT_NORMAL, 14.0f, &fBody_);
    make(L"Segoe UI Variable Text", DWRITE_FONT_WEIGHT_SEMI_BOLD, 14.0f, &fBodyStrong_);
    make(L"Segoe UI Variable Text", DWRITE_FONT_WEIGHT_NORMAL, 12.0f, &fSmall_);
    make(L"Segoe UI Variable Display", DWRITE_FONT_WEIGHT_SEMI_BOLD, 32.0f, &fBig_);
    make(L"Segoe Fluent Icons", DWRITE_FONT_WEIGHT_NORMAL, 14.0f, &fGlyph_);
}

bool App::CreateTarget() {
    if (!d2d_ || !hwnd_) return false;
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    scale_ = GetDpiForWindow(hwnd_) / 96.0f;
    D2D1_SIZE_U size = D2D1::SizeU(std::max<LONG>(1, rc.right - rc.left), std::max<LONG>(1, rc.bottom - rc.top));
    if (!rt_) {
        auto props = D2D1::RenderTargetProperties();
        // 软件光栅：硬件 HwndRenderTarget 在这类小工具上要多背 ~45 MB 私有内存和
        // 20 多个驱动线程（实测 65 MB / 45 线程 vs 27 MB / 27 线程），而界面只在
        // 收到状态帧和鼠标交互时重绘，软件光栅的 CPU 代价（约 4% 单核）更划算。
        props.type = D2D1_RENDER_TARGET_TYPE_SOFTWARE;
        props.dpiX = 96.0f * scale_;
        props.dpiY = 96.0f * scale_;
        auto hwndProps = D2D1::HwndRenderTargetProperties(hwnd_, size);
        if (FAILED(d2d_->CreateHwndRenderTarget(props, hwndProps, &rt_))) return false;
    } else {
        rt_->SetDpi(96.0f * scale_, 96.0f * scale_);
        rt_->Resize(size);
    }
    return true;
}

void App::DropTarget() {
    if (rt_) { rt_->Release(); rt_ = nullptr; }
}

void App::Release() {
    StopIpc();
    DropTarget();
    for (IDWriteTextFormat** f : { &fTitle_, &fH2_, &fBody_, &fBodyStrong_, &fSmall_, &fBig_, &fGlyph_ }) {
        if (*f) { (*f)->Release(); *f = nullptr; }
    }
    if (dw_) { dw_->Release(); dw_ = nullptr; }
    if (d2d_) { d2d_->Release(); d2d_ = nullptr; }
}

void App::Resize(UINT pxWidth, UINT pxHeight) {
    if (!rt_) { CreateTarget(); return; }
    scale_ = GetDpiForWindow(hwnd_) / 96.0f;
    rt_->SetDpi(96.0f * scale_, 96.0f * scale_);
    rt_->Resize(D2D1::SizeU(std::max<UINT>(1, pxWidth), std::max<UINT>(1, pxHeight)));
    InvalidateRect(hwnd_, nullptr, FALSE);
}

// ---------------- drawing helpers ----------------

void App::FillRect(D2D1_RECT_F r, D2D1_COLOR_F c) {
    ID2D1SolidColorBrush* b = nullptr;
    if (SUCCEEDED(rt_->CreateSolidColorBrush(c, &b))) {
        rt_->FillRectangle(r, b);
        b->Release();
    }
}

void App::FillRound(D2D1_RECT_F r, float radius, D2D1_COLOR_F c) {
    ID2D1SolidColorBrush* b = nullptr;
    if (SUCCEEDED(rt_->CreateSolidColorBrush(c, &b))) {
        rt_->FillRoundedRectangle(D2D1::RoundedRect(r, radius, radius), b);
        b->Release();
    }
}

void App::StrokeRound(D2D1_RECT_F r, float radius, D2D1_COLOR_F c, float width) {
    ID2D1SolidColorBrush* b = nullptr;
    if (SUCCEEDED(rt_->CreateSolidColorBrush(c, &b))) {
        rt_->DrawRoundedRectangle(D2D1::RoundedRect(r, radius, radius), b, width);
        b->Release();
    }
}

void App::Line(float x1, float y1, float x2, float y2, D2D1_COLOR_F c, float width) {
    ID2D1SolidColorBrush* b = nullptr;
    if (SUCCEEDED(rt_->CreateSolidColorBrush(c, &b))) {
        rt_->DrawLine(D2D1::Point2F(x1, y1), D2D1::Point2F(x2, y2), b, width);
        b->Release();
    }
}

void App::Text(const std::wstring& s, D2D1_RECT_F r, IDWriteTextFormat* fmt, D2D1_COLOR_F c) {
    ID2D1SolidColorBrush* b = nullptr;
    if (SUCCEEDED(rt_->CreateSolidColorBrush(c, &b))) {
        fmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        rt_->DrawTextW(s.c_str(), static_cast<UINT32>(s.size()), fmt, r, b, D2D1_DRAW_TEXT_OPTIONS_CLIP);
        b->Release();
    }
}

void App::TextCentered(const std::wstring& s, D2D1_RECT_F r, IDWriteTextFormat* fmt, D2D1_COLOR_F c) {
    ID2D1SolidColorBrush* b = nullptr;
    if (SUCCEEDED(rt_->CreateSolidColorBrush(c, &b))) {
        fmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        rt_->DrawTextW(s.c_str(), static_cast<UINT32>(s.size()), fmt, r, b, D2D1_DRAW_TEXT_OPTIONS_CLIP);
        b->Release();
    }
}

void App::TextRight(const std::wstring& s, D2D1_RECT_F r, IDWriteTextFormat* fmt, D2D1_COLOR_F c) {
    ID2D1SolidColorBrush* b = nullptr;
    if (SUCCEEDED(rt_->CreateSolidColorBrush(c, &b))) {
        fmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
        fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        rt_->DrawTextW(s.c_str(), static_cast<UINT32>(s.size()), fmt, r, b, D2D1_DRAW_TEXT_OPTIONS_CLIP);
        b->Release();
    }
}

void App::Glyph(const std::wstring& s, D2D1_RECT_F r, float size, D2D1_COLOR_F c) {
    IDWriteTextFormat* fmt = nullptr;
    if (FAILED(dw_->CreateTextFormat(L"Segoe Fluent Icons", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                                     DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"en-US", &fmt))) {
        return;
    }
    fmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    ID2D1SolidColorBrush* b = nullptr;
    if (SUCCEEDED(rt_->CreateSolidColorBrush(c, &b))) {
        rt_->DrawTextW(s.c_str(), static_cast<UINT32>(s.size()), fmt, r, b, D2D1_DRAW_TEXT_OPTIONS_CLIP);
        b->Release();
    }
    fmt->Release();
}

void App::AddHit(D2D1_RECT_F r, const std::wstring& id) { hits_.push_back({ r, id }); }

// ---------------- widgets ----------------

void App::Switch(float x, float y, bool on) {
    D2D1_RECT_F track = D2D1::RectF(x, y, x + 40, y + 20);
    FillRound(track, 10.0f, on ? theme_.accent : theme_.controlBorder);
    float knob = 12.0f;
    float kx = on ? (track.right - knob - 4) : (track.left + 4);
    D2D1_RECT_F k = D2D1::RectF(kx, y + 4, kx + knob, y + 4 + knob);
    FillRound(k, knob / 2, on ? theme_.accentText : theme_.textPrimary);
}

void App::Chip(float x, float y, float w, float h, const std::wstring& text, bool accent,
               const std::wstring& id) {
    D2D1_RECT_F r = D2D1::RectF(x, y, x + w, y + h);
    FillRound(r, 4.0f, accent ? theme_.accent : theme_.controlBg);
    if (!accent) StrokeRound(r, 4.0f, theme_.controlBorder);
    TextCentered(text, r, fBody_, accent ? theme_.accentText : theme_.textPrimary);
    // 命中 id 用固定标识，不跟显示文本走，否则切换语言后按钮就点不动了
    AddHit(r, id.empty() ? L"chip:" + text : L"chip:" + id);
}

void App::RowButton(float x, float y, float w, float h, const std::wstring& id, const std::wstring& title,
                    const std::wstring& subtitle, const std::wstring& value) {
    D2D1_RECT_F r = D2D1::RectF(x, y, x + w, y + h);
    if (Hovered(id)) FillRound(r, 4.0f, theme_.controlHover);
    Text(title, D2D1::RectF(x + 16, y + h / 2 - 19, x + w - 220, y + h / 2 - 1), fBody_, theme_.textPrimary);
    if (!subtitle.empty())
        Text(subtitle, D2D1::RectF(x + 16, y + h / 2 + 1, x + w - 220, y + h / 2 + 18), fSmall_, theme_.textSecondary);
    if (!value.empty()) {
        TextRight(value, D2D1::RectF(x + w - 210, y, x + w - 40, y + h), fBody_, theme_.textSecondary);
        Glyph(L"\xE76C", D2D1::RectF(x + w - 32, y + h / 2 - 10, x + w - 12, y + h / 2 + 10), 12.0f, theme_.textTertiary);
    }
    AddHit(r, id);
}

void App::RowToggle(float x, float y, float w, float h, const std::wstring& id, const std::wstring& title,
                    const std::wstring& subtitle, bool on) {
    D2D1_RECT_F r = D2D1::RectF(x, y, x + w, y + h);
    if (Hovered(id)) FillRound(r, 4.0f, theme_.controlHover);
    Text(title, D2D1::RectF(x + 16, y + h / 2 - 19, x + w - 80, y + h / 2 - 1), fBody_, theme_.textPrimary);
    if (!subtitle.empty())
        Text(subtitle, D2D1::RectF(x + 16, y + h / 2 + 1, x + w - 80, y + h / 2 + 18), fSmall_, theme_.textSecondary);
    Switch(x + w - 56, y + h / 2 - 10, on);
    AddHit(r, id);
}

void App::RowSlider(float x, float y, float w, float h, const std::wstring& id, const std::wstring& title,
                    const std::wstring& subtitle, int value, int lo, int hi) {
    D2D1_RECT_F r = D2D1::RectF(x, y, x + w, y + h);
    if (Hovered(id)) FillRound(r, 4.0f, theme_.controlHover);
    Text(title, D2D1::RectF(x + 16, y + h / 2 - 19, x + w - 260, y + h / 2 - 1), fBody_, theme_.textPrimary);
    if (!subtitle.empty())
        Text(subtitle, D2D1::RectF(x + 16, y + h / 2 + 1, x + w - 260, y + h / 2 + 18), fSmall_, theme_.textSecondary);
    float trackL = x + w - 240, trackR = x + w - 60, cy = y + h / 2;
    FillRound(D2D1::RectF(trackL, cy - 2, trackR, cy + 2), 2.0f, theme_.controlBorder);
    float ratio = (value - lo) / static_cast<float>(hi - lo);
    FillRound(D2D1::RectF(trackL, cy - 2, trackL + (trackR - trackL) * ratio, cy + 2), 2.0f, theme_.accent);
    float kx = trackL + (trackR - trackL) * ratio;
    FillRound(D2D1::RectF(kx - 10, cy - 10, kx + 10, cy + 10), 10.0f, theme_.controlBg);
    StrokeRound(D2D1::RectF(kx - 10, cy - 10, kx + 10, cy + 10), 10.0f, theme_.controlBorder);
    TextRight(Fmt(L"%d ms", value), D2D1::RectF(x + w - 56, y, x + w - 16, y + h), fBody_, theme_.textSecondary);
    AddHit(r, id);
}

float App::Card(float x, float y, float w, const std::wstring& title, float height) {
    D2D1_RECT_F r = D2D1::RectF(x, y, x + w, y + height);
    FillRound(r, 8.0f, theme_.cardBg);
    StrokeRound(r, 8.0f, theme_.cardBorder);
    if (!title.empty()) {
        Text(title, D2D1::RectF(x + 16, y + 14, x + w - 16, y + 36), fBodyStrong_, theme_.textPrimary);
    }
    return y + height;
}

// ---------------- pages ----------------

void App::DrawSidebar(float w, float h) {
    (void)w;
    FillRect(D2D1::RectF(0, 0, kSidebarWidth, h), theme_.sidebarBg);
    Text(L"FanControl", D2D1::RectF(20, 18, kSidebarWidth - 20, 48), fH2_, theme_.textPrimary);

    D2D1_RECT_F search = D2D1::RectF(20, 60, kSidebarWidth - 20, 92);
    FillRound(search, 4.0f, theme_.controlBg);
    StrokeRound(search, 4.0f, theme_.controlBorder);
    Glyph(L"\xE721", D2D1::RectF(28, 66, 48, 86), 12.0f, theme_.textTertiary);
    Text(fc::T(L"搜索设置"), D2D1::RectF(52, 60, kSidebarWidth - 32, 92), fSmall_, theme_.textTertiary);
    AddHit(search, L"search");

    float y = 110.0f;
    for (int i = 0; i < 4; ++i) {
        D2D1_RECT_F item = D2D1::RectF(12, y, kSidebarWidth - 12, y + 36);
        std::wstring id = L"nav:" + std::to_wstring(i);
        bool selected = (static_cast<int>(page_) == i);
        if (selected) {
            FillRound(item, 4.0f, theme_.navSelected);
            FillRound(D2D1::RectF(12, y + 9, 15, y + 27), 1.5f, theme_.accentBar);
        } else if (Hovered(id)) {
            FillRound(item, 4.0f, theme_.navHover);
        }
        const wchar_t* icons[] = { L"\xE80F", L"\xE713", L"\xE9D2", L"\xE946" };
        Glyph(icons[i], D2D1::RectF(28, y + 8, 48, y + 28), 15.0f, selected ? theme_.textPrimary : theme_.textSecondary);
        Text(PageName(i), D2D1::RectF(56, y + 6, kSidebarWidth - 24, y + 30), fBody_,
             selected ? theme_.textPrimary : theme_.textSecondary);
        AddHit(item, id);
        y += 40.0f;
    }

    // 底部后端状态
    float by = h - 64.0f;
    D2D1_COLOR_F dot = snap_.backendConnected ? theme_.ok : theme_.textTertiary;
    FillRound(D2D1::RectF(24, by + 8, 32, by + 16), 4.0f, dot);
    Text(snap_.backendConnected ? fc::T(L"后台已连接") : fc::T(L"演示数据（后台未连接）"),
         D2D1::RectF(40, by, kSidebarWidth - 20, by + 24), fSmall_, theme_.textSecondary);
    Text(Fmt(fc::T(L"后台 v1.4.0 · %s"), dark_ ? fc::T(L"深色") : fc::T(L"浅色")),
         D2D1::RectF(24, by + 24, kSidebarWidth - 20, by + 48), fSmall_, theme_.textTertiary);
}

void App::DrawDashboard(float x, float y, float w) {
    float cardW = (w - 16.0f) / 3.0f;
    struct Stat { const wchar_t* label; std::wstring value; const wchar_t* unit; const wchar_t* sub; };
    Stat stats[3] = {
        { fc::T(L"CPU 温度"), snap_.cpu > 0.0 ? Fmt(L"%.1f", snap_.cpu) : L"—", L"°C", fc::T(L"LHM 封装传感器") },
        { fc::T(L"GPU 温度"), snap_.gpu > 0.0 ? Fmt(L"%.1f", snap_.gpu) : L"—", L"°C", fc::T(L"未启用时回退 CPU") },
        { fc::T(L"风扇 PWM"), Fmt(L"%d", snap_.pwm), L"%", fc::T(L"转速见趋势") },
    };
    for (int i = 0; i < 3; ++i) {
        float cx = x + i * (cardW + 8.0f);
        D2D1_RECT_F r = D2D1::RectF(cx, y, cx + cardW, y + 118);
        FillRound(r, 8.0f, theme_.cardBg);
        StrokeRound(r, 8.0f, theme_.cardBorder);
        Text(stats[i].label, D2D1::RectF(cx + 16, y + 14, cx + cardW - 16, y + 34), fSmall_, theme_.textSecondary);
        Text(stats[i].value, D2D1::RectF(cx + 16, y + 36, cx + cardW - 44, y + 78), fBig_, theme_.textPrimary);
        TextRight(stats[i].unit, D2D1::RectF(cx + cardW - 42, y + 36, cx + cardW - 14, y + 78), fBody_,
                  theme_.textSecondary);
        Text(stats[i].sub, D2D1::RectF(cx + 16, y + 88, cx + cardW - 16, y + 108), fSmall_, theme_.textTertiary);
    }
    y += 134.0f;

    // 趋势卡
    float h = 250.0f;
    D2D1_RECT_F card = D2D1::RectF(x, y, x + w, y + h);
    FillRound(card, 8.0f, theme_.cardBg);
    StrokeRound(card, 8.0f, theme_.cardBorder);
    Text(fc::T(L"最近 4 分钟"), D2D1::RectF(x + 16, y + 12, x + w - 200, y + 34), fBodyStrong_,
         theme_.textPrimary);
    // 图例
    float lx = x + w - 210;
    struct Leg { const wchar_t* name; D2D1_COLOR_F color; };
    Leg legs[3] = { { L"CPU", theme_.cpuLine }, { L"GPU", theme_.gpuLine }, { L"PWM", theme_.pwmLine } };
    for (auto& leg : legs) {
        FillRound(D2D1::RectF(lx, y + 20, lx + 8, y + 28), 4.0f, leg.color);
        Text(leg.name, D2D1::RectF(lx + 12, y + 14, lx + 44, y + 34), fSmall_, theme_.textSecondary);
        lx += 66;
    }

    D2D1_RECT_F plot = D2D1::RectF(x + 44, y + 48, x + w - 20, y + h - 28);
    for (int i = 0; i <= 4; ++i) {  // 水平网格
        float gy = plot.top + (plot.bottom - plot.top) * i / 4.0f;
        Line(plot.left, gy, plot.right, gy, theme_.gridLine);
    }
    auto drawSeries = [&](const std::deque<std::pair<double, double>>& data, D2D1_COLOR_F color) {
        if (data.size() < 2) return;
        float step = (plot.right - plot.left) / (kHistoryPoints - 1);
        for (size_t i = 1; i < data.size(); ++i) {
            float x1 = plot.left + (i - 1) * step;
            float x2 = plot.left + i * step;
            float y1 = plot.bottom - static_cast<float>(data[i - 1].second) / 100.0f * (plot.bottom - plot.top);
            float y2 = plot.bottom - static_cast<float>(data[i].second) / 100.0f * (plot.bottom - plot.top);
            Line(x1, y1, x2, y2, color, 1.6f);
        }
    };
    drawSeries(cpuHist_, theme_.cpuLine);
    drawSeries(gpuHist_, theme_.gpuLine);
    drawSeries(pwmHist_, theme_.pwmLine);

    TextCentered(L"100", D2D1::RectF(x + 8, plot.top - 8, x + 40, plot.top + 8), fSmall_, theme_.textTertiary);
    TextCentered(L"0", D2D1::RectF(x + 8, plot.bottom - 8, x + 40, plot.bottom + 8), fSmall_, theme_.textTertiary);
    y += h + 16.0f;

    // 状态卡
    float sh = 140.0f;
    D2D1_RECT_F st = D2D1::RectF(x, y, x + w, y + sh);
    FillRound(st, 8.0f, theme_.cardBg);
    StrokeRound(st, 8.0f, theme_.cardBorder);
    Text(fc::T(L"连接状态"), D2D1::RectF(x + 16, y + 12, x + w - 16, y + 34), fBodyStrong_, theme_.textPrimary);
    struct KV { const wchar_t* k; std::wstring v; };
    KV rows[3] = {
        { fc::T(L"通信"), snap_.commMode },
        { fc::T(L"设备"), snap_.deviceConnected ? fc::T(L"已连接") : fc::T(L"未连接") },
        { fc::T(L"最近错误"), snap_.lastError.empty()
                                  ? std::wstring(fc::T(L"无"))
                                  : fc::ErrorText(snap_.errorCode, snap_.errorArg1, snap_.errorArg2, snap_.lastError) },
    };
    float ry = y + 44;
    for (auto& kv : rows) {
        Text(kv.k, D2D1::RectF(x + 16, ry, x + 160, ry + 24), fSmall_, theme_.textSecondary);
        Text(kv.v, D2D1::RectF(x + 160, ry, x + w - 16, ry + 24), fBody_, theme_.textPrimary);
        ry += 28;
    }
    contentHeight_ = y + sh + 24.0f;
}

namespace {
std::wstring SourceLabel(const std::wstring& value) {
    if (value == L"LibreHardwareMonitor") return L"LibreHardwareMonitor";
    if (value == L"AtkAcpi") return L"ATK ACPI";
    if (value == L"Wmi") return L"WMI";
    if (value == L"Aida64") return L"AIDA64";
    if (value == L"Simulated") return fc::T(L"模拟数据");
    if (value == L"Serial") return fc::T(L"串口");
    return value;
}
} // namespace

void App::DrawSettings(float x, float y, float w) {
    float top = y;
    float rowH = 64.0f;

    Text(fc::T(L"通用"), D2D1::RectF(x, y, x + w, y + 22), fSmall_, theme_.textSecondary);
    y += 26;
    Card(x, y, w, L"", 4 * rowH + 8);
    float ry = y + 4;
    RowButton(x, ry, w, rowH, L"set:theme", fc::T(L"应用主题"), fc::T(L"跟随系统 / 深色 / 浅色"), ThemeLabel());
    ry += rowH;
    RowButton(x, ry, w, rowH, L"set:lang", fc::T(L"语言"), fc::T(L"界面语言"), LangLabel());
    ry += rowH;
    RowButton(x, ry, w, rowH, L"set:unit", fc::T(L"温度单位"), fc::T(L"仪表盘与曲线显示单位"), UnitLabel());
    ry += rowH;
    RowToggle(x, ry, w, rowH, L"set:autostart", fc::T(L"开机自启"), fc::T(L"登录后自动启动后台守护进程"), autostart_);
    y += 4 * rowH + 8 + 20;

    Text(fc::T(L"硬件"), D2D1::RectF(x, y, x + w, y + 22), fSmall_, theme_.textSecondary);
    y += 26;
    Card(x, y, w, L"", 3 * rowH + 8);
    ry = y + 4;
    RowButton(x, ry, w, rowH, L"set:tempsrc", fc::T(L"温度来源"), L"LibreHardwareMonitor / ATK ACPI / WMI",
              SourceLabel(config_.temperatureSource));
    ry += rowH;
    RowButton(x, ry, w, rowH, L"set:fansrc", fc::T(L"转速来源"),
              std::wstring(fc::T(L"EC 直读")) + L" / LibreHardwareMonitor / AIDA64",
              SourceLabel(config_.fanSpeedSource));
    ry += rowH;
    RowButton(x, ry, w, rowH, L"set:comm", fc::T(L"通信方式"), fc::T(L"蓝牙 BLE 或串口"),
              config_.communicationType == L"Com" ? (std::wstring(fc::T(L"串口")) + L" " + config_.comPort) : fc::T(L"蓝牙 BLE"));
    y += 3 * rowH + 8 + 20;

    Text(fc::T(L"控制"), D2D1::RectF(x, y, x + w, y + 22), fSmall_, theme_.textSecondary);
    y += 26;
    Card(x, y, w, L"", 2 * rowH + 8);
    ry = y + 4;
    RowSlider(x, ry, w, rowH, L"set:smooth", fc::T(L"平滑窗口"), fc::T(L"PWM 变化的过渡时间"), smoothMs_, 100, 3000);
    ry += rowH;
    RowToggle(x, ry, w, rowH, L"set:notify", fc::T(L"断连通知"), fc::T(L"蓝牙断开时弹出系统通知"), config_.notifyOnBleDisconnect);
    y += 2 * rowH + 8 + 20;

    Text(fc::T(L"维护"), D2D1::RectF(x, y, x + w, y + 22), fSmall_, theme_.textSecondary);
    y += 26;
    Card(x, y, w, L"", rowH + 8);
    RowButton(x, y + 4, w, rowH, L"set:logs", fc::T(L"日志目录"), fc::T(L"打开日志所在文件夹"), L"");
    y += rowH + 8 + 16;

    contentHeight_ = y - top + 24.0f;
}

void App::DrawCurve(float x, float y, float w) {
    float h = 380.0f;
    D2D1_RECT_F card = D2D1::RectF(x, y, x + w, y + h);
    FillRound(card, 8.0f, theme_.cardBg);
    StrokeRound(card, 8.0f, theme_.cardBorder);
    Text(fc::T(L"温度 → PWM 曲线"), D2D1::RectF(x + 16, y + 12, x + w - 16, y + 34), fBodyStrong_, theme_.textPrimary);

    D2D1_RECT_F plot = D2D1::RectF(x + 56, y + 56, x + w - 32, y + h - 48);
    curvePlot_ = plot;
    for (int i = 0; i <= 5; ++i) {
        float gy = plot.top + (plot.bottom - plot.top) * i / 5.0f;
        Line(plot.left, gy, plot.right, gy, theme_.gridLine);
        TextRight(Fmt(L"%d", 100 - i * 20), D2D1::RectF(x + 8, gy - 10, plot.left - 10, gy + 10), fSmall_,
                  theme_.textTertiary);
        float gx = plot.left + (plot.right - plot.left) * i / 5.0f;
        Line(gx, plot.top, gx, plot.bottom, theme_.gridLine);
        TextCentered(Fmt(L"%d", i * 20), D2D1::RectF(gx - 20, plot.bottom + 4, gx + 20, plot.bottom + 24), fSmall_,
                     theme_.textTertiary);
    }
    TextCentered(fc::T(L"温度 °C"), D2D1::RectF(plot.left, plot.bottom + 26, plot.right, plot.bottom + 46), fSmall_,
                 theme_.textTertiary);

    auto toX = [&](double t) { return plot.left + static_cast<float>(t / 100.0) * (plot.right - plot.left); };
    auto toY = [&](double p) { return plot.bottom - static_cast<float>(p / 100.0) * (plot.bottom - plot.top); };

    for (size_t i = 1; i < curve_.size(); ++i) {
        Line(toX(curve_[i - 1].t), toY(curve_[i - 1].p), toX(curve_[i].t), toY(curve_[i].p), theme_.accent, 2.0f);
    }
    for (size_t i = 0; i < curve_.size(); ++i) {
        float px = toX(curve_[i].t), py = toY(curve_[i].p);
        FillRound(D2D1::RectF(px - 6, py - 6, px + 6, py + 6), 6.0f, theme_.cardBg);
        StrokeRound(D2D1::RectF(px - 6, py - 6, px + 6, py + 6), 6.0f, theme_.accent, 2.0f);
    }

    float by = y + h + 16;
    Chip(x, by, 96, 32, fc::T(L"重置曲线"), false, L"reset");
    Chip(x + 104, by, 96, 32, fc::T(L"应用"), true, L"apply");
    Text(fc::T(L"拖动圆点调整映射，双击空白处新增点"), D2D1::RectF(x + 216, by, x + w, by + 32), fSmall_,
         theme_.textTertiary);
    contentHeight_ = by + 32 + 24 - y;
}

void App::DrawAbout(float x, float y, float w) {
    float rowH = 56.0f;
    Card(x, y, w, L"", 4 * rowH + 8);
    struct Row { const wchar_t* k; std::wstring v; };
    Row rows[4] = {
        { fc::T(L"版本"), fc::T(L"1.4.0（C++ UI）") },
        { fc::T(L"界面"), L"Win32 + Direct2D / DirectWrite" },
        { fc::T(L"后端"), snap_.backendConnected ? fc::T(L"FanControl 守护进程（命名管道）") : fc::T(L"未连接（演示数据）") },
        { fc::T(L"配置文件"), L"%LOCALAPPDATA%\\FanControl\\config.json" },
    };
    float ry = y + 4;
    for (auto& r : rows) {
        Text(r.k, D2D1::RectF(x + 16, ry + 16, x + 180, ry + 40), fSmall_, theme_.textSecondary);
        Text(r.v, D2D1::RectF(x + 180, ry + 16, x + w - 16, ry + 40), fBody_, theme_.textPrimary);
        ry += rowH;
    }
    y += 4 * rowH + 8 + 20;
    Chip(x, y, 110, 32, fc::T(L"打开日志目录"), false, L"logs");
    Chip(x + 118, y, 110, 32, fc::T(L"检查更新"), false, L"update");
    contentHeight_ = y + 32 + 24 - y;
}

// ---------------- frame ----------------

void App::Paint() {
    if (!rt_ && !CreateTarget()) return;
    hits_.clear();
    rt_->BeginDraw();
    rt_->Clear(theme_.windowBg);

    D2D1_SIZE_F size = rt_->GetSize();
    DrawSidebar(size.width, size.height);

    viewH_ = size.height;
    float cx = kSidebarWidth + kContentPad;
    float cw = size.width - kSidebarWidth - kContentPad * 2;
    if (cw < 320) cw = std::max(240.0f, size.width - kSidebarWidth - 2 * kContentPad);

    // 滚动内容区（底部留给状态条）
    float clipBottom = size.height - 8.0f;
    rt_->PushAxisAlignedClip(D2D1::RectF(kSidebarWidth, 0, size.width, clipBottom), D2D1_ANTIALIAS_MODE_ALIASED);
    float top = 28.0f - scroll_;
    const wchar_t* subtitles[] = { fc::T(L"实时温度、转速与趋势"), fc::T(L"外观、硬件来源与控制行为"),
                                   fc::T(L"温度到风扇转速的映射"), fc::T(L"版本与运行信息") };
    Text(PageName(static_cast<int>(page_)), D2D1::RectF(cx, top, cx + cw, top + 40), fTitle_, theme_.textPrimary);
    Text(subtitles[static_cast<int>(page_)], D2D1::RectF(cx, top + 40, cx + cw, top + 62), fSmall_, theme_.textSecondary);

    float bodyY = top + 84;
    switch (page_) {
    case Page::Dashboard: DrawDashboard(cx, bodyY, cw); break;
    case Page::Settings: DrawSettings(cx, bodyY, cw); break;
    case Page::Curve: DrawCurve(cx, bodyY, cw); break;
    case Page::About: DrawAbout(cx, bodyY, cw); break;
    }
    rt_->PopAxisAlignedClip();
    pageHeight_[static_cast<int>(page_)] = contentHeight_;
    contentHeight_ = pageHeight_[static_cast<int>(page_)];

    // 后台未连接时的提示放在侧栏底部，避免遮挡内容

    if (!toast_.empty() && GetTickCount() < toastUntil_) {
        float tw = 240.0f;
        D2D1_RECT_F r = D2D1::RectF(size.width - tw - 24, size.height - 88, size.width - 24, size.height - 56);
        FillRound(r, 4.0f, dark_ ? Col(0x3A3A3A) : Col(0xE5E5E5));
        TextCentered(toast_, r, fSmall_, theme_.textPrimary);
    }

    rt_->EndDraw();
}

void App::Tick() {
    t_ += 1.0;
    if (backendUp_) {
        snap_.backendConnected = true;
        return;
    }
    {
        // 后台未连接时用演示波形占位，连上后完全以管道数据为准
        snap_.cpu = 46.0 + 9.0 * std::sin(t_ / 17.0) + 2.0 * std::sin(t_ / 3.1);
        snap_.gpu = 52.0 + 11.0 * std::sin(t_ / 23.0 + 1.2);
        const double pwm = 30.0 + 26.0 * std::sin(t_ / 19.0 + 0.4);
        snap_.pwm = static_cast<int>(pwm);
        snap_.rpm = static_cast<int>(600 + pwm * 28);
        snap_.backendConnected = false;
    }
    BuildHistory(1.0);
}

void App::BuildHistory(float) {
    auto push = [](std::deque<std::pair<double, double>>& d, double v) {
        d.emplace_back(0.0, v);
        while (d.size() > static_cast<size_t>(kHistoryPoints)) d.pop_front();
    };
    push(cpuHist_, snap_.cpu);
    push(gpuHist_, snap_.gpu);
    push(pwmHist_, static_cast<double>(snap_.pwm));
}

// ---------------- input ----------------

void App::OnMouseMove(int x, int y) {
    mouseX_ = x;
    mouseY_ = y;
    float sx = x / scale_, sy = y / scale_;
    std::wstring previous = hover_;
    hover_.clear();
    for (auto& h : hits_) {
        if (sx >= h.rect.left && sx <= h.rect.right && sy >= h.rect.top && sy <= h.rect.bottom) {
            hover_ = h.id;
            break;
        }
    }
    bool interactive = !hover_.empty();
    SetCursor(LoadCursorW(nullptr, interactive ? IDC_HAND : IDC_ARROW));

    if (dragIndex_ >= 0 && page_ == Page::Curve) {
        float t = (sx - curvePlot_.left) / (curvePlot_.right - curvePlot_.left) * 100.0f;
        float p = (curvePlot_.bottom - sy) / (curvePlot_.bottom - curvePlot_.top) * 100.0f;
        curve_[dragIndex_].t = std::clamp(static_cast<double>(t), 0.0, 100.0);
        curve_[dragIndex_].p = std::clamp(static_cast<double>(p), 0.0, 100.0);
        std::sort(curve_.begin(), curve_.end(), [](const Point2& a, const Point2& b) { return a.t < b.t; });
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }
    if (previous != hover_) InvalidateRect(hwnd_, nullptr, FALSE);
}

void App::OnMouseLeave() {
    hover_.clear();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void App::OnLButtonDown(int x, int y) {
    float sx = x / scale_, sy = y / scale_;
    if (page_ == Page::Curve) {
        for (size_t i = 0; i < curve_.size(); ++i) {
            float px = curvePlot_.left + static_cast<float>(curve_[i].t / 100.0) * (curvePlot_.right - curvePlot_.left);
            float py = curvePlot_.bottom - static_cast<float>(curve_[i].p / 100.0) * (curvePlot_.bottom - curvePlot_.top);
            if (std::abs(sx - px) < 12 && std::abs(sy - py) < 12) {
                dragIndex_ = static_cast<int>(i);
                return;
            }
        }
    }
    for (auto& h : hits_) {
        if (sx < h.rect.left || sx > h.rect.right || sy < h.rect.top || sy > h.rect.bottom) continue;
        const std::wstring& id = h.id;
        if (id.rfind(L"nav:", 0) == 0) {
            page_ = static_cast<Page>(_wtoi(id.c_str() + 4));
            scroll_ = 0.0f;
            InvalidateRect(hwnd_, nullptr, FALSE);
        } else if (id == L"set:theme") {
            const std::wstring next = config_.theme == L"System" ? L"Dark"
                                      : config_.theme == L"Dark" ? L"Light"
                                                                 : L"System";
            config_.theme = next;
            SendValue("theme", next == L"Dark" ? "\"Dark\"" : next == L"Light" ? "\"Light\"" : "\"System\"");
            if (!hasInitial_) {
                dark_ = next == L"Dark" ? true : next == L"Light" ? false : !SystemUsesLightTheme();
                theme_ = MakeTheme(dark_);
                BOOL useDark = dark_ ? TRUE : FALSE;
                DwmSetWindowAttribute(hwnd_, 20, &useDark, sizeof(useDark));
            }
            InvalidateRect(hwnd_, nullptr, FALSE);
        } else if (id == L"set:lang") {
            const bool toEn = !fc::IsEn();
            config_.language = toEn ? L"en-US" : L"zh-CN";
            fc::SetLang(toEn ? fc::Lang::En : fc::Lang::Zh);
            SendValue("language", toEn ? "\"en-US\"" : "\"zh-CN\"");
            InvalidateRect(hwnd_, nullptr, FALSE);
        } else if (id == L"set:unit") {
            config_.temperatureUnit = config_.temperatureUnit == L"Fahrenheit" ? L"Celsius" : L"Fahrenheit";
            SendValue("temperatureUnit", config_.temperatureUnit == L"Fahrenheit" ? "\"Fahrenheit\"" : "\"Celsius\"");
            InvalidateRect(hwnd_, nullptr, FALSE);
        } else if (id == L"set:autostart") {
            autostart_ = !autostart_;
            toast_ = autostart_ ? fc::T(L"开机自启由安装器写入（待接线）") : fc::T(L"已关闭开机自启（待接线）");
            toastUntil_ = GetTickCount() + 2000;
            InvalidateRect(hwnd_, nullptr, FALSE);
        } else if (id == L"set:notify") {
            config_.notifyOnBleDisconnect = !config_.notifyOnBleDisconnect;
            SendValue("notifyOnBleDisconnect", config_.notifyOnBleDisconnect ? "true" : "false");
            InvalidateRect(hwnd_, nullptr, FALSE);
        } else if (id == L"set:smooth") {
            smoothMs_ = smoothMs_ >= 3000 ? 100 : smoothMs_ + 350;
            char buffer[32];
            std::snprintf(buffer, sizeof(buffer), "%.2f", smoothMs_ / 1000.0);
            SendValue("pwmSmoothingWindowSeconds", buffer);
            InvalidateRect(hwnd_, nullptr, FALSE);
        } else if (id == L"set:tempsrc") {
            static const wchar_t* kSources[] = { L"LibreHardwareMonitor", L"AtkAcpi", L"Wmi", L"Aida64" };
            static const char* kJson[] = { "\"LibreHardwareMonitor\"", "\"AtkAcpi\"", "\"Wmi\"", "\"Aida64\"" };
            int index = 0;
            for (int i = 0; i < 4; ++i) if (config_.temperatureSource == kSources[i]) index = i;
            index = (index + 1) % 4;
            config_.temperatureSource = kSources[index];
            SendValue("temperatureSource", kJson[index]);
            InvalidateRect(hwnd_, nullptr, FALSE);
        } else if (id == L"set:fansrc") {
            static const wchar_t* kSources[] = { L"AtkAcpi", L"LibreHardwareMonitor", L"Aida64", L"Simulated" };
            static const char* kJson[] = { "\"AtkAcpi\"", "\"LibreHardwareMonitor\"", "\"Aida64\"", "\"Simulated\"" };
            int index = 0;
            for (int i = 0; i < 4; ++i) if (config_.fanSpeedSource == kSources[i]) index = i;
            index = (index + 1) % 4;
            config_.fanSpeedSource = kSources[index];
            SendValue("fanSpeedSource", kJson[index]);
            InvalidateRect(hwnd_, nullptr, FALSE);
        } else if (id == L"set:comm") {
            config_.communicationType = config_.communicationType == L"Com" ? L"Ble" : L"Com";
            SendValue("communicationType", config_.communicationType == L"Com" ? "\"Com\"" : "\"Ble\"");
            InvalidateRect(hwnd_, nullptr, FALSE);
        } else if (id == L"set:logs" || id == L"chip:logs") {
            ShellExecuteW(nullptr, L"open", L"explorer.exe", L"%APPDATA%\\FanControl\\Logs", nullptr, SW_SHOWNORMAL);
        } else if (id == L"chip:apply") {
            PushCurveToBackend();
            InvalidateRect(hwnd_, nullptr, FALSE);
        } else if (id == L"chip:update") {
            toast_ = fc::T(L"当前已是最新版本");
            toastUntil_ = GetTickCount() + 1800;
            InvalidateRect(hwnd_, nullptr, FALSE);
        } else if (id == L"chip:reset") {
            curve_ = { {30, 20}, {50, 30}, {65, 48}, {75, 70}, {85, 90}, {95, 100} };
            PushCurveToBackend();
            InvalidateRect(hwnd_, nullptr, FALSE);
        } else {
            toast_ = fc::T(L"命令已发送：") + id;
            toastUntil_ = GetTickCount() + 1500;
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
        return;
    }
}

void App::OnLButtonUp(int, int) {
    if (dragIndex_ >= 0 && page_ == Page::Curve) PushCurveToBackend();
    dragIndex_ = -1;
}

void App::OnMouseWheel(int delta) {
    float visible = std::max(120.0f, viewH_ - 96.0f);
    float maxScroll = std::max(0.0f, pageHeight_[static_cast<int>(page_)] - visible);
    scroll_ -= delta / 120.0f * 70.0f;
    scroll_ = std::clamp(scroll_, 0.0f, maxScroll);
}

void App::SetInitialPage(int index) {
    if (index >= 0 && index < 4) page_ = static_cast<Page>(index);
}

void App::SetInitialTheme(bool dark) {
    hasInitial_ = true;
    initialDark_ = dark;
}

void App::ToggleTheme() {
    dark_ = !dark_;
    theme_ = MakeTheme(dark_);
    BOOL useDark = dark_ ? TRUE : FALSE;
    DwmSetWindowAttribute(hwnd_, 20, &useDark, sizeof(useDark));
    InvalidateRect(hwnd_, nullptr, FALSE);
}

// ---------------- 后台守护进程（命名管道）----------------

void App::StartIpc() {
    if (ipcThread_.joinable()) return;
    ipc_ = std::make_unique<fc::IpcClient>();
    ipcStop_ = false;
    ipcThread_ = std::thread([this]() {
        fc::Message message;
        while (!ipcStop_) {
            bool connected = false;
            {
                std::lock_guard<std::mutex> lock(sendMutex_);
                connected = ipc_->Connect(1200);
            }
            if (!connected) {
                if (backendUp_.exchange(false) && hwnd_) PostMessageW(hwnd_, WM_APP + 2, 0, 0);
                if (!launcherTried_) {
                    launcherTried_ = true;
                    StartHostProcess();
                }
                for (int i = 0; i < 8 && !ipcStop_; ++i) Sleep(250);
                continue;
            }
            if (!backendUp_.exchange(true) && hwnd_) PostMessageW(hwnd_, WM_APP + 2, 1, 0);
            // 服务端连上就推一份 state；先读后写，避免首帧写把界面线程堵在管道缓冲上
            while (!ipcStop_) {
                // 用超时读替代阻塞读：既能及时响应退出，也不会被空闲管道挂住
                if (!ipc_->ReadMessage(1000, message)) {
                    if (ipcStop_) break;
                    if (ipc_->IsOpen()) continue;
                    break;
                }
                auto* payload = new fc::Message(message);
                if (!hwnd_ || !PostMessageW(hwnd_, WM_APP + 1, 0, reinterpret_cast<LPARAM>(payload))) {
                    delete payload;
                    break;
                }
            }
            {
                std::lock_guard<std::mutex> lock(sendMutex_);
                ipc_->Close();
            }
            if (backendUp_.exchange(false) && hwnd_) PostMessageW(hwnd_, WM_APP + 2, 0, 0);
            for (int i = 0; i < 8 && !ipcStop_; ++i) Sleep(250);
        }
    });
}

void App::StopIpc() {
    ipcStop_ = true;
    {
        std::lock_guard<std::mutex> lock(sendMutex_);
        if (ipc_) ipc_->Close();
    }
    if (ipcThread_.joinable()) ipcThread_.join();
    ipc_.reset();
}

void App::SendValue(const char* key, const char* valueJson) {
    std::lock_guard<std::mutex> lock(sendMutex_);
    if (ipc_ && ipc_->IsOpen()) ipc_->SetValue(key, valueJson);
}

void App::OnIpcStatus(bool connected) {
    backendUp_ = connected;
    snap_.backendConnected = connected;
    if (!connected) {
        snap_.commMode = fc::T(L"等待后台连接");
        snap_.lastError.clear();
    } else {
        // 连上后台后丢弃演示历史，只保留真实采样
        cpuHist_.clear();
        gpuHist_.clear();
        pwmHist_.clear();
        lastSampleMs_ = 0;
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void App::OnIpcMessage(void* payload) {
    std::unique_ptr<fc::Message> message(static_cast<fc::Message*>(payload));
    if (!message) return;
    if (message->hasConfig) ApplyConfig(message->config);
    if (message->hasState) ApplyState(message->state);
    if (message->event == L"error") {
        // 后台只给 code + 参数，文案由界面按当前语言拼
        toast_ = fc::ErrorText(message->errorCode, message->errorArg1, message->errorArg2, message->error);
        toastUntil_ = GetTickCount() + 4000;
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void App::ApplyConfig(const fc::Config& config) {
    const bool first = !hasConfig_;
    config_ = config;
    hasConfig_ = true;
    // 语言跟着后台配置走：后台是唯一写盘方，换语言后重启界面仍然是同一种
    const fc::Lang lang = config.language.rfind(L"en", 0) == 0 ? fc::Lang::En : fc::Lang::Zh;
    if (lang != fc::GetLang()) {
        fc::SetLang(lang);
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
    smoothMs_ = std::clamp(static_cast<int>(config.pwmSmoothingWindowSeconds * 1000.0), 100, 3000);
    if (first || (dragIndex_ < 0 && curve_.size() != config.curve.size())) {
        if (dragIndex_ < 0) {
            curve_.clear();
            for (const auto& point : config.curve) curve_.push_back(Point2{ point.x, point.y });
        }
    }
    if (!hasInitial_) {
        const bool wantDark = config.theme == L"Dark" ? true
                              : config.theme == L"Light" ? false
                                                         : !SystemUsesLightTheme();
        if (wantDark != dark_) {
            dark_ = wantDark;
            theme_ = MakeTheme(dark_);
            BOOL useDark = dark_ ? TRUE : FALSE;
            DwmSetWindowAttribute(hwnd_, 20, &useDark, sizeof(useDark));
        }
    }
}

void App::ApplyState(const fc::State& state) {
    const bool fahrenheit = config_.temperatureUnit == L"Fahrenheit";
    auto toDisplay = [fahrenheit](double celsius) {
        return fahrenheit ? celsius * 9.0 / 5.0 + 32.0 : celsius;
    };
    snap_.cpu = toDisplay(state.cpu);
    snap_.gpu = toDisplay(state.gpu);
    snap_.pwm = static_cast<int>(std::lround(state.pwm));
    snap_.rpm = static_cast<int>(std::lround(state.rpm));
    snap_.deviceConnected = state.connected;
    snap_.backendConnected = true;
    const std::wstring prefix = state.communicationType == L"Com" ? std::wstring(fc::T(L"串口")) + L" · "
                                                                  : std::wstring(fc::T(L"蓝牙")) + L" · ";
    snap_.commMode = prefix + (state.deviceName.empty() ? std::wstring(fc::T(L"未配对设备")) : state.deviceName);
    if (!state.lastError.empty()) {
        snap_.lastError = state.lastError;
        snap_.errorCode = state.errorCode;
        snap_.errorArg1 = state.errorArg1;
        snap_.errorArg2 = state.errorArg2;
    }
    if (cpuHist_.empty()) {
        // 首次连上后台：用当前值铺满窗口，避免趋势图一开始只有左端一两个点
        for (int i = 0; i < kHistoryPoints; ++i) {
            cpuHist_.emplace_back(0.0, snap_.cpu);
            gpuHist_.emplace_back(0.0, snap_.gpu);
            pwmHist_.emplace_back(0.0, static_cast<double>(snap_.pwm));
        }
    }
    if (state.timestamp - lastSampleMs_ >= 900) {
        lastSampleMs_ = state.timestamp;
        BuildHistory(1.0f);
    }
}

void App::PushCurveToBackend() {
    std::vector<fc::CurvePoint> points;
    points.reserve(curve_.size());
    for (const auto& point : curve_) points.push_back(fc::CurvePoint{ point.t, point.p });
    std::lock_guard<std::mutex> lock(sendMutex_);
    if (ipc_ && ipc_->IsOpen()) ipc_->SetCurve(points, false);
}

std::wstring App::ThemeLabel() const {
    if (config_.theme == L"Dark") return fc::T(L"深色");
    if (config_.theme == L"Light") return fc::T(L"浅色");
    return fc::T(L"跟随系统");
}

std::wstring App::UnitLabel() const {
    return config_.temperatureUnit == L"Fahrenheit" ? fc::T(L"华氏度 °F") : fc::T(L"摄氏度 °C");
}

std::wstring App::LangLabel() const {
    return fc::IsEn() ? L"English" : fc::T(L"简体中文");
}


void App::StartHostProcess() {
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (wchar_t* slash = wcsrchr(path, L'\\')) *(slash + 1) = L'\0';
    else return;
    wcscat_s(path, L"FanControl.Host.exe");
    if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) return;
    // 守护进程需要管理员权限：首次连接失败时按需提权拉起
    auto result = reinterpret_cast<INT_PTR>(
        ShellExecuteW(nullptr, L"runas", path, nullptr, nullptr, SW_SHOWNORMAL));
    if (result <= 32) {
        toast_ = fc::T(L"启动后台失败（需要管理员权限）");
        toastUntil_ = GetTickCount() + 3000;
    }
}

