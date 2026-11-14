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
    Text(value, D2D1::RectF(box.left + 10, box.top, box.right - 26, box.bottom), fBody_, theme_.textPrimary);
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
        { fc::T(L"风扇 PWM"), Fmt(L"%d", snap_.pwm), L"%" },
        { fc::T(L"风扇转速"), snap_.rpm > 0 ? Fmt(L"%d", snap_.rpm) : L"—", L"RPM" },
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
    y += h + 14.0f;

    // 最近错误：与 1.3.0 一样只在真的出错时出现一行，不再单独占一张卡
    std::wstring recentError;
    if (!snap_.lastError.empty()) {
        recentError = fc::ErrorText(snap_.errorCode, snap_.errorArg1, snap_.errorArg2, snap_.lastError);
    } else if (!snap_.commError.empty()) {
        recentError = fc::ErrorText(snap_.commErrorCode, snap_.commErrorArg1, snap_.commErrorArg2, snap_.commError);
    }
    if (!recentError.empty()) {
        Text(std::wstring(fc::T(L"最近错误")) + L" · " + snap_.commMode + L" · " + recentError,
             D2D1::RectF(x + 4, y, x + w - 4, y + 22), fSmall_, theme_.danger);
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
    if (value == L"Simulated") return fc::T(L"模拟数据");
    if (value == L"Serial") return fc::T(L"串口");
    return value;
}
} // namespace

