#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d2d1.h>
#include <d2d1helper.h>
#include <dwrite.h>
#include <string>
#include <vector>
#include <deque>
#include <utility>
#include <cmath>
#include <cstdint>
#include <memory>
#include <mutex>
#include <atomic>
#include <thread>

#include "ipc.h"

inline D2D1_COLOR_F Col(uint32_t rgb, float a = 1.0f) {
    return D2D1::ColorF(D2D1::ColorF(((rgb >> 16) & 0xFF) / 255.0f, ((rgb >> 8) & 0xFF) / 255.0f,
                                     (rgb & 0xFF) / 255.0f, a));
}

struct Theme {
    D2D1_COLOR_F windowBg, sidebarBg, cardBg, cardBorder;
    D2D1_COLOR_F textPrimary, textSecondary, textTertiary;
    D2D1_COLOR_F accent, accentText, controlBg, controlBorder, controlHover;
    D2D1_COLOR_F navHover, navSelected, accentBar;
    D2D1_COLOR_F chartBg, gridLine, cpuLine, gpuLine, pwmLine, danger, ok;
};

Theme MakeTheme(bool dark);

struct Snapshot {
    double cpu = 46.0, gpu = 52.0;
    int pwm = 42, rpm = 1380;
    bool backendConnected = false;
    bool deviceConnected = true;
    std::wstring commMode = L"蓝牙 · LAPTOP-FAN";
    std::wstring lastError;
    std::wstring errorCode;
    std::wstring errorArg1;
    std::wstring errorArg2;
};

enum class Page { Dashboard = 0, Settings, Curve, About };

class App {
public:
    bool Init(HWND hwnd);
    void Release();
    void Resize(UINT pxWidth, UINT pxHeight);
    void Paint();
    void Tick();
    void OnMouseMove(int x, int y);
    void OnMouseLeave();
    void OnLButtonDown(int x, int y);
    void OnLButtonUp(int x, int y);
    void OnMouseWheel(int delta);
    void ToggleTheme();
    void SetInitialTheme(bool dark);
    void SetInitialPage(int index);
    void OnIpcMessage(void* payload);   // 接管 fc::Message* 的所有权
    void OnIpcStatus(bool connected);
    bool IsDark() const { return dark_; }
    float DpiScale() const { return scale_; }

private:
    struct Hit { D2D1_RECT_F rect; std::wstring id; };
    struct Point2 { double t; double p; };

    // device resources
    bool CreateTarget();
    void CreateFormats();
    void DropTarget();

    // drawing helpers (all coordinates are DIPs: 96 dpi logical units)
    void FillRect(D2D1_RECT_F r, D2D1_COLOR_F c);
    void FillRound(D2D1_RECT_F r, float radius, D2D1_COLOR_F c);
    void StrokeRound(D2D1_RECT_F r, float radius, D2D1_COLOR_F c, float width = 1.0f);
    void Text(const std::wstring& s, D2D1_RECT_F r, IDWriteTextFormat* fmt, D2D1_COLOR_F c);
    void TextCentered(const std::wstring& s, D2D1_RECT_F r, IDWriteTextFormat* fmt, D2D1_COLOR_F c);
    void TextRight(const std::wstring& s, D2D1_RECT_F r, IDWriteTextFormat* fmt, D2D1_COLOR_F c);
    void Glyph(const std::wstring& s, D2D1_RECT_F r, float size, D2D1_COLOR_F c);
    void Line(float x1, float y1, float x2, float y2, D2D1_COLOR_F c, float width = 1.0f);
    void AddHit(D2D1_RECT_F r, const std::wstring& id);
    bool Hovered(const std::wstring& id) const { return hover_ == id; }

    // pages
    void DrawSidebar(float w, float h);
    void DrawDashboard(float x, float y, float w);
    void DrawSettings(float x, float y, float w);
    void DrawCurve(float x, float y, float w);
    void DrawAbout(float x, float y, float w);

    // widgets
    float Card(float x, float y, float w, const std::wstring& title, float height);
    void RowButton(float x, float y, float w, float h, const std::wstring& id, const std::wstring& title,
                   const std::wstring& subtitle, const std::wstring& value);
    void RowToggle(float x, float y, float w, float h, const std::wstring& id, const std::wstring& title,
                   const std::wstring& subtitle, bool on);
    void RowSlider(float x, float y, float w, float h, const std::wstring& id, const std::wstring& title,
                   const std::wstring& subtitle, int value, int lo, int hi);
    void Switch(float x, float y, bool on);
    void Chip(float x, float y, float w, float h, const std::wstring& text, bool accent,
              const std::wstring& id = L"");

    void BuildHistory(float dt);

    HWND hwnd_ = nullptr;
    ID2D1Factory* d2d_ = nullptr;
    ID2D1HwndRenderTarget* rt_ = nullptr;
    IDWriteFactory* dw_ = nullptr;

    IDWriteTextFormat* fTitle_ = nullptr;
    IDWriteTextFormat* fH2_ = nullptr;
    IDWriteTextFormat* fBody_ = nullptr;
    IDWriteTextFormat* fBodyStrong_ = nullptr;
    IDWriteTextFormat* fSmall_ = nullptr;
    IDWriteTextFormat* fBig_ = nullptr;
    IDWriteTextFormat* fGlyph_ = nullptr;

    Theme theme_;
    bool dark_ = true;
    bool hasInitial_ = false;
    bool initialDark_ = true;
    float scale_ = 1.0f;
    Page page_ = Page::Dashboard;
    float scroll_ = 0.0f;
    float contentHeight_ = 0.0f;
    float pageHeight_[4] = { 0, 0, 0, 0 };
    float viewH_ = 0.0f;

    std::vector<Hit> hits_;
    std::wstring hover_;
    int mouseX_ = 0, mouseY_ = 0;

    Snapshot snap_;
    std::deque<std::pair<double, double>> cpuHist_;
    std::deque<std::pair<double, double>> gpuHist_;
    std::deque<std::pair<double, double>> pwmHist_;
    double t_ = 0.0;

    std::vector<Point2> curve_;
    D2D1_RECT_F curvePlot_{};
    int dragIndex_ = -1;
    int smoothMs_ = 800;
    bool autostart_ = true;
    bool notify_ = true;
    std::wstring toast_;
    DWORD toastUntil_ = 0;

    // ---- 与后台守护进程（命名管道）的交互 ----
    void StartIpc();
    void StopIpc();
    void SendValue(const char* key, const char* valueJson);
    void ApplyConfig(const fc::Config& config);
    void ApplyState(const fc::State& state);
    void PushCurveToBackend();
    void StartHostProcess();
    std::wstring ThemeLabel() const;
    std::wstring UnitLabel() const;
    std::wstring LangLabel() const;

    std::unique_ptr<fc::IpcClient> ipc_;
    std::thread ipcThread_;
    std::atomic<bool> ipcStop_{ false };
    std::atomic<bool> backendUp_{ false };
    std::mutex sendMutex_;
    fc::Config config_;
    bool hasConfig_ = false;
    bool launcherTried_ = false;
    long long lastSampleMs_ = 0;
};
