#include "app.h"
#include "i18n.h"
#include <dwmapi.h>
#include <shellapi.h>
#include <shobjidl.h>  // IFileOpenDialog：首次运行与设置页选数据目录
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
    case 3: return fc::T(L"关于");
    default: return fc::T(L"运行日志");
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
        t.rpmLine = Col(0xE06C9F);
        t.danger = Col(0xFF99A4);
        t.warning = Col(0xE8B84B);
        t.validation = Col(0xC59BFF);
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
        t.rpmLine = Col(0xB03A6E);
        t.danger = Col(0xC42B1C);
        t.warning = Col(0x8A6100);
        t.validation = Col(0x6E4BC4);
        t.ok = Col(0x0F7B0F);
    }
    return t;
}

namespace {
// 错误的四个级别：0 提示 / 1 异常 / 2 错误 / 3 验证错误（与后台 ErrorSeverity 一致）
const wchar_t* SeverityLabel(int severity) {
    switch (severity) {
    case 0: return fc::T(L"提示");
    case 1: return fc::T(L"异常");
    case 3: return fc::T(L"验证错误");
    default: return fc::T(L"错误");
    }
}

D2D1_COLOR_F SeverityColor(const Theme& theme, int severity) {
    switch (severity) {
    case 0: return theme.textSecondary;
    case 1: return theme.warning;
    case 3: return theme.validation;
    default: return theme.danger;
    }
}
} // namespace

bool App::Init(HWND hwnd) {
    hwnd_ = hwnd;
    dark_ = hasInitial_ ? initialDark_ : !SystemUsesLightTheme();
    theme_ = MakeTheme(dark_);

    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &d2d_))) return false;
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                   reinterpret_cast<IUnknown**>(&dw_)))) return false;
    CreateFormats();

    // 界面不编造数据：曲线等后台配置下发，趋势图在收到第一帧 state 时用真实值铺满（见 ApplyState）。
    // 这两处以前各塞了一份假数据（6 点曲线 + 180 个正弦历史点），首次打开会显示根本不存在的曲线。
    const bool ok = CreateTarget();
    // 单进程：界面 DLL 由 FanControl.exe 在进程内加载，后台从一开始就是「已连接」
    backendUp_ = true;
    snap_.backendConnected = true;
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
    // 等宽：日志的时间戳要按列对齐；Consolas 每台 Windows 都自带，不必随包分发字体
    make(L"Consolas", DWRITE_FONT_WEIGHT_NORMAL, 12.0f, &fMono_);
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
    // 颜色刷挂在 render target 上，target 没了它们也必须先放掉
    for (auto& entry : brushes_) entry.second->Release();
    brushes_.clear();
    if (rt_) { rt_->Release(); rt_ = nullptr; }
}