void App::DrawSettings(float x, float y, float w) {
    const float top = y;
    const float rowH = 64.0f;

    auto groupTitle = [&](const wchar_t* title) {
        Text(title, D2D1::RectF(x, y, x + w, y + 22), fSmall_, theme_.textSecondary);
        y += 26;
    };
    auto cardTop = [&](int rows) {
        Card(x, y, w, L"", rows * rowH + 8);
        return y + 4;
    };

    // 温度与风扇（对应 1.3.0 第一张卡）
    groupTitle(fc::T(L"温度与风扇"));
    {
        const bool manual = config_.fanControlMode == L"Manual";
        const int rows = 5 + (manual ? 1 : 0) + 1 + (config_.smoothingEnabled ? 1 : 0);
        float ry = cardTop(rows);
        RowCombo(x, ry, w, rowH, L"set:tempsrc", fc::T(L"温度来源"), fc::T(L"CPU 温度的读取方式"),
                 SourceLabel(config_.temperatureSource));
        ry += rowH;
        RowCombo(x, ry, w, rowH, L"set:gpusrc", fc::T(L"GPU 温度源"), fc::T(L"显卡温度的读取方式"),
                 GpuSourceLabel());
        ry += rowH;
        RowCombo(x, ry, w, rowH, L"set:gpusel", fc::T(L"GPU 选择"), fc::T(L"多显卡时指定读取哪一块"),
                 config_.gpuSelection == L"Auto" ? std::wstring(fc::T(L"自动")) : config_.gpuSelection);
        ry += rowH;
        RowCombo(x, ry, w, rowH, L"set:fansrc", fc::T(L"转速来源"), fc::T(L"风扇转速的读取方式"),
                 SourceLabel(config_.fanSpeedSource));
        ry += rowH;
        RowCombo(x, ry, w, rowH, L"set:mode", fc::T(L"控制模式"), fc::T(L"决定 PWM 由谁计算"),
                 ModeLabel(config_.fanControlMode));
        ry += rowH;
        if (manual) {
            RowSlider(x, ry, w, rowH, L"set:manualpwm", fc::T(L"手动 PWM"), fc::T(L"手动模式下直接指定占空比"),
                      config_.manualPwmPercent, 0, 100,
                      Fmt(L"%d %%", static_cast<int>(std::lround(config_.manualPwmPercent))));
            ry += rowH;
        }
        RowToggle(x, ry, w, rowH, L"set:smoothenable", fc::T(L"平滑输出"), fc::T(L"让 PWM 变化有过渡"),
                  config_.smoothingEnabled);
        ry += rowH;
        if (config_.smoothingEnabled) {
            RowSlider(x, ry, w, rowH, L"set:smooth", fc::T(L"平滑窗口"), fc::T(L"PWM 变化的过渡时间"),
                      config_.pwmSmoothingWindowSeconds, 0.5, 30.0,
                      Fmt(L"%.1f s", config_.pwmSmoothingWindowSeconds));
            ry += rowH;
        }
        y += rows * rowH + 8 + 20;
    }

    // 通信（对应 1.3.0 第二张卡）
    groupTitle(fc::T(L"通信"));
    {
        const int rows = 8;
        float ry = cardTop(rows);
        RowCombo(x, ry, w, rowH, L"set:comm", fc::T(L"通信方式"), fc::T(L"蓝牙 BLE 或串口"),
                 config_.communicationType == L"Com" ? fc::T(L"串口") : fc::T(L"蓝牙 BLE"));
        ry += rowH;
        RowCombo(x, ry, w, rowH, L"set:comport", fc::T(L"串口端口"), fc::T(L"与风扇相连的 COM 口"), config_.comPort);
        ry += rowH;
        RowCombo(x, ry, w, rowH, L"set:baud", fc::T(L"波特率"), fc::T(L"串口通信速率"),
                 std::to_wstring(config_.comBaudRate));
        ry += rowH;
        RowCombo(x, ry, w, rowH, L"set:bledev", fc::T(L"蓝牙设备"), fc::T(L"已配对的 NUS 风扇"), DeviceLabel());
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
        RowStatus(x, ry, w, rowH, L"set:connstate", fc::T(L"连接状态"), fc::T(L"设备链路"), ConnectionLabel());
        y += rows * rowH + 8 + 20;
    }

    // 系统（对应 1.3.0 第三张卡）
    groupTitle(fc::T(L"系统"));
    {
        const int rows = 9;
        float ry = cardTop(rows);
        RowSlider(x, ry, w, rowH, L"set:poll", fc::T(L"采样间隔"), fc::T(L"读取传感器的周期"),
                  config_.pollIntervalMilliseconds / 1000.0, 0.1, 5.0,
                  Fmt(L"%.1f s", config_.pollIntervalMilliseconds / 1000.0));
        ry += rowH;
        RowToggle(x, ry, w, rowH, L"set:autostart", fc::T(L"开机自启"), fc::T(L"登录后自动启动 FanControl"), autostart_);
        ry += rowH;
        RowCombo(x, ry, w, rowH, L"set:datadir", fc::T(L"数据目录"), fc::T(L"配置与日志的存放位置（重启后生效）"),
                 DataDirLabel());
        ry += rowH;
        RowToggle(x, ry, w, rowH, L"set:logenabled", fc::T(L"日志记录"), fc::T(L"把运行日志写入数据目录"), logEnabled_);
        ry += rowH;
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
        y += rows * rowH + 8 + 20;
    }

    // 重新加载：把界面上的值从后台再取一遍（1.3.0 的「重新加载」按钮）
    Chip(x, y, 120, 34, fc::T(L"重新加载"), false, L"settings:reload");
    y += 34 + 18;





    contentHeight_ = y - top + 24.0f;
}

void App::DrawCurve(float x, float y, float w) {
    const float h = 380.0f;
    D2D1_RECT_F card = D2D1::RectF(x, y, x + w, y + h);
    FillRound(card, 8.0f, theme_.cardBg);
    StrokeRound(card, 8.0f, theme_.cardBorder);

    // 两条曲线用页签切换：温度→PWM（风扇温度曲线）、转速→PWM（目标转速曲线）
    // 宽度按文字量：英文 “Temperature curve” 比中文长，固定 104 会溢出
    const float tabW = std::max({ 104.0f,
        MeasureText(fc::T(L"温度曲线"), fBody_) + 32.0f,
        MeasureText(fc::T(L"转速曲线"), fBody_) + 32.0f });
    Chip(x + 16, y + 12, tabW, 30, fc::T(L"温度曲线"), curveTab_ == 0, L"curve:temp");
    Chip(x + 24 + tabW, y + 12, tabW, 30, fc::T(L"转速曲线"), curveTab_ == 1, L"curve:rpm");
    Text(curveTab_ == 0 ? fc::T(L"温度 °C → PWM %") : fc::T(L"转速 RPM → PWM %"),
         D2D1::RectF(x + 40 + 2 * tabW, y + 16, x + w - 16, y + 40), fSmall_, theme_.textSecondary);

    std::vector<Point2>& points = curveTab_ == 0 ? curve_ : rpmCurve_;
    const double xMax = curveTab_ == 0 ? 120.0 : 10000.0;

    D2D1_RECT_F plot = D2D1::RectF(x + 56, y + 56, x + w - 32, y + h - 48);
    curvePlot_ = plot;
    for (int i = 0; i <= 5; ++i) {
        float gy = plot.top + (plot.bottom - plot.top) * i / 5.0f;
        Line(plot.left, gy, plot.right, gy, theme_.gridLine);
        TextRight(Fmt(L"%d", 100 - i * 20), D2D1::RectF(x + 8, gy - 10, plot.left - 10, gy + 10), fSmall_,
                  theme_.textTertiary);
        float gx = plot.left + (plot.right - plot.left) * i / 5.0f;
        Line(gx, plot.top, gx, plot.bottom, theme_.gridLine);
        TextCentered(Fmt(L"%d", static_cast<int>(std::lround(xMax * i / 5.0))),
                     D2D1::RectF(gx - 28, plot.bottom + 4, gx + 28, plot.bottom + 24), fSmall_, theme_.textTertiary);
    }
    TextCentered(curveTab_ == 0 ? fc::T(L"温度 °C") : fc::T(L"转速 RPM"),
                 D2D1::RectF(plot.left, plot.bottom + 26, plot.right, plot.bottom + 46), fSmall_,
                 theme_.textTertiary);

    auto toX = [&](double t) { return plot.left + static_cast<float>(t / xMax) * (plot.right - plot.left); };
    auto toY = [&](double p) { return plot.bottom - static_cast<float>(p / 100.0) * (plot.bottom - plot.top); };

    for (size_t i = 1; i < points.size(); ++i) {
        Line(toX(points[i - 1].t), toY(points[i - 1].p), toX(points[i].t), toY(points[i].p), theme_.accent, 2.0f);
    }
    for (size_t i = 0; i < points.size(); ++i) {
        float px = toX(points[i].t), py = toY(points[i].p);
        const bool selected = static_cast<int>(i) == curveSelected_;
        FillRound(D2D1::RectF(px - 6, py - 6, px + 6, py + 6), 6.0f, selected ? theme_.accent : theme_.cardBg);
        StrokeRound(D2D1::RectF(px - 6, py - 6, px + 6, py + 6), 6.0f, theme_.accent, 2.0f);
    }

    // 1.3.0 曲线页的四个按钮：添加点 / 删除点 / 重置 / 保存
    float by = y + h + 16;
    float bx = x;
    auto actionChip = [&](const wchar_t* label, bool accent, const wchar_t* id) {
        const float bw = std::max(88.0f, MeasureText(fc::T(label), fBody_) + 32.0f);
        Chip(bx, by, bw, 32, fc::T(label), accent, id);
        bx += bw + 8;
    };
    actionChip(L"添加点", false, L"curve:add");
    actionChip(L"删除点", false, L"curve:del");
    actionChip(L"重置", false, L"reset");
    actionChip(L"保存", true, L"apply");
    Text(fc::T(L"拖动圆点调整映射，点击空白处新增点"), D2D1::RectF(bx + 8, by, x + w, by + 32), fSmall_,
         theme_.textTertiary);
    contentHeight_ = by + 32 + 24 - y;
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
    } else if (id == L"set:gpusrc") {
        add(L"NVIDIA nvidia-smi", L"NvidiaSmiAdl");
        add(L"LibreHardwareMonitor", L"LibreHardwareMonitor");
        add(L"ATK ACPI", L"AtkAcpi");
        add(L"AIDA64", L"Aida64");
    } else if (id == L"set:fansrc") {
        add(L"ATK ACPI", L"AtkAcpi");
        add(L"WMI", L"Wmi");
        add(L"LibreHardwareMonitor", L"LibreHardwareMonitor");
        add(L"AIDA64", L"Aida64");
    } else if (id == L"set:mode") {
        add(TN(L"手动 PWM"), L"Manual");
        add(TN(L"CPU 温度"), L"CpuTemp");
        add(TN(L"GPU 温度"), L"GpuTemp");
        add(TN(L"混合（取平均）"), L"MixedAvg");
        add(TN(L"混合（取高）"), L"Mixed");
        add(TN(L"目标转速"), L"TargetRpm");
    } else if (id == L"set:comm") {
        add(TN(L"蓝牙 BLE"), L"Ble");
        add(TN(L"串口"), L"Com");
    } else if (id == L"set:baud") {
        for (int rate : { 9600, 19200, 38400, 57600, 115200, 230400 }) add(std::to_wstring(rate), std::to_wstring(rate));
    } else if (id == L"set:datadir") {
        add(TN(L"应用数据目录"), L"AppData");
        add(TN(L"安装目录"), L"ExeDirectory");
    } else if (id == L"set:gpusel") {
        add(TN(L"自动"), L"Auto");
        comboBusy_ = true;
        fc::ListGpus();
    } else if (id == L"set:comport") {
        comboBusy_ = true;
        fc::ListPorts();
    } else if (id == L"set:bledev") {
        comboBusy_ = true;
        fc::ScanBleDevices();
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
        userDataLocation_ = value;
        SendSystem("userDataLocation", std::string("\"") + fc::WideToUtf8(value) + "\"");
    } else if (id == L"set:gpusel") {
        config_.gpuSelection = value;
        SendString("gpuSelection", value);
    } else if (id == L"set:comport") {
        config_.comPort = value;
        SendString("comPort", value);
    } else if (id == L"set:bledev") {
        // value 是 MAC，label 是设备名（后台给同名项时 label == value）
        config_.bleDeviceMac = value;
        config_.bleDeviceName = comboItems_.size() > static_cast<size_t>(index) ? comboItems_[index].label : std::wstring();
        if (config_.bleDeviceName == value) config_.bleDeviceName.clear();
        SendString("bleDeviceMac", value);
        SendString("bleDeviceName", config_.bleDeviceName);
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
        Text(comboItems_[index].label, D2D1::RectF(row.left + 10, row.top, row.right - 36, row.bottom), fBody_,
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

    // 下拉浮层画在内容之上（不裁剪），点击外部由 OnLButtonDown 关掉
    DrawCombo(size.width, size.height);

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
}

// ---------------- input ----------------

void App::OnMouseMove(int x, int y) {
    mouseX_ = x;
    mouseY_ = y;
    float sx = x / scale_, sy = y / scale_;
    if (!sliderDrag_.empty()) {
        // 拖动只改本地值并重绘，命令在松手时发一次，避免每个像素都写盘
        sliderValue_ = SliderValueAt(sliderDrag_, static_cast<int>(std::lround(sx)), SliderRowValue(sliderDrag_));
        ApplySliderValue(sliderDrag_, sliderValue_, false);
        hover_ = sliderDrag_;
        SetCursor(LoadCursorW(nullptr, IDC_HAND));
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }
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
        std::vector<Point2>& points = curveTab_ == 0 ? curve_ : rpmCurve_;
        const double xMax = curveTab_ == 0 ? 120.0 : 10000.0;
        const float t = (sx - curvePlot_.left) / (curvePlot_.right - curvePlot_.left) * static_cast<float>(xMax);
        const float p = (curvePlot_.bottom - sy) / (curvePlot_.bottom - curvePlot_.top) * 100.0f;
        points[dragIndex_].t = std::clamp(static_cast<double>(t), 0.0, xMax);
        points[dragIndex_].p = std::clamp(static_cast<double>(p), 0.0, 100.0);
        std::sort(points.begin(), points.end(), [](const Point2& a, const Point2& b) { return a.t < b.t; });
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
        const std::vector<Point2>& points = curveTab_ == 0 ? curve_ : rpmCurve_;
        const double xMax = curveTab_ == 0 ? 120.0 : 10000.0;
        for (size_t i = 0; i < points.size(); ++i) {
            float px = curvePlot_.left + static_cast<float>(points[i].t / xMax) * (curvePlot_.right - curvePlot_.left);
            float py = curvePlot_.bottom - static_cast<float>(points[i].p / 100.0) * (curvePlot_.bottom - curvePlot_.top);
            if (std::abs(sx - px) < 12 && std::abs(sy - py) < 12) {
                dragIndex_ = static_cast<int>(i);
                curveSelected_ = static_cast<int>(i);
                InvalidateRect(hwnd_, nullptr, FALSE);
                return;
            }
        }
        // 空白处点一下就在那里加一个点（1.3.0 曲线编辑器的手感）
        if (sx >= curvePlot_.left && sx <= curvePlot_.right && sy >= curvePlot_.top && sy <= curvePlot_.bottom) {
            AddCurvePointAt(sx, sy);
            return;
        }
    }
    for (auto& h : hits_) {
        if (sx < h.rect.left || sx > h.rect.right || sy < h.rect.top || sy > h.rect.bottom) continue;
        const std::wstring& id = h.id;
        if (id.rfind(L"nav:", 0) == 0) {
            page_ = static_cast<Page>(_wtoi(id.c_str() + 4));
            scroll_ = 0.0f;
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
        } else if (id == L"chip:curve:temp" || id == L"chip:curve:rpm") {
            curveTab_ = (id == L"chip:curve:rpm") ? 1 : 0;
            dragIndex_ = -1;
            InvalidateRect(hwnd_, nullptr, FALSE);
        } else if (id == L"act:reconnect") {
            fc::RequestReconnect();
            toast_.clear();  // 状态由下一帧 state 带回来，这里不假装成功
            InvalidateRect(hwnd_, nullptr, FALSE);
        } else if (id == L"settings:reload") {
            ReloadFromBackend();
        } else if (id == L"curve:add") {
            AddCurvePointAt(curvePlot_.left + (curvePlot_.right - curvePlot_.left) * 0.5f,
                            curvePlot_.top + (curvePlot_.bottom - curvePlot_.top) * 0.5f);
        } else if (id == L"curve:del") {
            std::vector<Point2>& points = curveTab_ == 0 ? curve_ : rpmCurve_;
            if (curveSelected_ >= 0 && curveSelected_ < static_cast<int>(points.size()) && points.size() > 2) {
                points.erase(points.begin() + curveSelected_);
                curveSelected_ = -1;
                PushCurveToBackend();
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
        } else if (id == L"set:logs" || id == L"chip:logs") {
            ShellExecuteW(nullptr, L"open", L"explorer.exe", L"%APPDATA%\\FanControl\\Logs", nullptr, SW_SHOWNORMAL);
        } else if (id == L"chip:apply") {
            PushCurveToBackend();
            InvalidateRect(hwnd_, nullptr, FALSE);
        } else if (id == L"chip:release") {
            ShellExecuteW(nullptr, L"open", L"https://github.com/Anlerway/Bluetooth-FanControl/releases",
                          nullptr, nullptr, SW_SHOWNORMAL);
        } else if (id == L"chip:reset") {
            if (curveTab_ == 0) {
                curve_ = { {30, 20}, {50, 35}, {70, 60}, {90, 100} };
            } else {
                rpmCurve_ = { {0, 0}, {2000, 25}, {4000, 45}, {6000, 65}, {8000, 85}, {10000, 100} };
            }
            PushCurveToBackend();
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
        // 未登记的命中 id 直接忽略：以前这里会弹「命令已发送」把未实现的点击伪装成成功。
        return;
    }
}

void App::OnLButtonUp(int, int) {
    if (!sliderDrag_.empty()) {
        ApplySliderValue(sliderDrag_, sliderValue_, true);
        sliderDrag_.clear();
        ReleaseCapture();
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }
    if (dragIndex_ >= 0 && page_ == Page::Curve) PushCurveToBackend();
    dragIndex_ = -1;
}

void App::OnMouseWheel(int delta) {
    if (!comboOpen_.empty()) {  // 下拉浮层不随页面滚动，滚轮只在列表内部翻页
        const int count = static_cast<int>(comboItems_.size());
        comboScroll_ = std::clamp(comboScroll_ - static_cast<int>(delta / 120), 0, std::max(0, count - 8));
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }
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
        // 后台只给 code + 参数，文案由界面按当前语言拼
        toast_ = fc::ErrorText(message->errorCode, message->errorArg1, message->errorArg2, message->error);
        toastUntil_ = GetTickCount() + 4000;
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
    RestoreSliderValue();
    if (dragIndex_ < 0) {
        // 拖拽期间不覆盖用户正在编辑的曲线
        if (first || curve_.size() != config.curve.size()) {
            curve_.clear();
            for (const auto& point : config.curve) curve_.push_back(Point2{ point.x, point.y });
        }
        if (first || rpmCurve_.size() != config.rpmCurve.size()) {
            rpmCurve_.clear();
            for (const auto& point : config.rpmCurve) rpmCurve_.push_back(Point2{ point.x, point.y });
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
    snap_.lastError = state.lastError;
    snap_.errorCode = state.errorCode;
    snap_.errorArg1 = state.errorArg1;
    snap_.errorArg2 = state.errorArg2;
    snap_.commError = state.commError;
    snap_.commErrorCode = state.commErrorCode;
    snap_.commErrorArg1 = state.commErrorArg1;
    snap_.commErrorArg2 = state.commErrorArg2;
    snap_.connectionState = state.connectionState;
    autostart_ = state.autostart;
    logEnabled_ = state.logEnabled;
    userDataLocation_ = state.userDataLocation;
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
    const std::vector<Point2>& source = curveTab_ == 0 ? curve_ : rpmCurve_;
    std::vector<fc::CurvePoint> points;
    points.reserve(source.size());
    for (const auto& point : source) points.push_back(fc::CurvePoint{ point.t, point.p });
    fc::SetCurve(points, curveTab_ != 0);
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
    if (mode == L"TargetRpm") return fc::T(L"目标转速");
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
    return L"LibreHardwareMonitor";
}

std::wstring App::DataDirLabel() const {
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
    static const wchar_t* kComboRows[] = { L"set:tempsrc", L"set:gpusrc",  L"set:gpusel", L"set:fansrc",
                                           L"set:mode",    L"set:comm",    L"set:comport", L"set:baud",
                                           L"set:bledev",  L"set:theme",   L"set:lang",    L"set:unit",
                                           L"set:datadir" };
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

void App::AddCurvePointAt(float sx, float sy) {
    if (curvePlot_.right <= curvePlot_.left || curvePlot_.bottom <= curvePlot_.top) return;
    std::vector<Point2>& points = curveTab_ == 0 ? curve_ : rpmCurve_;
    const double xMax = curveTab_ == 0 ? 120.0 : 10000.0;
    const double t = std::clamp(static_cast<double>((sx - curvePlot_.left) / (curvePlot_.right - curvePlot_.left)) * xMax,
                                0.0, xMax);
    const double p =
        std::clamp(static_cast<double>((curvePlot_.bottom - sy) / (curvePlot_.bottom - curvePlot_.top)) * 100.0, 0.0, 100.0);
    for (const auto& point : points) {  // 太近就不再加点，避免两个圆重叠后拖不清
        if (std::abs(point.t - t) < xMax / 100.0) return;
    }
    points.push_back(Point2{ t, p });
    std::sort(points.begin(), points.end(), [](const Point2& a, const Point2& b) { return a.t < b.t; });
    curveSelected_ = -1;
    for (size_t i = 0; i < points.size(); ++i) {
        if (std::abs(points[i].t - t) < 1e-9) curveSelected_ = static_cast<int>(i);
    }
    PushCurveToBackend();
    InvalidateRect(hwnd_, nullptr, FALSE);
}