void App::Release() {
    DropTarget();
    for (auto& entry : glyphFormats_) entry.second->Release();
    glyphFormats_.clear();
    measureCache_.clear();
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

ID2D1SolidColorBrush* App::Brush(const D2D1_COLOR_F& c) {
    if (!rt_) return nullptr;
    const uint32_t key = (static_cast<uint32_t>(c.r * 255.0f + 0.5f) << 24) |
                         (static_cast<uint32_t>(c.g * 255.0f + 0.5f) << 16) |
                         (static_cast<uint32_t>(c.b * 255.0f + 0.5f) << 8) |
                         static_cast<uint32_t>(c.a * 255.0f + 0.5f);
    auto it = brushes_.find(key);
    if (it != brushes_.end()) return it->second;
    ID2D1SolidColorBrush* b = nullptr;
    if (FAILED(rt_->CreateSolidColorBrush(c, &b)) || !b) return nullptr;
    brushes_.emplace(key, b);
    return b;
}

void App::FillRect(D2D1_RECT_F r, D2D1_COLOR_F c) {
    if (ID2D1SolidColorBrush* b = Brush(c)) rt_->FillRectangle(r, b);
}

void App::FillRound(D2D1_RECT_F r, float radius, D2D1_COLOR_F c) {
    if (ID2D1SolidColorBrush* b = Brush(c)) rt_->FillRoundedRectangle(D2D1::RoundedRect(r, radius, radius), b);
}

void App::StrokeRound(D2D1_RECT_F r, float radius, D2D1_COLOR_F c, float width) {
    if (ID2D1SolidColorBrush* b = Brush(c)) rt_->DrawRoundedRectangle(D2D1::RoundedRect(r, radius, radius), b, width);
}

void App::Line(float x1, float y1, float x2, float y2, D2D1_COLOR_F c, float width) {
    if (ID2D1SolidColorBrush* b = Brush(c)) rt_->DrawLine(D2D1::Point2F(x1, y1), D2D1::Point2F(x2, y2), b, width);
}

void App::Text(const std::wstring& s, D2D1_RECT_F r, IDWriteTextFormat* fmt, D2D1_COLOR_F c) {
    ID2D1SolidColorBrush* b = Brush(c);
    if (!b) return;
    fmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    rt_->DrawTextW(s.c_str(), static_cast<UINT32>(s.size()), fmt, r, b, D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

void App::TextMiddle(const std::wstring& s, D2D1_RECT_F r, IDWriteTextFormat* fmt, D2D1_COLOR_F c) {
    ID2D1SolidColorBrush* b = Brush(c);
    if (!b) return;
    fmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    rt_->DrawTextW(s.c_str(), static_cast<UINT32>(s.size()), fmt, r, b, D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

void App::TextCentered(const std::wstring& s, D2D1_RECT_F r, IDWriteTextFormat* fmt, D2D1_COLOR_F c) {
    ID2D1SolidColorBrush* b = Brush(c);
    if (!b) return;
    fmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    rt_->DrawTextW(s.c_str(), static_cast<UINT32>(s.size()), fmt, r, b, D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

void App::TextRight(const std::wstring& s, D2D1_RECT_F r, IDWriteTextFormat* fmt, D2D1_COLOR_F c) {
    ID2D1SolidColorBrush* b = Brush(c);
    if (!b) return;
    fmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
    fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    rt_->DrawTextW(s.c_str(), static_cast<UINT32>(s.size()), fmt, r, b, D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

void App::Glyph(const std::wstring& s, D2D1_RECT_F r, float size, D2D1_COLOR_F c) {
    // 字号只有 10/12/14/16 这几档，按桶缓存 TextFormat；以前每次画图标都新建一个
    const int key = static_cast<int>(size * 4.0f);
    IDWriteTextFormat* fmt = nullptr;
    auto it = glyphFormats_.find(key);
    if (it != glyphFormats_.end()) {
        fmt = it->second;
    } else {
        if (FAILED(dw_->CreateTextFormat(L"Segoe Fluent Icons", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                                         DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"en-US", &fmt))) {
            return;
        }
        fmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        glyphFormats_.emplace(key, fmt);
    }
    ID2D1SolidColorBrush* b = Brush(c);
    if (!b) return;
    rt_->DrawTextW(s.c_str(), static_cast<UINT32>(s.size()), fmt, r, b, D2D1_DRAW_TEXT_OPTIONS_CLIP);
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

float App::MeasureText(const std::wstring& text, IDWriteTextFormat* format) const {
    if (!dw_ || !format || text.empty()) return 0.0f;
    // 每个按钮每帧都量一次会把 TextLayout 造得很凶：按 (格式, 文本) 缓存，宽度不随主题变
    std::wstring key = std::to_wstring(reinterpret_cast<uintptr_t>(format));
    key.push_back(L'|');
    key += text;
    auto cached = measureCache_.find(key);
    if (cached != measureCache_.end()) return cached->second;
    IDWriteTextLayout* layout = nullptr;
    if (FAILED(dw_->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()), format,
                                     2048.0f, 100.0f, &layout)) || !layout)
        return 0.0f;
    DWRITE_TEXT_METRICS m{};
    const float width = SUCCEEDED(layout->GetMetrics(&m)) ? m.widthIncludingTrailingWhitespace : 0.0f;
    layout->Release();
    if (measureCache_.size() > 512) measureCache_.clear();  // 设备名之类会变的文本不做无限缓存
    measureCache_.emplace(std::move(key), width);
    return width;
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
    // 开关只认开关本体（右边 96 DIP 宽、整行高）：以前整行都能点，点到标题就把设置翻了
    AddHit(D2D1::RectF(x + w - 96.0f, y, x + w - 4.0f, y + h), id);
}

void App::RowSlider(float x, float y, float w, float h, const std::wstring& id, const std::wstring& title,
                    const std::wstring& subtitle, double value, double lo, double hi,
                    const std::wstring& valueText) {
    D2D1_RECT_F r = D2D1::RectF(x, y, x + w, y + h);
    if (Hovered(id)) FillRound(r, 4.0f, theme_.controlHover);
    Text(title, D2D1::RectF(x + 16, y + h / 2 - 19, x + w - 340, y + h / 2 - 1), fBody_, theme_.textPrimary);
    if (!subtitle.empty())
        Text(subtitle, D2D1::RectF(x + 16, y + h / 2 + 1, x + w - 340, y + h / 2 + 18), fSmall_, theme_.textSecondary);
    // 轨道右侧给数值留出固定宽度，避免滑块推到最右时压住文字。
    float trackL = x + w - 320, trackR = x + w - 140, cy = y + h / 2;
    FillRound(D2D1::RectF(trackL, cy - 2, trackR, cy + 2), 2.0f, theme_.controlBorder);
    const double ratio = hi > lo ? std::clamp((value - lo) / (hi - lo), 0.0, 1.0) : 0.0;
    FillRound(D2D1::RectF(trackL, cy - 2, trackL + static_cast<float>((trackR - trackL) * ratio), cy + 2), 2.0f,
              theme_.accent);
    float kx = trackL + static_cast<float>((trackR - trackL) * ratio);
    FillRound(D2D1::RectF(kx - 10, cy - 10, kx + 10, cy + 10), 10.0f, theme_.controlBg);
    StrokeRound(D2D1::RectF(kx - 10, cy - 10, kx + 10, cy + 10), 10.0f, theme_.controlBorder);
    TextRight(valueText, D2D1::RectF(x + w - 128, y, x + w - 16, y + h), fBody_, theme_.textSecondary);
    sliders_[id] = SliderGeom{ trackL, trackR, lo, hi };
    // 只有轨道（两侧各留 12 DIP 手指余量）算命中：以前整行都算，点标题也会把值跳到底
    AddHit(D2D1::RectF(trackL - 12.0f, cy - 20.0f, trackR + 12.0f, cy + 20.0f), id);
}

double App::SliderValueAt(const std::wstring& id, int clickX, double fallback) const {
    const auto it = sliders_.find(id);
    if (it == sliders_.end() || it->second.trackR <= it->second.trackL) return fallback;
    const double t = std::clamp((clickX - it->second.trackL) / (it->second.trackR - it->second.trackL), 0.0f, 1.0f);
    return it->second.lo + t * (it->second.hi - it->second.lo);
}

bool App::IsSliderRow(const std::wstring& id) const {
    return id == L"set:manualpwm" || id == L"set:smooth" || id == L"set:poll" || id == L"set:blepoll" ||
           id == L"set:reconnect";
}

double App::SliderRowValue(const std::wstring& id) const {
    if (id == L"set:manualpwm") return config_.manualPwmPercent;
    if (id == L"set:smooth") return config_.pwmSmoothingWindowSeconds;
    if (id == L"set:poll") return config_.pollIntervalMilliseconds / 1000.0;
    if (id == L"set:blepoll") return config_.blePollIntervalSeconds;
    if (id == L"set:reconnect") return config_.bleReconnectIntervalSeconds;
    return 0.0;
}

// 滑块取值的量化规则集中在这里：拖动（send=false）与松手（send=true）走同一条路径，
// 否则会出现「拖的时候显示 4.37、松手变成 4.4」的跳动。
void App::ApplySliderValue(const std::wstring& id, double value, bool send) {
    char buffer[32];
    if (id == L"set:manualpwm") {
        const int next = static_cast<int>(std::lround(value));
        config_.manualPwmPercent = next;
        if (send) {
            std::snprintf(buffer, sizeof(buffer), "%d", next);
            SendValue("manualPwmPercent", buffer);
        }
    } else if (id == L"set:smooth") {
        const double seconds = std::round(value * 10.0) / 10.0;
        config_.pwmSmoothingWindowSeconds = seconds;
        smoothMs_ = std::clamp(static_cast<int>(seconds * 1000.0), 100, 3000);
        if (send) {
            std::snprintf(buffer, sizeof(buffer), "%.1f", seconds);
            SendValue("pwmSmoothingWindowSeconds", buffer);
        }
    } else if (id == L"set:poll") {
        const double seconds = std::round(value * 10.0) / 10.0;
        config_.pollIntervalMilliseconds = static_cast<int>(std::lround(seconds * 1000.0));
        if (send) {
            std::snprintf(buffer, sizeof(buffer), "%d", config_.pollIntervalMilliseconds);
            SendValue("pollIntervalMilliseconds", buffer);
        }
    } else if (id == L"set:blepoll") {
        const int next = static_cast<int>(std::lround(value));
        config_.blePollIntervalSeconds = next;
        if (send) {
            std::snprintf(buffer, sizeof(buffer), "%d", next);
            SendValue("blePollIntervalSeconds", buffer);
        }
    } else if (id == L"set:reconnect") {
        const int next = static_cast<int>(std::lround(value));
        config_.bleReconnectIntervalSeconds = next;
        if (send) {
            std::snprintf(buffer, sizeof(buffer), "%d", next);
            SendValue("bleReconnectIntervalSeconds", buffer);
        }
    }
}

// 宿主每 400 ms 会推一份完整配置；拖拽期间不能让旧快照把滑块拉回去
void App::RestoreSliderValue() {
    if (sliderDrag_.empty()) return;
    ApplySliderValue(sliderDrag_, sliderValue_, false);
}

void App::RowCombo(float x, float y, float w, float h, const std::wstring& id, const std::wstring& title,
                   const std::wstring& subtitle, const std::wstring& value) {
    D2D1_RECT_F r = D2D1::RectF(x, y, x + w, y + h);
    if (Hovered(id)) FillRound(r, 4.0f, theme_.controlHover);
    Text(title, D2D1::RectF(x + 16, y + h / 2 - 19, x + w - 280, y + h / 2 - 1), fBody_, theme_.textPrimary);
    if (!subtitle.empty())
        Text(subtitle, D2D1::RectF(x + 16, y + h / 2 + 1, x + w - 280, y + h / 2 + 18), fSmall_, theme_.textSecondary);
    // 右侧画一个像下拉框的值区：Win11 设置里 ComboBox 就是这个样子
    D2D1_RECT_F box = D2D1::RectF(x + w - 244, y + h / 2 - 15, x + w - 16, y + h / 2 + 15);
    FillRound(box, 4.0f, theme_.controlBg);
    StrokeRound(box, 4.0f, Hovered(id) ? theme_.accent : theme_.controlBorder);
    TextMiddle(value, D2D1::RectF(box.left + 10, box.top, box.right - 26, box.bottom), fBody_, theme_.textPrimary);
    Glyph(L"\xE70D", D2D1::RectF(box.right - 24, box.top, box.right - 6, box.bottom), 10.0f, theme_.textSecondary);
    // 下拉只认值框（左右各留 8 DIP 余量、整行高）
    AddHit(D2D1::RectF(box.left - 8.0f, y, box.right + 8.0f, y + h), id);
}

void App::RowStatus(float x, float y, float w, float h, const std::wstring& id, const std::wstring& title,
                    const std::wstring& subtitle, const std::wstring& value) {
    D2D1_RECT_F r = D2D1::RectF(x, y, x + w, y + h);
    if (Hovered(id)) FillRound(r, 4.0f, theme_.controlHover);
    Text(title, D2D1::RectF(x + 16, y + h / 2 - 19, x + w - 340, y + h / 2 - 1), fBody_, theme_.textPrimary);
    if (!subtitle.empty())
        Text(subtitle, D2D1::RectF(x + 16, y + h / 2 + 1, x + w - 340, y + h / 2 + 18), fSmall_, theme_.textSecondary);
    TextRight(value, D2D1::RectF(x + w - 320, y, x + w - 140, y + h), fBody_, theme_.textSecondary);
    D2D1_RECT_F button = D2D1::RectF(x + w - 132, y + h / 2 - 15, x + w - 16, y + h / 2 + 15);
    FillRound(button, 4.0f, theme_.controlBg);
    StrokeRound(button, 4.0f, theme_.controlBorder);
    TextCentered(fc::T(L"重新连接"), button, fSmall_, theme_.textPrimary);
    AddHit(button, L"act:reconnect");
}

void App::RowLink(float x, float y, float w, float h, const std::wstring& title, const std::wstring& subtitle,
                 const std::wstring& url) {
    D2D1_RECT_F r = D2D1::RectF(x, y, x + w, y + h);
    const std::wstring id = L"url:" + url;
    if (Hovered(id)) FillRound(r, 4.0f, theme_.controlHover);
    Text(title, D2D1::RectF(x + 16, y + 8, x + w - 16, y + 28), fBody_, theme_.accent);
    if (!subtitle.empty() && subtitle != title)
        Text(subtitle, D2D1::RectF(x + 16, y + 30, x + w - 16, y + 48), fSmall_, theme_.textSecondary);
    if (Hovered(id)) Line(x + 16, y + 29, x + 16 + MeasureText(title, fBody_), y + 29, theme_.accent, 1.0f);
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

    float y = 64.0f;
    for (int i = 0; i < 5; ++i) {
        D2D1_RECT_F item = D2D1::RectF(12, y, kSidebarWidth - 12, y + 36);
        std::wstring id = L"nav:" + std::to_wstring(i);
        bool selected = (static_cast<int>(page_) == i);
        if (selected) {
            FillRound(item, 4.0f, theme_.navSelected);
            FillRound(D2D1::RectF(12, y + 9, 15, y + 27), 1.5f, theme_.accentBar);
        } else if (Hovered(id)) {
            FillRound(item, 4.0f, theme_.navHover);
        }
        const wchar_t* icons[] = { L"\xE80F", L"\xE713", L"\xE9D2", L"\xE946", L"\xE81C" };
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
    Text(snap_.backendConnected ? fc::T(L"服务运行中") : fc::T(L"演示数据（服务未就绪）"),
         D2D1::RectF(40, by, kSidebarWidth - 20, by + 24), fSmall_, theme_.textSecondary);
    Text(Fmt(fc::T(L"版本 1.4.0 · %s"), dark_ ? fc::T(L"深色") : fc::T(L"浅色")),
         D2D1::RectF(24, by + 24, kSidebarWidth - 20, by + 48), fSmall_, theme_.textTertiary);
}

void App::DrawDashboard(float x, float y, float w) {
    // 与 1.3.0 一致：六块指标卡（CPU / GPU / PWM / 转速 / 控制模式 / 连接状态）+ 一条趋势图。
    const float gap = 8.0f;
    const float cardW = (w - gap * 2.0f) / 3.0f;
    const float cardH = 104.0f;
    struct Tile { const wchar_t* label; std::wstring value; const wchar_t* unit; };
    Tile tiles[6] = {
        { fc::T(L"CPU 温度"), snap_.cpu > 0.0 ? Fmt(L"%.1f", snap_.cpu) : L"—", L"°C" },
        { fc::T(L"GPU 温度"), snap_.gpu > 0.0 ? Fmt(L"%.1f", snap_.gpu) : L"—", L"°C" },
        { fc::T(L"输出 PWM"), Fmt(L"%d", snap_.pwm), L"%" },
        { fc::T(L"CPU 风扇转速"), snap_.rpm > 0 ? Fmt(L"%d", snap_.rpm) : L"—", L"RPM" },
        { fc::T(L"控制模式"), ModeLabel(config_.fanControlMode), L"" },
        { fc::T(L"连接状态"), snap_.deviceConnected ? fc::T(L"已连接") : fc::T(L"未连接"), L"" },
    };
    for (int i = 0; i < 6; ++i) {
        const int row = i / 3;
        const int col = i % 3;
        float cx = x + col * (cardW + gap);
        float cy = y + row * (cardH + gap);
        D2D1_RECT_F r = D2D1::RectF(cx, cy, cx + cardW, cy + cardH);
        FillRound(r, 8.0f, theme_.cardBg);
        StrokeRound(r, 8.0f, theme_.cardBorder);
        Text(tiles[i].label, D2D1::RectF(cx + 16, cy + 14, cx + cardW - 16, cy + 34), fSmall_, theme_.textSecondary);
        const bool wide = i >= 4;  // 控制模式/连接状态是文字，用小一号字体避免裁切
        Text(tiles[i].value, D2D1::RectF(cx + 16, cy + 40, cx + cardW - (wide ? 16.0f : 44.0f), cy + 82),
             wide ? fH2_ : fBig_, theme_.textPrimary);
        if (tiles[i].unit[0] != L'\0') {
            TextRight(tiles[i].unit, D2D1::RectF(cx + cardW - 42, cy + 40, cx + cardW - 14, cy + 82), fBody_,
                      theme_.textSecondary);
        }
    }
    y += cardH * 2 + gap + 18.0f;

    // 趋势卡
    float h = 250.0f;
    D2D1_RECT_F card = D2D1::RectF(x, y, x + w, y + h);
    FillRound(card, 8.0f, theme_.cardBg);
    StrokeRound(card, 8.0f, theme_.cardBorder);
    Text(fc::T(L"最近 4 分钟"), D2D1::RectF(x + 16, y + 12, x + w - 200, y + 34), fBodyStrong_,
         theme_.textPrimary);
    // 图例：四项（CPU / GPU / 输出 PWM / CPU 风扇转速），按文字宽度从右往左排，避免写死坐标被挤出去
    struct Leg { const wchar_t* name; D2D1_COLOR_F color; float width; };
    Leg legs[4] = { { L"CPU", theme_.cpuLine, 0.0f }, { L"GPU", theme_.gpuLine, 0.0f },
                    { fc::T(L"输出 PWM"), theme_.pwmLine, 0.0f }, { fc::T(L"CPU 风扇转速"), theme_.rpmLine, 0.0f } };
    float legendW = 0.0f;
    for (auto& leg : legs) {
        leg.width = MeasureText(leg.name, fSmall_);
        legendW += 12.0f + leg.width + 18.0f;
    }
    float lx = x + w - 16.0f - (legendW - 18.0f);
    for (auto& leg : legs) {
        FillRound(D2D1::RectF(lx, y + 20, lx + 8, y + 28), 4.0f, leg.color);
        Text(leg.name, D2D1::RectF(lx + 12, y + 14, lx + 20 + leg.width, y + 34), fSmall_, theme_.textSecondary);
        lx += 12.0f + leg.width + 18.0f;
    }

    // 左轴 0–100（温度 °C 与输出 PWM %），右轴给 CPU 风扇转速：峰值向上取整到 1000 RPM，至少 2000
    double rpmPeak = 0.0;
    for (const auto& point : rpmHist_) rpmPeak = std::max(rpmPeak, point.second);
    const double rpmAxisMax = std::max(2000.0, std::ceil(rpmPeak / 1000.0) * 1000.0);

    D2D1_RECT_F plot = D2D1::RectF(x + 44, y + 48, x + w - 44, y + h - 28);
    for (int i = 0; i <= 4; ++i) {  // 水平网格
        float gy = plot.top + (plot.bottom - plot.top) * i / 4.0f;
        Line(plot.left, gy, plot.right, gy, theme_.gridLine);
    }
    auto drawSeries = [&](const std::deque<std::pair<double, double>>& data, D2D1_COLOR_F color,
                          double maxValue, float lineWidth) {
        if (data.size() < 2) return;
        float step = (plot.right - plot.left) / (kHistoryPoints - 1);
        for (size_t i = 1; i < data.size(); ++i) {
            float x1 = plot.left + (i - 1) * step;
            float x2 = plot.left + i * step;
            float y1 = plot.bottom - static_cast<float>(data[i - 1].second / maxValue) * (plot.bottom - plot.top);
            float y2 = plot.bottom - static_cast<float>(data[i].second / maxValue) * (plot.bottom - plot.top);
            Line(x1, y1, x2, y2, color, lineWidth);
        }
    };
    drawSeries(cpuHist_, theme_.cpuLine, 100.0, 1.6f);
    drawSeries(gpuHist_, theme_.gpuLine, 100.0, 1.6f);
    drawSeries(pwmHist_, theme_.pwmLine, 100.0, 1.6f);
    drawSeries(rpmHist_, theme_.rpmLine, rpmAxisMax, 1.4f);

    TextCentered(L"100", D2D1::RectF(x + 8, plot.top - 8, x + 40, plot.top + 8), fSmall_, theme_.textTertiary);
    TextCentered(L"0", D2D1::RectF(x + 8, plot.bottom - 8, x + 40, plot.bottom + 8), fSmall_, theme_.textTertiary);
    TextCentered(Fmt(L"%d", static_cast<int>(rpmAxisMax)),
                 D2D1::RectF(x + w - 40, plot.top - 8, x + w - 4, plot.top + 8), fSmall_, theme_.rpmLine);
    TextCentered(L"0", D2D1::RectF(x + w - 40, plot.bottom - 8, x + w - 4, plot.bottom + 8), fSmall_,
                 theme_.rpmLine);
    y += h + 14.0f;

    // 最近错误：与 1.3.0 一样只在真的出错时出现一行，不再单独占一张卡
    std::wstring recentError;
    int recentSeverity = snap_.errorSeverity;
    if (!snap_.lastError.empty()) {
        recentError = fc::ErrorText(snap_.errorCode, snap_.errorArg1, snap_.errorArg2, snap_.lastError);
    } else if (!snap_.commError.empty()) {
        recentError = fc::ErrorText(snap_.commErrorCode, snap_.commErrorArg1, snap_.commErrorArg2, snap_.commError);
        recentSeverity = snap_.commErrorSeverity;
    }
    if (!recentError.empty()) {
        // 级别决定前缀与颜色：提示（灰）/ 异常（琥珀）/ 错误（红）/ 验证错误（紫）
        Text(std::wstring(fc::T(L"最近错误")) + L" · " + SeverityLabel(recentSeverity) + L" · " + snap_.commMode +
                 L" · " + recentError,
             D2D1::RectF(x + 4, y, x + w - 4, y + 22), fSmall_, SeverityColor(theme_, recentSeverity));
        y += 26.0f;
    }
    contentHeight_ = y + 16.0f;
}

namespace {
std::wstring SourceLabel(const std::wstring& value) {
    if (value == L"LibreHardwareMonitor") return L"LibreHardwareMonitor";
    if (value == L"AtkAcpi") return L"ATK ACPI";
    if (value == L"Wmi") return L"WMI";
    if (value == L"Aida64") return L"AIDA64";
    if (value == L"LenovoWmi") return L"Lenovo WMI";
    if (value == L"Simulated") return fc::T(L"模拟数据");
    if (value == L"Serial") return fc::T(L"串口");
    return value;
}
} // namespace

void App::DrawSettings(float x, float y, float w) {
    const float top = y;
    const float rowH = 64.0f;
    const float headerH = 44.0f;  // 与 Card 的标题区一致：内容从标题下 44 DIP 开始
    const float cardGap = 16.0f;
    std::vector<float> separators;  // 行分隔线留到最后统一画，否则会被行的悬停底色盖住

    float ry = y;
    auto beginCard = [&](const wchar_t* title, int rows) {
        const float height = headerH + rows * rowH + 8.0f;
        Card(x, y, w, fc::T(title), height);
        for (int i = 1; i < rows; ++i) separators.push_back(y + headerH + i * rowH);
        ry = y + headerH;
        y += height + cardGap;
    };

    // 1) 温度与转速：只放「数据从哪来」，控制行为放到下一张卡
    beginCard(L"温度与转速", 4);
    RowCombo(x, ry, w, rowH, L"set:tempsrc", fc::T(L"温度来源"), fc::T(L"CPU 温度的读取方式"),
             SourceLabel(config_.temperatureSource));
    ry += rowH;
    RowCombo(x, ry, w, rowH, L"set:gpusrc", fc::T(L"GPU 温度源"), fc::T(L"显卡温度的读取方式"), GpuSourceLabel());
    ry += rowH;
    RowCombo(x, ry, w, rowH, L"set:gpusel", fc::T(L"GPU 选择"), fc::T(L"多显卡时指定读取哪一块"),
             config_.gpuSelection == L"Auto" ? std::wstring(fc::T(L"自动")) : config_.gpuSelection);
    ry += rowH;
    RowCombo(x, ry, w, rowH, L"set:fansrc", fc::T(L"转速来源"), fc::T(L"CPU 风扇转速的读取方式"),
             SourceLabel(config_.fanSpeedSource));

    // 2) 风扇控制：模式与平滑放在一起，按模式/开关条件显示行
    const bool manual = config_.fanControlMode == L"Manual";
    beginCard(L"风扇控制", 2 + (manual ? 1 : 0) + (config_.smoothingEnabled ? 1 : 0));
    RowCombo(x, ry, w, rowH, L"set:mode", fc::T(L"控制模式"), fc::T(L"决定输出 PWM 由谁计算"), ModeLabel(config_.fanControlMode));
    ry += rowH;
    if (manual) {
        RowSlider(x, ry, w, rowH, L"set:manualpwm", fc::T(L"手动 PWM"), fc::T(L"手动模式下直接指定占空比"),
                  config_.manualPwmPercent, 0, 100,
                  Fmt(L"%d %%", static_cast<int>(std::lround(config_.manualPwmPercent))));
        ry += rowH;
    }
    RowToggle(x, ry, w, rowH, L"set:smoothenable", fc::T(L"平滑输出"), fc::T(L"让输出 PWM 有过渡"),
              config_.smoothingEnabled);
    ry += rowH;
    if (config_.smoothingEnabled) {
        RowSlider(x, ry, w, rowH, L"set:smooth", fc::T(L"平滑窗口"), fc::T(L"输出 PWM 变化的过渡时间"),
                  config_.pwmSmoothingWindowSeconds, 0.5, 30.0,
                  Fmt(L"%.1f s", config_.pwmSmoothingWindowSeconds));
    }

    // 3) 通信：串口/蓝牙只显示当前方式用得上的行，另一套收起来
    const bool serial = config_.communicationType == L"Com";
    beginCard(L"通信", 2 + (serial ? 2 : 4));
    RowCombo(x, ry, w, rowH, L"set:comm", fc::T(L"通信方式"), fc::T(L"蓝牙 BLE 或串口"),
             serial ? fc::T(L"串口") : fc::T(L"蓝牙 BLE"));
    ry += rowH;
    if (serial) {
        RowCombo(x, ry, w, rowH, L"set:comport", fc::T(L"串口端口"), fc::T(L"与风扇相连的 COM 口"), config_.comPort);
        ry += rowH;
        RowCombo(x, ry, w, rowH, L"set:baud", fc::T(L"波特率"), fc::T(L"串口通信速率"),
                 std::to_wstring(config_.comBaudRate));
        ry += rowH;
    } else {
        // 蓝牙设备要看到一屏候选（名字 + MAC）才选得准，行内下拉放不下，改成点行弹窗
        RowButton(x, ry, w, rowH, L"set:bledev", fc::T(L"蓝牙设备"), fc::T(L"已配对的 NUS 风扇"), DeviceLabel());
        ry += rowH;
        RowSlider(x, ry, w, rowH, L"set:blepoll", fc::T(L"BLE 轮询"), fc::T(L"发送温度命令的间隔"),
                  config_.blePollIntervalSeconds, 1, 30, Fmt(L"%d s", config_.blePollIntervalSeconds));
        ry += rowH;
        RowSlider(x, ry, w, rowH, L"set:reconnect", fc::T(L"重连间隔"), fc::T(L"断开后每隔多少秒重试一次"),
                  config_.bleReconnectIntervalSeconds, 1, 60,
                  Fmt(L"%d s", config_.bleReconnectIntervalSeconds));
        ry += rowH;
        RowToggle(x, ry, w, rowH, L"set:autoreconnect", fc::T(L"自动重连"), fc::T(L"断开后自动尝试恢复"),
                  config_.autoReconnectBle);
        ry += rowH;
    }
    RowStatus(x, ry, w, rowH, L"set:connstate", fc::T(L"连接状态"), fc::T(L"设备链路"), ConnectionLabel());

    // 4) 系统：采样与启动行为
    beginCard(L"系统", 4);
    RowSlider(x, ry, w, rowH, L"set:poll", fc::T(L"采样间隔"), fc::T(L"读取传感器的周期"),
              config_.pollIntervalMilliseconds / 1000.0, 0.1, 5.0,
              Fmt(L"%.1f s", config_.pollIntervalMilliseconds / 1000.0));
    ry += rowH;
    RowToggle(x, ry, w, rowH, L"set:autostart", fc::T(L"开机自启"), fc::T(L"登录后自动启动 FanControl"), autostart_);
    ry += rowH;
    RowCombo(x, ry, w, rowH, L"set:datadir", fc::T(L"数据目录"), fc::T(L"配置与日志都放这里，切换时自动搬家"),
             DataDirLabel());
    ry += rowH;
    RowToggle(x, ry, w, rowH, L"set:logenabled", fc::T(L"日志记录"), fc::T(L"把运行日志写入数据目录"), logEnabled_);

    // 5) 外观与通知：只影响界面显示与提示
    beginCard(L"外观与通知", 5);
    RowCombo(x, ry, w, rowH, L"set:theme", fc::T(L"应用主题"), fc::T(L"跟随系统 / 深色 / 浅色"), ThemeLabel());
    ry += rowH;
    RowCombo(x, ry, w, rowH, L"set:lang", fc::T(L"语言"), fc::T(L"界面语言"), LangLabel());
    ry += rowH;
    RowCombo(x, ry, w, rowH, L"set:unit", fc::T(L"温度单位"), fc::T(L"仪表盘与曲线显示单位"), UnitLabel());
    ry += rowH;
    RowToggle(x, ry, w, rowH, L"set:notify", fc::T(L"断连通知"), fc::T(L"蓝牙断开时弹出系统通知"),
              config_.notifyOnBleDisconnect);
    ry += rowH;
    RowToggle(x, ry, w, rowH, L"set:notifytemp", fc::T(L"温度异常通知"), fc::T(L"温度数据源读取失败时提示"),
              config_.notifyOnTemperatureError);

    for (float sy : separators) Line(x + 16.0f, sy, x + w - 16.0f, sy, theme_.gridLine);

    // 重新加载：把界面上的值从后台再取一遍（1.3.0 的「重新加载」按钮）
    Chip(x, y, 120, 34, fc::T(L"重新加载"), false, L"settings:reload");
    y += 34 + 18;
    contentHeight_ = y - top + 24.0f;
}

void App::DrawCurve(float x, float y, float w) {
    // 曲线页：两条映射曲线并排平铺（去掉页签切换），下面是输出 PWM 的实时趋势。
    // 点哪张图就切到哪条曲线编辑（描边高亮），「添加点 / 删除点 / 重置」都作用在当前曲线。
    const float gap = 16.0f;
    const float panelH = 320.0f;
    const bool side = w >= 760.0f;
    const float pw = side ? (w - gap) / 2.0f : w;
    DrawCurvePanel(x, y, pw, panelH, 0, fc::T(L"温度曲线"), fc::T(L"温度 °C → 输出 PWM %"), CurveXMax(0));
    const float y2 = side ? y : y + panelH + gap;
    const float x2 = side ? x + pw + gap : x;
    DrawCurvePanel(x2, y2, pw, panelH, 1, fc::T(L"转速曲线"), fc::T(L"转速 RPM → 输出 PWM %"), CurveXMax(1));
    float rowY = (side ? y + panelH : y2 + panelH) + 16.0f;

    // 没有「保存」按钮：拖动松手、加点、删点、重置都立刻写回宿主，改坏了用「撤回」（最多 32 步）
    float bx = x;
    auto actionChip = [&](const wchar_t* label, bool accent, const wchar_t* id) {
        const float bw = std::max(88.0f, MeasureText(fc::T(label), fBody_) + 32.0f);
        Chip(bx, rowY, bw, 32, fc::T(label), accent, id);
        bx += bw + 8;
    };
    actionChip(L"添加点", false, L"curve:add");
    actionChip(L"删除点", false, L"curve:del");
    actionChip(L"重置", false, L"reset");
    actionChip(L"撤回", false, L"curve:undo");
    Text(fc::T(L"拖动圆点调整，点击空白处新增点；改动自动保存"),
         D2D1::RectF(bx + 8, rowY + 8, x + w, rowY + 32), fSmall_, theme_.textTertiary);

    const float pwmY = rowY + 48.0f;
    DrawPwmChart(x, pwmY, w);
    contentHeight_ = pwmY + 210.0f + 24.0f - y;
}

std::vector<App::Point2>& App::CurvePoints(int which) { return which == 0 ? curve_ : rpmCurve_; }

const std::vector<App::Point2>& App::CurvePoints(int which) const { return which == 0 ? curve_ : rpmCurve_; }

D2D1_RECT_F App::CurvePlot(int which) const { return which == 0 ? curvePlot_ : rpmPlot_; }

void App::DrawCurvePanel(float x, float y, float w, float h, int which, const std::wstring& title,
                         const std::wstring& axisLabel, double xMax) {
    const bool active = which == curveActive_;
    D2D1_RECT_F card = D2D1::RectF(x, y, x + w, y + h);
    FillRound(card, 8.0f, theme_.cardBg);
    StrokeRound(card, 8.0f, active ? theme_.accent : theme_.cardBorder, active ? 1.5f : 1.0f);
    Text(title, D2D1::RectF(x + 16, y + 12, x + w - 150, y + 34), fBodyStrong_,
         active ? theme_.textPrimary : theme_.textSecondary);
    TextRight(axisLabel, D2D1::RectF(x + w - 168, y + 14, x + w - 16, y + 34), fSmall_, theme_.textTertiary);

    D2D1_RECT_F plot = D2D1::RectF(x + 52, y + 48, x + w - 20, y + h - 42);
    if (which == 0) curvePlot_ = plot; else rpmPlot_ = plot;

    for (int i = 0; i <= 5; ++i) {
        float gy = plot.top + (plot.bottom - plot.top) * i / 5.0f;
        Line(plot.left, gy, plot.right, gy, theme_.gridLine);
        TextRight(Fmt(L"%d", 100 - i * 20), D2D1::RectF(x + 4, gy - 10, plot.left - 10, gy + 10), fSmall_,
                  theme_.textTertiary);
        float gx = plot.left + (plot.right - plot.left) * i / 5.0f;
        Line(gx, plot.top, gx, plot.bottom, theme_.gridLine);
        TextCentered(Fmt(L"%d", static_cast<int>(std::lround(xMax * i / 5.0))),
                     D2D1::RectF(gx - 28, plot.bottom + 4, gx + 28, plot.bottom + 22), fSmall_, theme_.textTertiary);
    }
    TextCentered(which == 0 ? fc::T(L"温度 °C") : fc::T(L"转速 RPM"),
                 D2D1::RectF(plot.left, plot.bottom + 22, plot.right, plot.bottom + 40), fSmall_,
                 theme_.textTertiary);

    const std::vector<Point2>& points = CurvePoints(which);
    auto toX = [&](double t) { return plot.left + static_cast<float>(t / xMax) * (plot.right - plot.left); };
    auto toY = [&](double p) { return plot.bottom - static_cast<float>(p / 100.0) * (plot.bottom - plot.top); };
    for (size_t i = 1; i < points.size(); ++i)
        Line(toX(points[i - 1].t), toY(points[i - 1].p), toX(points[i].t), toY(points[i].p), theme_.accent, 2.0f);
    for (size_t i = 0; i < points.size(); ++i) {
        float px = toX(points[i].t), py = toY(points[i].p);
        const bool selected = active && static_cast<int>(i) == curveSelected_;
        FillRound(D2D1::RectF(px - 5, py - 5, px + 5, py + 5), 5.0f, selected ? theme_.accent : theme_.cardBg);
        StrokeRound(D2D1::RectF(px - 5, py - 5, px + 5, py + 5), 5.0f, theme_.accent, 2.0f);
    }
    if (points.empty()) {
        // 没有曲线点时给出提示，而不是留一张空白图表让人以为坏了
        const float mid = (plot.top + plot.bottom) / 2;
        TextCentered(fc::T(L"曲线为空：点「添加点」，或直接在图里点一下"),
                     D2D1::RectF(plot.left, mid - 12, plot.right, mid + 12), fSmall_, theme_.textTertiary);
    }
}

void App::DrawPwmChart(float x, float y, float w) {
    const float h = 210.0f;
    D2D1_RECT_F card = D2D1::RectF(x, y, x + w, y + h);
    FillRound(card, 8.0f, theme_.cardBg);
    StrokeRound(card, 8.0f, theme_.cardBorder);
    Text(fc::T(L"输出 PWM"), D2D1::RectF(x + 16, y + 12, x + w - 260, y + 34), fBodyStrong_, theme_.textPrimary);
    Text(fc::T(L"最近 4 分钟"), D2D1::RectF(x + 16, y + 32, x + w - 260, y + 50), fSmall_, theme_.textTertiary);
    TextRight(Fmt(L"%d %%", snap_.pwm), D2D1::RectF(x + w - 176, y + 10, x + w - 16, y + 40), fH2_, theme_.pwmLine);

    D2D1_RECT_F plot = D2D1::RectF(x + 52, y + 58, x + w - 20, y + h - 26);
    for (int i = 0; i <= 4; ++i) {
        float gy = plot.top + (plot.bottom - plot.top) * i / 4.0f;
        Line(plot.left, gy, plot.right, gy, theme_.gridLine);
        TextRight(Fmt(L"%d", 100 - i * 25), D2D1::RectF(x + 8, gy - 10, plot.left - 10, gy + 10), fSmall_,
                  theme_.textTertiary);
    }
    if (pwmHist_.size() >= 2) {
        const float step = (plot.right - plot.left) / (kHistoryPoints - 1);
        for (size_t i = 1; i < pwmHist_.size(); ++i) {
            float x1 = plot.left + (i - 1) * step;
            float x2 = plot.left + i * step;
            float y1 = plot.bottom - static_cast<float>(pwmHist_[i - 1].second) / 100.0f * (plot.bottom - plot.top);
            float y2 = plot.bottom - static_cast<float>(pwmHist_[i].second) / 100.0f * (plot.bottom - plot.top);
            Line(x1, y1, x2, y2, theme_.pwmLine, 1.6f);
        }
    } else {
        const float mid = (plot.top + plot.bottom) / 2;
        TextCentered(fc::T(L"等待采样…"), D2D1::RectF(plot.left, mid - 12, plot.right, mid + 12), fSmall_,
                     theme_.textTertiary);
    }
}

void App::RecordCurveUndo() {
    curveUndo_.push_back(CurveUndo{ curve_, rpmCurve_ });
    while (curveUndo_.size() > 32) curveUndo_.pop_front();
}

void App::UndoCurve() {
    if (curveUndo_.empty()) return;
    CurveUndo snapshot = std::move(curveUndo_.back());
    curveUndo_.pop_back();
    curve_ = std::move(snapshot.temp);
    rpmCurve_ = std::move(snapshot.rpm);
    curveSelected_ = -1;
    dragIndex_ = -1;
    PushCurveToBackend(0);
    PushCurveToBackend(1);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

// 关于页按 1.3.0 的四块内容排：引用库 / 参考项目 / 开源地址 / 检查更新
void App::DrawAbout(float x, float y, float w) {
    const float top = y;
    Text(L"FanControl v1.4.0", D2D1::RectF(x, y, x + w, y + 34), fH2_, theme_.textPrimary);
    y += 42;

    struct LibRow { const wchar_t* name; const wchar_t* version; };
    const LibRow libs[] = {
        { L"LibreHardwareMonitorLib", L"0.9.6" },
        { L"CommunityToolkit.Mvvm", L"8.4.2" },
        { L"Microsoft.Extensions.Hosting", L"8.0.1" },
        { L"System.IO.Ports", L"10.0.3" },
        { L"System.Management", L"10.0.2" },
        { L"Windows App SDK", fc::T(L"Windows 通知") },
        { L".NET", L"8" },
    };
    // 卡片标题占掉顶部 44 DIP：以前从 y+4 开始排行，标题和第一行叠在一起
    const float headerH = 44.0f;
    const float libRowH = 30.0f;
    const int libCount = 7;
    float cardH = headerH + static_cast<float>(libCount) * libRowH + 8;
    Card(x, y, w, fc::T(L"引用库"), cardH);
    float ry = y + headerH;
    for (const auto& lib : libs) {
        Text(lib.name, D2D1::RectF(x + 16, ry + 7, x + w - 220, ry + 27), fBody_, theme_.textPrimary);
        TextRight(lib.version, D2D1::RectF(x + w - 210, ry, x + w - 16, ry + libRowH), fSmall_, theme_.textSecondary);
        ry += libRowH;
    }
    y += cardH + 16;

    const float linkH = 54.0f;
    cardH = headerH + 3 * linkH + 8;
    Card(x, y, w, fc::T(L"参考项目"), cardH);
    RowLink(x, y + headerH, w, linkH, L"G-Helper", fc::T(L"华硕设备控制参考"), L"https://github.com/seerge/g-helper");
    RowLink(x, y + headerH + linkH, w, linkH, fc::T(L"适用的开源硬件及参考"), fc::T(L"B 站视频"),
            L"https://www.bilibili.com/video/BV1Lr421M7u2/");
    RowLink(x, y + headerH + 2 * linkH, w, linkH, L"LibreHardwareMonitor", fc::T(L"硬件监控库"),
            L"https://github.com/LibreHardwareMonitor/LibreHardwareMonitor");
    y += cardH + 16;

    cardH = headerH + 2 * linkH + 8;
    Card(x, y, w, fc::T(L"开源地址"), cardH);
    RowLink(x, y + headerH, w, linkH, L"anlerway/Bluetooth-FanControl", fc::T(L"本仓库"),
            L"https://github.com/anlerway/Bluetooth-FanControl");
    RowLink(x, y + headerH + linkH, w, linkH, L"anlerway/ESP32-LaptopFan", fc::T(L"固件"),
            L"https://github.com/anlerway/ESP32-LaptopFan");
    y += cardH + 16;

    const bool newer = updateState_ == UpdateState::Newer && !updateUrl_.empty();
    cardH = newer ? 168.0f : 104.0f;
    Card(x, y, w, fc::T(L"检查更新"), cardH);
    const float bw = std::max(110.0f, MeasureText(fc::T(L"检查更新"), fBody_) + 32.0f);
    Chip(x + 16, y + headerH + 6, bw, 32, fc::T(L"检查更新"), false, L"act:checkupdate");
    std::wstring status;
    switch (updateState_) {
        case UpdateState::Checking: status = fc::T(L"正在检查更新…"); break;
        case UpdateState::Current: status = fc::T(L"已是最新版本"); break;
        case UpdateState::Newer: status = std::wstring(fc::T(L"发现新版本")) + L" " + updateTag_; break;
        case UpdateState::None: status = fc::T(L"仓库暂无发布版本"); break;
        case UpdateState::Failed:
            status = std::wstring(fc::T(L"检查更新失败：")) + updateDetail_;
            break;
        case UpdateState::Idle:
        default: status = fc::T(L"点击检查更新"); break;
    }
    Text(status, D2D1::RectF(x + 16 + bw + 12, y + headerH + 6, x + w - 16, y + headerH + 38), fSmall_,
         theme_.textSecondary);
    if (newer) RowLink(x, y + headerH + 48, w, 50, fc::T(L"打开下载页"), updateUrl_, updateUrl_);
    y += cardH;
    contentHeight_ = y + 24 - top;
}

// ---------------- 运行日志页 ----------------

int App::VisibleLogCount() const {
    if (logFilter_ < 0) return static_cast<int>(logs_.size());
    int count = 0;
    for (const fc::LogEntry& entry : logs_) {
        if (entry.level == logFilter_) ++count;
    }
    return count;
}

void App::AppendLogs(const std::vector<fc::LogEntry>& entries) {
    // 宿主按 seq 增量给：重连/重复请求都可能把同一批再送一次，靠 seq 去重
    for (const fc::LogEntry& entry : entries) {
        if (!logs_.empty() && entry.seq <= logs_.back().seq) continue;
        logs_.push_back(entry);
        if (entry.seq > logSeq_) logSeq_ = entry.seq;
    }
    while (logs_.size() > 2000) logs_.pop_front();
    if (logFollow_) logScroll_ = 1e9f;  // DrawLogs 会夹到底部
}

void App::EnterLogPage() {
    logNextPoll_ = 0;  // 进页面立刻拉一次，别等下一拍
}

void App::DrawLogs(float x, float y, float w) {
    const float rowH = 24.0f;
    float cx = x;
    auto filterChip = [&](const wchar_t* label, bool on, const std::wstring& id) {
        const float cw = std::max(56.0f, MeasureText(label, fSmall_) + 26.0f);
        Chip(cx, y, cw, 30.0f, label, on, id);
        cx += cw + 8.0f;
    };
    filterChip(fc::T(L"全部"), logFilter_ < 0, L"log:all");
    const wchar_t* levels[] = { L"提示", L"异常", L"错误", L"验证错误" };
    for (int i = 0; i < 4; ++i) filterChip(fc::T(levels[i]), logFilter_ == i, L"log:" + std::to_wstring(i));
    cx += 10.0f;
    filterChip(logPaused_ ? fc::T(L"继续") : fc::T(L"暂停"), logPaused_, L"log:pause");
    filterChip(fc::T(L"清空"), false, L"log:clear");
    filterChip(fc::T(L"打开日志目录"), false, L"log:open");

    const float top = y + 44.0f;
    const float cardH = std::max(200.0f, std::min(520.0f, viewH_ - top - 64.0f));
    D2D1_RECT_F card = D2D1::RectF(x, top, x + w, top + cardH);
    FillRound(card, 8.0f, theme_.cardBg);
    StrokeRound(card, 8.0f, theme_.cardBorder);
    logListRect_ = card;

    const int total = VisibleLogCount();
    const int rows = std::max(1, static_cast<int>((cardH - 16.0f) / rowH));
    const int maxFirst = std::max(0, total - rows);
    if (logFollow_) logScroll_ = static_cast<float>(maxFirst);
    logScroll_ = std::clamp(logScroll_, 0.0f, static_cast<float>(maxFirst));
    const int first = static_cast<int>(logScroll_);

    rt_->PushAxisAlignedClip(D2D1::RectF(card.left + 1, card.top + 1, card.right - 1, card.bottom - 1),
                             D2D1_ANTIALIAS_MODE_ALIASED);
    if (total == 0) {
        const wchar_t* empty = logPaused_ ? fc::T(L"已暂停") : fc::T(L"暂无日志");
        TextCentered(empty, D2D1::RectF(card.left, top + cardH * 0.5f - 12.0f, card.right, top + cardH * 0.5f + 12.0f),
                     fBody_, theme_.textTertiary);
    } else {
        int index = 0;
        int drawn = 0;
        for (const fc::LogEntry& entry : logs_) {
            if (logFilter_ >= 0 && entry.level != logFilter_) continue;
            if (index++ < first) continue;
            const float ry = top + 8.0f + drawn * rowH;
            if (ry + rowH > top + cardH) break;
            Text(entry.time, D2D1::RectF(x + 16.0f, ry + 4.0f, x + 96.0f, ry + rowH), fMono_, theme_.textTertiary);
            Text(SeverityLabel(entry.level), D2D1::RectF(x + 100.0f, ry + 4.0f, x + 180.0f, ry + rowH), fSmall_,
                 SeverityColor(theme_, entry.level));
            Text(entry.message, D2D1::RectF(x + 184.0f, ry + 4.0f, x + w - 28.0f, ry + rowH), fSmall_,
                 theme_.textPrimary);
            ++drawn;
        }
    }
    rt_->PopAxisAlignedClip();

    if (total > rows) {  // 细滚动条：只提示当前位置，不做拖拽
        const float trackTop = top + 8.0f;
        const float trackH = cardH - 16.0f;
        const float thumbH = std::max(24.0f, trackH * static_cast<float>(rows) / static_cast<float>(total));
        const float ratio = maxFirst == 0 ? 0.0f : logScroll_ / static_cast<float>(maxFirst);
        const float thumbY = trackTop + (trackH - thumbH) * ratio;
        FillRound(D2D1::RectF(x + w - 9.0f, thumbY, x + w - 4.0f, thumbY + thumbH), 2.5f, theme_.textTertiary);
    }

    // 底下这行说明：缓冲只在内存里，完整历史在日志文件
    Text(Fmt(fc::T(L"共 %d 条 · 内存中保留最近 2000 条%s"), total,
             logPaused_ ? fc::T(L"（已暂停刷新）") : L""),
         D2D1::RectF(x, top + cardH + 10.0f, x + w, top + cardH + 32.0f), fSmall_, theme_.textTertiary);
    contentHeight_ = top + cardH + 40.0f - y;
}

// ---------------- 首次运行向导 ----------------

void App::OpenSetupDialog() {
    setupOpen_ = true;
    setupLocation_ = userDataLocation_.empty() ? L"AppData" : userDataLocation_;
    setupPath_ = userDataPath_;
    setupLanguage_ = fc::GetLang() == fc::Lang::En ? L"en-US" : L"zh-CN";
}

void App::PickSetupFolder() {
    IFileOpenDialog* dialog = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog))) ||
        dialog == nullptr) {
        return;
    }
    DWORD options = 0;
    if (SUCCEEDED(dialog->GetOptions(&options))) {
        dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    }
    dialog->SetTitle(fc::T(L"选择配置和日志的存放位置"));
    if (SUCCEEDED(dialog->Show(hwnd_))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dialog->GetResult(&item)) && item != nullptr) {
            PWSTR path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path != nullptr) {
                setupPath_ = path;
                CoTaskMemFree(path);
            }
            item->Release();
        }
    }
    dialog->Release();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void App::FinishSetup() {
    if (setupLocation_ == L"Custom" && setupPath_.empty()) {
        PickSetupFolder();  // 还没选过文件夹：先让用户选，别拿着空路径落盘
        if (setupPath_.empty()) return;
    }
    setupOpen_ = false;
    setupDone_ = true;
    fc::CompleteSetup(setupLanguage_, setupLocation_, setupLocation_ == L"Custom" ? setupPath_ : std::wstring());
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void App::DrawSetupDialog(float w, float h) {
    if (!setupOpen_) return;
    FillRect(D2D1::RectF(0, 0, w, h), Col(0x000000, 0.5f));
    const float panelW = std::min(600.0f, w - 80.0f);
    const float panelH = 428.0f;
    const float left = (w - panelW) * 0.5f;
    const float top = std::max(20.0f, (h - panelH) * 0.5f);
    D2D1_RECT_F panel = D2D1::RectF(left, top, left + panelW, top + panelH);
    FillRound(panel, 10.0f, theme_.windowBg);
    StrokeRound(panel, 10.0f, theme_.cardBorder);
    const float innerLeft = left + 32.0f;
    const float innerRight = left + panelW - 32.0f;

    Text(fc::T(L"欢迎使用 FanControl"), D2D1::RectF(innerLeft, top + 26, innerRight, top + 60), fH2_,
         theme_.textPrimary);
    Text(fc::T(L"先选界面语言，再选配置和日志放在哪里；之后可以在设置页改。"),
         D2D1::RectF(innerLeft, top + 64, innerRight, top + 88), fSmall_, theme_.textSecondary);

    Text(fc::T(L"语言"), D2D1::RectF(innerLeft, top + 104, innerRight, top + 124), fBodyStrong_, theme_.textPrimary);
    struct LangChip { const wchar_t* label; const wchar_t* value; };
    const LangChip langs[] = { { L"中文", L"zh-CN" }, { L"English", L"en-US" } };
    float chipX = innerLeft;
    for (const LangChip& lang : langs) {
        const float cw = std::max(96.0f, MeasureText(lang.label, fBody_) + 36.0f);
        Chip(chipX, top + 130, cw, 32.0f, lang.label, setupLanguage_ == lang.value,
             std::wstring(L"setup:lang:") + lang.value);
        chipX += cw + 10.0f;
    }

    Text(fc::T(L"配置和日志的存放位置"), D2D1::RectF(innerLeft, top + 182, innerRight, top + 202), fBodyStrong_,
         theme_.textPrimary);
    struct LocRow { const wchar_t* value; const wchar_t* title; const wchar_t* hint; };
    const LocRow rows[] = {
        { L"AppData", L"应用数据目录", L"%APPDATA%\\FanControl" },
        { L"ExeDirectory", L"安装目录", L"程序目录下的 Userdata，适合免安装或 U 盘" },
        { L"Custom", L"自定义文件夹", L"点这里选择一个文件夹" },
    };
    float rowY = top + 208;
    for (const LocRow& row : rows) {
        const bool selected = setupLocation_ == row.value;
        D2D1_RECT_F r = D2D1::RectF(innerLeft, rowY, innerRight, rowY + 50);
        FillRound(r, 6.0f, selected ? theme_.navSelected : theme_.cardBg);
        if (selected) StrokeRound(r, 6.0f, theme_.accent);
        // 单选圆点：选中的画实心
        const float dotX = innerLeft + 16.0f;
        const float dotY = rowY + 25.0f;
        FillRound(D2D1::RectF(dotX, dotY - 7, dotX + 14, dotY + 7), 7.0f, selected ? theme_.accent : theme_.cardBorder);
        if (selected) FillRound(D2D1::RectF(dotX + 4, dotY - 3, dotX + 10, dotY + 3), 3.0f, theme_.windowBg);
        Text(fc::T(row.title), D2D1::RectF(dotX + 26, rowY + 6, innerRight - 16, rowY + 28), fBody_,
             theme_.textPrimary);
        std::wstring hint = fc::T(row.hint);
        if (row.value[0] == L'C' && selected && !setupPath_.empty()) hint = setupPath_;
        Text(hint, D2D1::RectF(dotX + 26, rowY + 26, innerRight - 16, rowY + 46), fSmall_, theme_.textTertiary);
        AddHit(r, std::wstring(L"setup:loc:") + row.value);
        rowY += 58.0f;
    }

    const float buttonY = top + panelH - 60.0f;
    Chip(innerRight - 132, buttonY, 132.0f, 36.0f, fc::T(L"开始使用"), true, L"setup:done");
    Text(fc::T(L"这些设置之后可以在设置页里改。"), D2D1::RectF(innerLeft, buttonY + 8, innerRight - 148, buttonY + 32),
         fSmall_, theme_.textTertiary);
}

// ---------------- 行内下拉（ComboBox） ----------------

namespace {
const wchar_t* TN(const wchar_t* zh) { return fc::T(zh); }
}  // namespace

void App::FillComboItems(const std::wstring& id) {
    auto add = [this](const std::wstring& label, const std::wstring& value) {
        comboItems_.push_back(ComboItem{ label, value });
    };
    if (id == L"set:theme") {
        add(TN(L"跟随系统"), L"System");
        add(TN(L"深色"), L"Dark");
        add(TN(L"浅色"), L"Light");
    } else if (id == L"set:lang") {
        add(L"简体中文", L"zh-CN");
        add(L"English", L"en-US");
    } else if (id == L"set:unit") {
        add(TN(L"摄氏度 °C"), L"Celsius");
        add(TN(L"华氏度 °F"), L"Fahrenheit");
    } else if (id == L"set:tempsrc") {
        add(L"ATK ACPI", L"AtkAcpi");
        add(L"WMI", L"Wmi");
        add(L"LibreHardwareMonitor", L"LibreHardwareMonitor");
        add(L"AIDA64", L"Aida64");
        add(L"Lenovo WMI", L"LenovoWmi");
    } else if (id == L"set:gpusrc") {
        add(L"NVIDIA nvidia-smi", L"NvidiaSmiAdl");
        add(L"LibreHardwareMonitor", L"LibreHardwareMonitor");
        add(L"ATK ACPI", L"AtkAcpi");
        add(L"AIDA64", L"Aida64");
        add(L"Lenovo WMI", L"LenovoWmi");
    } else if (id == L"set:fansrc") {
        add(L"ATK ACPI", L"AtkAcpi");
        add(L"WMI", L"Wmi");
        add(L"LibreHardwareMonitor", L"LibreHardwareMonitor");
        add(L"AIDA64", L"Aida64");
        add(L"Lenovo WMI", L"LenovoWmi");
    } else if (id == L"set:mode") {
        add(TN(L"手动 PWM"), L"Manual");
        add(TN(L"CPU 温度"), L"CpuTemp");
        add(TN(L"GPU 温度"), L"GpuTemp");
        add(TN(L"混合（取平均）"), L"MixedAvg");
        add(TN(L"混合（取高）"), L"Mixed");
        add(TN(L"CPU 风扇转速映射"), L"TargetRpm");
    } else if (id == L"set:comm") {
        add(TN(L"蓝牙 BLE"), L"Ble");
        add(TN(L"串口"), L"Com");
    } else if (id == L"set:baud") {
        for (int rate : { 9600, 19200, 38400, 57600, 115200, 230400 }) add(std::to_wstring(rate), std::to_wstring(rate));
    } else if (id == L"set:datadir") {
        add(TN(L"应用数据目录"), L"AppData");
        add(TN(L"安装目录"), L"ExeDirectory");
        // 已经选了自定义目录就把路径当条目名，浮层的勾选才不会落空
        add(userDataLocation_ == L"Custom" && !userDataPath_.empty() ? userDataPath_ : TN(L"自定义文件夹…"),
            L"Custom");
    } else if (id == L"set:gpusel") {
        add(TN(L"自动"), L"Auto");
        comboBusy_ = true;
        fc::ListGpus();
    } else if (id == L"set:comport") {
        comboBusy_ = true;
        fc::ListPorts();
    }
}

void App::OpenCombo(const std::wstring& id) {
    comboOpen_ = id;
    comboScroll_ = 0;
    comboItems_.clear();
    comboBusy_ = false;
    FillComboItems(id);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void App::CloseCombo() {
    if (comboOpen_.empty()) return;
    comboOpen_.clear();
    comboItems_.clear();
    comboBusy_ = false;
    InvalidateRect(hwnd_, nullptr, FALSE);
}

bool App::DismissOverlay() {
    if (bleDialogOpen_) {
        CloseBleDialog();
        return true;
    }
    if (!comboOpen_.empty()) {
        CloseCombo();
        return true;
    }
    return false;
}

// ---------------- 蓝牙设备弹窗 ----------------

void App::OpenBleDialog() {
    CloseCombo();  // 两个浮层同时开着，点击分发会打架
    bleDialogOpen_ = true;
    bleDialogScroll_ = 0;
    bleDialogItems_.clear();
    bleDialogBusy_ = true;
    // 每次打开都重新扫：风扇可能刚开机，上次的结果早就过期了
    fc::ScanBleDevices();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void App::CloseBleDialog() {
    if (!bleDialogOpen_) return;
    bleDialogOpen_ = false;
    bleDialogBusy_ = false;
    bleDialogItems_.clear();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void App::ChooseBleDevice(int index) {
    if (index < 0) {
        // 「未选择设备」：清掉 MAC 与名字，后台回到按名字搜索的老路
        config_.bleDeviceMac.clear();
        config_.bleDeviceName.clear();
        SendString("bleDeviceMac", L"");
        SendString("bleDeviceName", L"");
    } else {
        if (index >= static_cast<int>(bleDialogItems_.size())) return;
        const fc::DialogItem& item = bleDialogItems_[index];
        config_.bleDeviceMac = item.value;
        // label 与 value 相同说明后台只有 MAC，没有广播名
        config_.bleDeviceName = item.label == item.value ? std::wstring() : item.label;
        SendString("bleDeviceMac", item.value);
        SendString("bleDeviceName", config_.bleDeviceName);
    }
    CloseBleDialog();
}

void App::DrawBleDialog(float w, float h) {
    if (!bleDialogOpen_) return;
    // 模态遮罩：设置页退到后面，点遮罩等于取消
    FillRect(D2D1::RectF(0, 0, w, h), Col(0x000000, 0.45f));
    const int count = static_cast<int>(bleDialogItems_.size());
    const int rows = count + 1;  // 首行固定是「未选择设备」，用来清掉当前选择
    const int visible = std::min(rows, 7);
    const float rowH = 48.0f;
    const float panelW = std::min(560.0f, w - 96.0f);
    const float panelH = 64.0f + visible * rowH + 64.0f;
    const float left = (w - panelW) * 0.5f;
    const float top = (h - panelH) * 0.5f;
    const float listTop = top + 64.0f;
    D2D1_RECT_F panel = D2D1::RectF(left, top, left + panelW, top + panelH);
    FillRound(panel, 8.0f, theme_.cardBg);
    StrokeRound(panel, 8.0f, theme_.cardBorder);
    Text(fc::T(L"选择蓝牙设备"), D2D1::RectF(left + 20, top + 14, left + panelW - 20, top + 38), fH2_,
         theme_.textPrimary);
    Text(fc::T(L"风扇需开机并处于配对模式"), D2D1::RectF(left + 20, top + 38, left + panelW - 20, top + 56), fSmall_,
         theme_.textSecondary);

    const int first = std::clamp(bleDialogScroll_, 0, std::max(0, rows - visible));
    rt_->PushAxisAlignedClip(D2D1::RectF(left + 8, listTop, left + panelW - 8, listTop + visible * rowH),
                             D2D1_ANTIALIAS_MODE_ALIASED);
    for (int i = 0; i < visible; ++i) {
        const int index = first + i;
        if (index >= rows) break;
        const bool clear = index == 0;
        D2D1_RECT_F r = D2D1::RectF(left + 8, listTop + i * rowH, left + panelW - 8, listTop + (i + 1) * rowH);
        const std::wstring id = L"bledlg:" + std::to_wstring(clear ? -1 : index - 1);
        if (Hovered(id)) FillRound(r, 4.0f, theme_.controlHover);
        std::wstring title = clear ? fc::T(L"未选择设备") : bleDialogItems_[index - 1].label;
        std::wstring subtitle;
        bool selected = clear ? config_.bleDeviceMac.empty() : false;
        if (!clear) {
            const fc::DialogItem& item = bleDialogItems_[index - 1];
            if (item.label != item.value) subtitle = item.value;  // 有名字时第二行显示 MAC
            selected = !config_.bleDeviceMac.empty() && item.value == config_.bleDeviceMac;
        }
        Text(title, D2D1::RectF(r.left + 12, r.top + 6, r.right - 40, r.top + 26), fBody_,
             selected ? theme_.accent : theme_.textPrimary);
        if (!subtitle.empty())
            Text(subtitle, D2D1::RectF(r.left + 12, r.top + 24, r.right - 40, r.top + 43), fSmall_,
                 theme_.textSecondary);
        if (selected)
            Glyph(L"\xE73E", D2D1::RectF(r.right - 32, r.top + 14, r.right - 10, r.top + 34), 14.0f, theme_.accent);
        AddHit(r, id);
    }
    rt_->PopAxisAlignedClip();
    if (rows > visible) {
        // 只画位置提示，不做拖动条：候选最多十几个，滚轮够用
        const float trackX = left + panelW - 18.0f;
        const float trackTop = listTop + 4.0f;
        const float trackH = visible * rowH - 8.0f;
        FillRound(D2D1::RectF(trackX, trackTop, trackX + 4.0f, trackTop + trackH), 2.0f, theme_.controlBorder);
        const float barH = std::max(24.0f, trackH * visible / rows);
        const float t0 = static_cast<float>(first) / static_cast<float>(rows - visible);
        FillRound(D2D1::RectF(trackX, trackTop + (trackH - barH) * t0, trackX + 4.0f,
                              trackTop + (trackH - barH) * t0 + barH),
                  2.0f, theme_.textTertiary);
    }

    const float by = top + panelH - 52.0f;
    if (bleDialogBusy_) {
        Text(fc::T(L"扫描中…"), D2D1::RectF(left + 140, by + 6, left + panelW - 120, by + 30), fSmall_,
             theme_.textSecondary);
    } else if (count == 0) {
        Text(fc::T(L"没有扫描到设备"), D2D1::RectF(left + 140, by + 6, left + panelW - 120, by + 30), fSmall_,
             theme_.textSecondary);
    }
    Chip(left + 20, by, 96.0f, 32.0f, fc::T(L"重新扫描"), false, L"bledlg:scan");
    Chip(left + panelW - 104.0f, by, 84.0f, 32.0f, fc::T(L"关闭"), false, L"bledlg:close");
}

void App::ChooseComboItem(int index) {
    if (index < 0 || index >= static_cast<int>(comboItems_.size())) return;
    const std::wstring id = comboOpen_;
    const std::wstring value = comboItems_[index].value;
    CloseCombo();
    if (id == L"set:theme") {
        config_.theme = value;
        SendString("theme", value);
        if (!hasInitial_) {
            dark_ = value == L"Dark" ? true : value == L"Light" ? false : !SystemUsesLightTheme();
            theme_ = MakeTheme(dark_);
            BOOL useDark = dark_ ? TRUE : FALSE;
            DwmSetWindowAttribute(hwnd_, 20, &useDark, sizeof(useDark));
        }
    } else if (id == L"set:lang") {
        config_.language = value;
        fc::SetLang(value.rfind(L"en", 0) == 0 ? fc::Lang::En : fc::Lang::Zh);
        SendString("language", value);
    } else if (id == L"set:unit") {
        config_.temperatureUnit = value;
        SendString("temperatureUnit", value);
    } else if (id == L"set:tempsrc") {
        config_.temperatureSource = value;
        SendString("temperatureSource", value);
    } else if (id == L"set:gpusrc") {
        config_.gpuTemperatureSource = value;
        SendString("gpuTemperatureSource", value);
    } else if (id == L"set:fansrc") {
        config_.fanSpeedSource = value;
        SendString("fanSpeedSource", value);
    } else if (id == L"set:mode") {
        config_.fanControlMode = value;
        SendString("fanControlMode", value);
    } else if (id == L"set:comm") {
        config_.communicationType = value;
        SendString("communicationType", value);
    } else if (id == L"set:baud") {
        config_.comBaudRate = _wtoi(value.c_str());
        SendValue("comBaudRate", fc::WideToUtf8(value).c_str());
    } else if (id == L"set:datadir") {
        if (value == L"Custom") {
            const std::wstring previous = setupPath_;
            setupPath_.clear();
            PickSetupFolder();  // 复用向导里的文件夹选择器
            if (setupPath_.empty()) {
                setupPath_ = previous;
                InvalidateRect(hwnd_, nullptr, FALSE);
                return;
            }
            userDataLocation_ = L"Custom";
            userDataPath_ = setupPath_;
            fc::SetDataLocation(L"Custom", setupPath_);
        } else {
            userDataLocation_ = value;
            userDataPath_.clear();
            fc::SetDataLocation(value, std::wstring());
        }
    } else if (id == L"set:gpusel") {
        config_.gpuSelection = value;
        SendString("gpuSelection", value);
    } else if (id == L"set:comport") {
        config_.comPort = value;
        SendString("comPort", value);
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void App::DrawCombo(float w, float h) {
    if (comboOpen_.empty()) return;
    const float listW = 280.0f;
    const float rowH = 36.0f;
    const int count = static_cast<int>(comboItems_.size());
    const int shown = std::min(std::max(count, comboBusy_ ? 1 : 0), 8);
    const float listH = shown * rowH + 12.0f;
    float left = std::clamp(comboAnchor_.right - listW, 12.0f, w - listW - 12.0f);
    float top = comboAnchor_.bottom + 4.0f;
    if (top + listH > h - 12.0f) top = comboAnchor_.top - listH - 4.0f;
    D2D1_RECT_F box = D2D1::RectF(left, top, left + listW, top + listH);
    FillRound(box, 6.0f, theme_.cardBg);
    StrokeRound(box, 6.0f, theme_.controlBorder);
    if (comboBusy_ && count == 0) {
        TextCentered(fc::T(L"扫描中…"), D2D1::RectF(box.left, box.top + 6, box.right, box.top + 6 + rowH), fBody_,
                     theme_.textSecondary);
    } else if (count == 0) {
        TextCentered(fc::T(L"没有可选项"), D2D1::RectF(box.left, box.top + 6, box.right, box.top + 6 + rowH), fBody_,
                     theme_.textSecondary);
    }
    const std::wstring current = CurrentComboLabel(comboOpen_);
    const int first = std::clamp(comboScroll_, 0, std::max(0, count - 8));
    for (int i = 0; i < shown && first + i < count; ++i) {
        const int index = first + i;
        D2D1_RECT_F row = D2D1::RectF(box.left + 6, box.top + 6 + i * rowH, box.right - 6, box.top + 6 + (i + 1) * rowH);
        const std::wstring id = L"combo:" + std::to_wstring(index);
        const bool selected = !current.empty() && comboItems_[index].label == current;
        if (Hovered(id)) FillRound(row, 4.0f, theme_.controlHover);
        TextMiddle(comboItems_[index].label, D2D1::RectF(row.left + 10, row.top, row.right - 36, row.bottom), fBody_,
                   selected ? theme_.accent : theme_.textPrimary);
        if (selected)
            Glyph(L"\xE73E", D2D1::RectF(row.right - 30, row.top, row.right - 8, row.bottom), 12.0f, theme_.accent);
        AddHit(row, id);
    }
}

// ---------------- frame ----------------

void App::Paint() {
    if (!rt_ && !CreateTarget()) return;
    hits_.clear();
    sliders_.clear();
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
                                   fc::T(L"温度到输出 PWM 的映射"), fc::T(L"版本与运行信息"),
                                   fc::T(L"本次运行的提示、异常与错误") };
    Text(PageName(static_cast<int>(page_)), D2D1::RectF(cx, top, cx + cw, top + 40), fTitle_, theme_.textPrimary);
    Text(subtitles[static_cast<int>(page_)], D2D1::RectF(cx, top + 40, cx + cw, top + 62), fSmall_, theme_.textSecondary);

    float bodyY = top + 84;
    switch (page_) {
    case Page::Dashboard: DrawDashboard(cx, bodyY, cw); break;
    case Page::Settings: DrawSettings(cx, bodyY, cw); break;
    case Page::Curve: DrawCurve(cx, bodyY, cw); break;
    case Page::About: DrawAbout(cx, bodyY, cw); break;
    case Page::Log: DrawLogs(cx, bodyY, cw); break;
    }
    rt_->PopAxisAlignedClip();
    pageHeight_[static_cast<int>(page_)] = contentHeight_;
    contentHeight_ = pageHeight_[static_cast<int>(page_)];

    // 下拉浮层画在内容之上（不裁剪），点击外部由 OnLButtonDown 关掉
    DrawCombo(size.width, size.height);
    DrawBleDialog(size.width, size.height);
    DrawSetupDialog(size.width, size.height);

    if (!toast_.empty() && GetTickCount() < toastUntil_) {
        float tw = 300.0f;
        D2D1_RECT_F r = D2D1::RectF(size.width - tw - 24, size.height - 88, size.width - 24, size.height - 56);
        FillRound(r, 4.0f, dark_ ? Col(0x3A3A3A) : Col(0xE5E5E5));
        TextCentered(toast_, r, fSmall_, SeverityColor(theme_, toastSeverity_));
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
        // 后台未接上时用演示波形占位，接上后完全以后台数据为准
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
    push(rpmHist_, static_cast<double>(snap_.rpm));
}

// ---------------- input ----------------

void App::OnMouseMove(int x, int y) {
    mouseX_ = x;
    mouseY_ = y;
    float sx = x / scale_, sy = y / scale_;
    if (!sliderDrag_.empty()) {
        // 拖动只改本地值并重绘，命令在松手时发一次，避免每个像素都写盘
        // 值没变就不用重画：以前每个 WM_MOUSEMOVE 都整窗重绘，1px 抖动也会卡
        const double before = SliderRowValue(sliderDrag_);
        sliderValue_ = SliderValueAt(sliderDrag_, static_cast<int>(std::lround(sx)), before);
        ApplySliderValue(sliderDrag_, sliderValue_, false);
        hover_ = sliderDrag_;
        SetCursor(LoadCursorW(nullptr, IDC_HAND));
        // 只有量化后的值真的变了才重画：拖动时每像素重绘整窗是高 DPI 下卡顿的主因
        if (SliderRowValue(sliderDrag_) != before) InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }
    std::wstring previous = hover_;
    hover_.clear();
    // hits_ 按绘制顺序追加，下拉浮层与弹窗画在最后；反向遍历才能让「最后画的」优先命中，
    // 否则浮层条目所在位置的设置行会先命中，高亮永远落在背后那一行。
    for (auto it = hits_.rbegin(); it != hits_.rend(); ++it) {
        const Hit& h = *it;
        if (sx >= h.rect.left && sx <= h.rect.right && sy >= h.rect.top && sy <= h.rect.bottom) {
            // 弹窗打开时只认弹窗里的控件，否则遮罩后面的行会跟着亮
            if (bleDialogOpen_ && h.id.rfind(L"bledlg:", 0) != 0 && h.id.rfind(L"chip:bledlg:", 0) != 0) continue;
            hover_ = h.id;
            break;
        }
    }
    bool interactive = !hover_.empty();
    SetCursor(LoadCursorW(nullptr, interactive ? IDC_HAND : IDC_ARROW));

    if (dragIndex_ >= 0 && page_ == Page::Curve) {
        std::vector<Point2>& points = CurvePoints(curveActive_);
        const D2D1_RECT_F plot = CurvePlot(curveActive_);
        const double xMax = CurveXMax(curveActive_);
        const float t = (sx - plot.left) / (plot.right - plot.left) * static_cast<float>(xMax);
        const float p = (plot.bottom - sy) / (plot.bottom - plot.top) * 100.0f;
        points[dragIndex_].t = std::clamp(static_cast<double>(t), 0.0, xMax);
        points[dragIndex_].p = std::clamp(static_cast<double>(p), 0.0, 100.0);
        std::sort(points.begin(), points.end(), [](const Point2& a, const Point2& b) { return a.t < b.t; });
        curveDragMoved_ = true;
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

    // 首次运行向导最优先：命中只认向导里的控件，点遮罩不穿透也不关
    if (setupOpen_) {
        for (auto it = hits_.rbegin(); it != hits_.rend(); ++it) {
            const D2D1_RECT_F& r = it->rect;
            if (sx < r.left || sx > r.right || sy < r.top || sy > r.bottom) continue;
            const std::wstring& id = it->id;
            if (id == L"chip:setup:done") {
                FinishSetup();
            } else if (id.rfind(L"chip:setup:lang:", 0) == 0) {
                setupLanguage_ = id.substr(16);
                fc::SetLang(setupLanguage_.rfind(L"en", 0) == 0 ? fc::Lang::En : fc::Lang::Zh);
                InvalidateRect(hwnd_, nullptr, FALSE);
            } else if (id.rfind(L"setup:loc:", 0) == 0) {
                setupLocation_ = id.substr(10);
                if (setupLocation_ == L"Custom") PickSetupFolder();
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
            return;
        }
        return;
    }

    // 蓝牙弹窗是模态的：命中只认弹窗自己的控件，点遮罩或别的任何地方都当取消
    if (bleDialogOpen_) {
        for (auto it = hits_.rbegin(); it != hits_.rend(); ++it) {
            const D2D1_RECT_F& r = it->rect;
            if (sx < r.left || sx > r.right || sy < r.top || sy > r.bottom) continue;
            if (it->id.rfind(L"bledlg:", 0) == 0) {
                ChooseBleDevice(_wtoi(it->id.c_str() + 7));
            } else if (it->id == L"chip:bledlg:scan") {
                bleDialogBusy_ = true;
                bleDialogItems_.clear();
                bleDialogScroll_ = 0;
                fc::ScanBleDevices();
                InvalidateRect(hwnd_, nullptr, FALSE);
            } else {
                CloseBleDialog();
            }
            return;
        }
        CloseBleDialog();
        return;
    }

    // 下拉打开时：点条目就选中，点别处就收起（和系统 ComboBox 一样）
    if (!comboOpen_.empty()) {
        // 浮层画在最上层，命中也要从后往前：底下的行先入表，否则点到浮层会先命中被遮住的行。
        const std::wstring openId = comboOpen_;
        for (auto it = hits_.rbegin(); it != hits_.rend(); ++it) {
            const D2D1_RECT_F& r = it->rect;
            if (sx < r.left || sx > r.right || sy < r.top || sy > r.bottom) continue;
            if (it->id.rfind(L"combo:", 0) == 0) {
                ChooseComboItem(_wtoi(it->id.c_str() + 6));
                return;
            }
            // 再点一次同一个值框就是收起，别刚收起又立刻弹开
            if (it->id == openId) {
                CloseCombo();
                return;
            }
            break;
        }
        // 点到别处：先收起浮层，但这一下仍然按命中的控件处理（点另一个下拉就直接换成它）
        CloseCombo();
    }

    if (page_ == Page::Curve) {
        // 两张图都参与命中：哪张图上的圆点被按住，哪张就是当前编辑的曲线
        for (int which = 0; which < 2; ++which) {
            const std::vector<Point2>& points = CurvePoints(which);
            const D2D1_RECT_F plot = CurvePlot(which);
            const double xMax = CurveXMax(which);
            for (size_t i = 0; i < points.size(); ++i) {
                float px = plot.left + static_cast<float>(points[i].t / xMax) * (plot.right - plot.left);
                float py = plot.bottom - static_cast<float>(points[i].p / 100.0) * (plot.bottom - plot.top);
                if (std::abs(sx - px) < 12 && std::abs(sy - py) < 12) {
                    curveActive_ = which;
                    dragIndex_ = static_cast<int>(i);
                    curveSelected_ = static_cast<int>(i);
                    curveDragMoved_ = false;
                    RecordCurveUndo();  // 先记下拖动前的样子，没真拖动就在松手时扔掉
                    InvalidateRect(hwnd_, nullptr, FALSE);
                    return;
                }
            }
        }
        // 空白处点一下就在那里加一个点（1.3.0 曲线编辑器的手感）
        for (int which = 0; which < 2; ++which) {
            const D2D1_RECT_F plot = CurvePlot(which);
            if (sx >= plot.left && sx <= plot.right && sy >= plot.top && sy <= plot.bottom) {
                AddCurvePointAt(which, sx, sy);
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
            if (page_ == Page::Log) EnterLogPage();
            InvalidateRect(hwnd_, nullptr, FALSE);
        } else if (id.rfind(L"set:", 0) == 0) {
            if (IsSliderRow(id)) {
                // 滑块：按下即定位并进入拖拽状态，松手才落盘
                sliderDrag_ = id;
                sliderValue_ = SliderValueAt(id, static_cast<int>(std::lround(sx)), SliderRowValue(id));
                ApplySliderValue(id, sliderValue_, false);
                SetCapture(hwnd_);
                InvalidateRect(hwnd_, nullptr, FALSE);
            } else {
                HandleSettingClick(id, h.rect, static_cast<int>(std::lround(sx)));
            }
        } else if (id.rfind(L"url:", 0) == 0) {
            ShellExecuteW(nullptr, L"open", id.c_str() + 4, nullptr, nullptr, SW_SHOWNORMAL);
        } else if (id == L"chip:act:checkupdate") {
            updateState_ = UpdateState::Checking;
            updateTag_.clear();
            updateUrl_.clear();
            updateDetail_.clear();
            fc::CheckUpdate();
            InvalidateRect(hwnd_, nullptr, FALSE);
        } else if (id == L"act:reconnect") {
            fc::RequestReconnect();
            toast_.clear();  // 状态由下一帧 state 带回来，这里不假装成功
            InvalidateRect(hwnd_, nullptr, FALSE);
        } else if (id == L"settings:reload") {
            ReloadFromBackend();
        } else if (id == L"chip:curve:add") {
            const D2D1_RECT_F plot = CurvePlot(curveActive_);
            AddCurvePointAt(curveActive_, (plot.left + plot.right) * 0.5f, (plot.top + plot.bottom) * 0.5f);
        } else if (id == L"chip:curve:del") {
            std::vector<Point2>& points = CurvePoints(curveActive_);
            if (curveSelected_ >= 0 && curveSelected_ < static_cast<int>(points.size()) && points.size() > 2) {
                RecordCurveUndo();
                points.erase(points.begin() + curveSelected_);
                curveSelected_ = -1;
                PushCurveToBackend(curveActive_);
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
        } else if (id == L"set:logs" || id == L"chip:logs" || id == L"chip:log:open") {
            // 数据目录可以是自定义路径，日志目录只有宿主知道，界面不再自己拼 %APPDATA%
            fc::OpenLogFolder();
        } else if (id.rfind(L"chip:log:", 0) == 0) {
            const std::wstring action = id.substr(9);
            if (action == L"pause") {
                logPaused_ = !logPaused_;
                if (!logPaused_) logNextPoll_ = 0;  // 继续后立刻补拉暂停期间攒下的
            } else if (action == L"clear") {
                logs_.clear();
                logScroll_ = 0.0f;
                logFollow_ = true;
                fc::ClearLogs();
            } else if (action == L"all") {
                logFilter_ = -1;
                logScroll_ = 0.0f;
                logFollow_ = true;
            } else {
                logFilter_ = _wtoi(action.c_str());
                logScroll_ = 0.0f;
                logFollow_ = true;
            }
            InvalidateRect(hwnd_, nullptr, FALSE);
        } else if (id == L"chip:curve:undo") {
            UndoCurve();
        } else if (id == L"chip:release") {
            ShellExecuteW(nullptr, L"open", L"https://github.com/Anlerway/Bluetooth-FanControl/releases",
                          nullptr, nullptr, SW_SHOWNORMAL);
        } else if (id == L"chip:reset") {
            RecordCurveUndo();
            if (curveActive_ == 0) {
                curve_ = { {30, 20}, {50, 35}, {70, 60}, {90, 100} };
            } else {
                rpmCurve_ = { {0, 0}, {2000, 25}, {4000, 45}, {6000, 65}, {8000, 85}, {10000, 100} };
            }
            PushCurveToBackend(curveActive_);
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
        // 未登记的命中 id 直接忽略：以前这里会弹「命令已发送」把未实现的点击伪装成成功。
        return;
    }
}

void App::OnLButtonUp(int x, int) {
    if (!sliderDrag_.empty()) {
        // 用松手位置落盘：最后一次 WM_MOUSEMOVE 可能与松手点差几个像素
        sliderValue_ = SliderValueAt(sliderDrag_, x, sliderValue_);
        ApplySliderValue(sliderDrag_, sliderValue_, true);
        sliderDrag_.clear();
        ReleaseCapture();
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }
    if (dragIndex_ >= 0 && page_ == Page::Curve) {
        if (curveDragMoved_) {
            PushCurveToBackend(curveActive_);  // 松手才落盘：拖动过程中不写盘
        } else if (!curveUndo_.empty()) {
            curveUndo_.pop_back();  // 只是点中一个点、没有拖动，这一步不算改动
        }
    }
    dragIndex_ = -1;
    curveDragMoved_ = false;
}

void App::OnMouseWheel(int delta) {
    if (bleDialogOpen_) {
        const int rows = static_cast<int>(bleDialogItems_.size()) + 1;
        bleDialogScroll_ =
            std::clamp(bleDialogScroll_ - static_cast<int>(delta / 120), 0, std::max(0, rows - 7));
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }
    if (!comboOpen_.empty()) {  // 下拉浮层不随页面滚动，滚轮只在列表内部翻页
        const int count = static_cast<int>(comboItems_.size());
        comboScroll_ = std::clamp(comboScroll_ - static_cast<int>(delta / 120), 0, std::max(0, count - 8));
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }
    if (page_ == Page::Log) {  // 日志页自己在列表里翻页，不跟着整页滚
        const int count = VisibleLogCount();
        const float listH = std::max(0.0f, logListRect_.bottom - logListRect_.top - 16.0f);
        const int rows = std::max(1, static_cast<int>(listH / 24.0f));
        const int maxFirst = std::max(0, count - rows);
        if (maxFirst > 0) {
            logFollow_ = false;
            logScroll_ = std::clamp(logScroll_ - delta / 120.0f * 3.0f, 0.0f, static_cast<float>(maxFirst));
            if (logScroll_ >= static_cast<float>(maxFirst)) logFollow_ = true;
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }
    }
    float visible = std::max(120.0f, viewH_ - 96.0f);
    float maxScroll = std::max(0.0f, pageHeight_[static_cast<int>(page_)] - visible);
    scroll_ -= delta / 120.0f * 70.0f;
    scroll_ = std::clamp(scroll_, 0.0f, maxScroll);
}

void App::SetInitialPage(int index) {
    if (index >= 0 && index < 5) page_ = static_cast<Page>(index);
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

// ---------------- 与后台的交互（同进程）----------------

void App::SendValue(const char* key, const char* valueJson) {
    fc::SetValue(key, valueJson);
}

void App::OnIpcMessage(void* payload) {
    std::unique_ptr<fc::Message> message(static_cast<fc::Message*>(payload));
    if (!message) return;
    if (message->hasConfig) ApplyConfig(message->config);
    if (message->hasState) ApplyState(message->state);
    if (message->event == L"error") {
        // 后台只给 code + 参数，文案由界面按当前语言拼；前缀与颜色按级别（提示/异常/错误/验证错误）
        toastSeverity_ = message->severity;
        toast_ = std::wstring(SeverityLabel(message->severity)) + L"：" +
                 fc::ErrorText(message->errorCode, message->errorArg1, message->errorArg2, message->error);
        toastUntil_ = GetTickCount() + 4000;
    }
    if (message->event == L"bleDevices" && bleDialogOpen_) {
        // 弹窗自己收一份：切页/收起下拉都不该把候选清掉
        bleDialogItems_ = message->items;
        bleDialogBusy_ = false;
        bleDialogScroll_ = 0;
    }
    if (message->event == L"logs") {
        if (message->hasLogs) AppendLogs(message->logs);
        if (message->logLastSeq > logSeq_) logSeq_ = message->logLastSeq;
    }
    if (message->hasItems) {
        // GPU 列表是异步回来的：保留第一项「自动」，再把后台枚举到的卡接上
        const std::wstring openId = comboOpen_;
        comboItems_.clear();
        comboItems_.reserve(message->items.size() + 1);
        if (openId == L"set:gpusel") comboItems_.push_back(ComboItem{ fc::T(L"自动"), L"Auto" });
        for (const auto& item : message->items) comboItems_.push_back(ComboItem{ item.label, item.value });
        comboBusy_ = false;
    }
    if (message->hasAutostart) autostart_ = message->autostart;
    if (message->event == L"update") {
        if (message->updateState == L"checking") updateState_ = UpdateState::Checking;
        else if (message->updateState == L"current") updateState_ = UpdateState::Current;
        else if (message->updateState == L"newer") updateState_ = UpdateState::Newer;
        else if (message->updateState == L"none") updateState_ = UpdateState::None;
        else updateState_ = UpdateState::Failed;
        updateTag_ = message->updateTag;
        updateUrl_ = message->updateUrl;
        updateDetail_ = message->updateDetail;
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void App::ApplyConfig(const fc::Config& config) {
    config_ = config;
    hasConfig_ = true;
    // 语言跟着后台配置走：后台是唯一写盘方，换语言后重启界面仍然是同一种
    const fc::Lang lang = config.language.rfind(L"en", 0) == 0 ? fc::Lang::En : fc::Lang::Zh;
    if (lang != fc::GetLang()) {
        fc::SetLang(lang);
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
    smoothMs_ = std::clamp(static_cast<int>(config.pwmSmoothingWindowSeconds * 1000.0), 100, 3000);
    RestoreSliderValue();
    if (dragIndex_ < 0) {
        // 拖拽期间不覆盖用户正在编辑的曲线；其余时候按内容同步。
        // 以前只在点数不同时才拷贝，配置里点数恰好等于界面的假数据时，界面会一直显示那份假曲线。
        curve_.clear();
        for (const auto& point : config.curve) curve_.push_back(Point2{ point.x, point.y });
        rpmCurve_.clear();
        for (const auto& point : config.rpmCurve) rpmCurve_.push_back(Point2{ point.x, point.y });
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
    snap_.lastError = state.lastError;
    snap_.errorCode = state.errorCode;
    snap_.errorArg1 = state.errorArg1;
    snap_.errorArg2 = state.errorArg2;
    snap_.errorSeverity = state.errorSeverity;
    snap_.commError = state.commError;
    snap_.commErrorCode = state.commErrorCode;
    snap_.commErrorArg1 = state.commErrorArg1;
    snap_.commErrorArg2 = state.commErrorArg2;
    snap_.commErrorSeverity = state.commErrorSeverity;
    snap_.connectionState = state.connectionState;
    autostart_ = state.autostart;
    logEnabled_ = state.logEnabled;
    userDataLocation_ = state.userDataLocation;
    userDataPath_ = state.userDataPath;
    if (state.firstRun && !setupOpen_ && !setupDone_) OpenSetupDialog();
    // 日志页按 700 ms 增量轮询：state 的 400 ms 节拍不该跟着每帧发命令
    if (page_ == Page::Log && !logPaused_) {
        const unsigned long long now = GetTickCount64();
        if (now >= logNextPoll_) {
            logNextPoll_ = now + 700;
            fc::GetLogs(logSeq_);
        }
    }
    if (cpuHist_.empty()) {
        // 首次连上后台：用当前值铺满窗口，避免趋势图一开始只有左端一两个点
        for (int i = 0; i < kHistoryPoints; ++i) {
            cpuHist_.emplace_back(0.0, snap_.cpu);
            gpuHist_.emplace_back(0.0, snap_.gpu);
            pwmHist_.emplace_back(0.0, static_cast<double>(snap_.pwm));
            rpmHist_.emplace_back(0.0, static_cast<double>(snap_.rpm));
        }
    }
    if (state.timestamp - lastSampleMs_ >= 900) {
        lastSampleMs_ = state.timestamp;
        BuildHistory(1.0f);
    }
}

void App::PushCurveToBackend(int which) {
    const std::vector<Point2>& source = CurvePoints(which);
    std::vector<fc::CurvePoint> points;
    points.reserve(source.size());
    for (const auto& point : source) points.push_back(fc::CurvePoint{ point.t, point.p });
    fc::SetCurve(points, which != 0);
}

void App::SendString(const char* key, const std::wstring& value) {
    std::string quoted = "\"";
    for (char c : fc::WideToUtf8(value)) {
        if (c == '"' || c == '\\') quoted.push_back('\\');
        quoted.push_back(c);
    }
    quoted.push_back('"');
    SendValue(key, quoted.c_str());
}

void App::SendAutostart(bool enabled) {
    fc::SetAutostart(enabled);
}

std::wstring App::ModeLabel(const std::wstring& mode) const {
    if (mode == L"Manual") return fc::T(L"手动 PWM");
    if (mode == L"GpuTemp") return fc::T(L"GPU 温度");
    if (mode == L"Mixed") return fc::T(L"混合（取高）");
    if (mode == L"MixedAvg") return fc::T(L"混合（取平均）");
    if (mode == L"SystemFan") return fc::T(L"系统风扇");
    if (mode == L"TargetRpm") return fc::T(L"CPU 风扇转速映射");
    return fc::T(L"CPU 温度");
}

std::wstring App::DeviceLabel() const {
    if (config_.bleDeviceMac.empty()) return fc::T(L"未选择设备");
    // 只在没有名字时显示 MAC：名字 + MAC 一起太长，右对齐会被裁掉半个字。
    std::wstring label = config_.bleDeviceName.empty() ? config_.bleDeviceMac : config_.bleDeviceName;
    if (label.size() > 24) label = label.substr(0, 23) + L"…";
    return label;
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


std::wstring App::GpuSourceLabel() const {
    const std::wstring& source = config_.gpuTemperatureSource;
    if (source.empty() || source == L"NvidiaSmiAdl") return L"NVIDIA nvidia-smi";
    if (source == L"AtkAcpi") return L"ATK ACPI";
    if (source == L"Aida64") return L"AIDA64";
    if (source == L"LenovoWmi") return L"Lenovo WMI";
    return L"LibreHardwareMonitor";
}

std::wstring App::DataDirLabel() const {
    if (userDataLocation_ == L"Custom") {
        return userDataPath_.empty() ? fc::T(L"自定义文件夹") : userDataPath_;
    }
    return userDataLocation_ == L"ExeDirectory" ? fc::T(L"安装目录") : fc::T(L"应用数据目录");
}

std::wstring App::ConnectionLabel() const {
    if (snap_.connectionState == L"Connected") return fc::T(L"已连接");
    if (snap_.connectionState == L"Reconnecting") return fc::T(L"重连中");
    return fc::T(L"等待连接");
}

std::wstring App::CurrentComboLabel(const std::wstring& id) const {
    // label 必须与 FillComboItems 里写的一字不差，否则浮层里的勾选标记对不上。
    if (id == L"set:theme") return ThemeLabel();
    if (id == L"set:lang") return LangLabel();
    if (id == L"set:unit") return UnitLabel();
    if (id == L"set:tempsrc") return SourceLabel(config_.temperatureSource);
    if (id == L"set:gpusrc") return GpuSourceLabel();
    if (id == L"set:fansrc") return SourceLabel(config_.fanSpeedSource);
    if (id == L"set:mode") return ModeLabel(config_.fanControlMode);
    if (id == L"set:comm") return config_.communicationType == L"Com" ? fc::T(L"串口") : fc::T(L"蓝牙 BLE");
    if (id == L"set:baud") return std::to_wstring(config_.comBaudRate);
    if (id == L"set:datadir") return DataDirLabel();
    if (id == L"set:gpusel") return config_.gpuSelection.empty() || config_.gpuSelection == L"Auto" ? fc::T(L"自动") : config_.gpuSelection;
    if (id == L"set:comport") return config_.comPort;
    if (id == L"set:bledev") return DeviceLabel();
    return std::wstring();
}

void App::SendSystem(const char* key, const std::string& valueJson) {
    fc::SetSystem(key, valueJson);
}

void App::ReloadFromBackend() {
    fc::RequestState();
}

void App::HandleSettingClick(const std::wstring& id, D2D1_RECT_F rect, int clickX) {
    // 下拉行：把行矩形记成浮层锚点，列表内容由 FillComboItems 准备
    if (id == L"set:bledev") {
        OpenBleDialog();
        return;
    }
    static const wchar_t* kComboRows[] = { L"set:tempsrc", L"set:gpusrc",  L"set:gpusel", L"set:fansrc",
                                           L"set:mode",    L"set:comm",    L"set:comport", L"set:baud",
                                           L"set:theme",   L"set:lang",    L"set:unit",    L"set:datadir" };
    for (const wchar_t* row : kComboRows) {
        if (id == row) {
            comboAnchor_ = rect;
            OpenCombo(id);
            return;
        }
    }

    if (IsSliderRow(id)) {
        // 键盘/点击兜底路径：按下时没走拖拽分支（例如探针脚本直接点到轨道）也按位置取值
        ApplySliderValue(id, SliderValueAt(id, clickX, SliderRowValue(id)), true);
    } else if (id == L"set:autostart") {
        autostart_ = !autostart_;
        SendAutostart(autostart_);
    } else if (id == L"set:smoothenable") {
        config_.smoothingEnabled = !config_.smoothingEnabled;
        SendValue("smoothingEnabled", config_.smoothingEnabled ? "true" : "false");
    } else if (id == L"set:autoreconnect") {
        config_.autoReconnectBle = !config_.autoReconnectBle;
        SendValue("autoReconnectBle", config_.autoReconnectBle ? "true" : "false");
    } else if (id == L"set:notify") {
        config_.notifyOnBleDisconnect = !config_.notifyOnBleDisconnect;
        SendValue("notifyOnBleDisconnect", config_.notifyOnBleDisconnect ? "true" : "false");
    } else if (id == L"set:notifytemp") {
        config_.notifyOnTemperatureError = !config_.notifyOnTemperatureError;
        SendValue("notifyOnTemperatureError", config_.notifyOnTemperatureError ? "true" : "false");
    } else if (id == L"set:logenabled") {
        logEnabled_ = !logEnabled_;
        SendSystem("logEnabled", logEnabled_ ? "true" : "false");
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void App::AddCurvePointAt(int which, float sx, float sy) {
    const D2D1_RECT_F plot = CurvePlot(which);
    if (plot.right <= plot.left || plot.bottom <= plot.top) return;
    curveActive_ = which;
    std::vector<Point2>& points = CurvePoints(which);
    const double xMax = CurveXMax(which);
    const double t =
        std::clamp(static_cast<double>((sx - plot.left) / (plot.right - plot.left)) * xMax, 0.0, xMax);
    const double p =
        std::clamp(static_cast<double>((plot.bottom - sy) / (plot.bottom - plot.top)) * 100.0, 0.0, 100.0);
    for (const auto& point : points) {  // 太近就不再加点，避免两个圆重叠后拖不清
        if (std::abs(point.t - t) < xMax / 100.0) return;
    }
    RecordCurveUndo();
    points.push_back(Point2{ t, p });
    std::sort(points.begin(), points.end(), [](const Point2& a, const Point2& b) { return a.t < b.t; });
    curveSelected_ = -1;
    for (size_t i = 0; i < points.size(); ++i) {
        if (std::abs(points[i].t - t) < 1e-9) curveSelected_ = static_cast<int>(i);
    }
    PushCurveToBackend(which);
    InvalidateRect(hwnd_, nullptr, FALSE);
}


