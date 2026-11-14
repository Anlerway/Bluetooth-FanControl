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
#include <unordered_map>
#include <algorithm>
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
    D2D1_COLOR_F chartBg, gridLine, cpuLine, gpuLine, pwmLine, rpmLine, danger, warning, validation, ok;
};

Theme MakeTheme(bool dark);

struct Snapshot {
    double cpu = 46.0, gpu = 52.0;
    int pwm = 42, rpm = 1380;
    bool backendConnected = false;
    bool deviceConnected = true;
    std::wstring commMode = L"蓝牙 · LAPTOP-FAN";
    std::wstring connectionState = L"Waiting";  // Waiting / Connected / Reconnecting
    std::wstring lastError;
    std::wstring errorCode;
    std::wstring errorArg1;
    std::wstring errorArg2;
    int errorSeverity = 2;  // 0 提示 / 1 异常 / 2 错误 / 3 验证错误
    std::wstring commError;
    std::wstring commErrorCode;
    std::wstring commErrorArg1;
    std::wstring commErrorArg2;
    int commErrorSeverity = 2;
};

enum class Page { Dashboard = 0, Settings, Curve, About, Log };

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
    // Esc / 关闭动作：先收弹窗、再收下拉，返回是否真的收掉了东西
    bool DismissOverlay();
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
    // 单行文字垂直居中：下拉框的值与浮层条目用它，Text() 是顶对齐，30px 高的框里看着偏上
    void TextMiddle(const std::wstring& s, D2D1_RECT_F r, IDWriteTextFormat* fmt, D2D1_COLOR_F c);
    void Glyph(const std::wstring& s, D2D1_RECT_F r, float size, D2D1_COLOR_F c);
    void Line(float x1, float y1, float x2, float y2, D2D1_COLOR_F c, float width = 1.0f);
    void AddHit(D2D1_RECT_F r, const std::wstring& id);
    bool Hovered(const std::wstring& id) const { return hover_ == id; }
    // 颜色刷按 RGBA 缓存：每画一条线/一段文字都 CreateSolidColorBrush + Release 会在
    // 一帧里造出上百个瞬时 COM 对象，私有内存和分配器碎片都白涨。
    ID2D1SolidColorBrush* Brush(const D2D1_COLOR_F& c);

    // pages
    void DrawSidebar(float w, float h);
    void DrawDashboard(float x, float y, float w);
    void DrawSettings(float x, float y, float w);
    void DrawCurve(float x, float y, float w);
    // 曲线页的两张映射图：which 0=温度曲线 1=转速曲线，并排平铺（不再用页签切换）
    void DrawCurvePanel(float x, float y, float w, float h, int which, const std::wstring& title,
                        const std::wstring& axisLabel, double xMax);
    void DrawPwmChart(float x, float y, float w);
    std::vector<Point2>& CurvePoints(int which);
    const std::vector<Point2>& CurvePoints(int which) const;
    D2D1_RECT_F CurvePlot(int which) const;
    static double CurveXMax(int which) { return which == 0 ? 120.0 : 10000.0; }
    struct CurveUndo { std::vector<Point2> temp; std::vector<Point2> rpm; };
    void RecordCurveUndo();
    void UndoCurve();
    void DrawAbout(float x, float y, float w);
    // 运行日志页：宿主内存环形缓冲的增量视图（进页面时拉、之后每 700 ms 拉一次）
    void DrawLogs(float x, float y, float w);
    void EnterLogPage();
    void AppendLogs(const std::vector<fc::LogEntry>& entries);
    int VisibleLogCount() const;

    // 行内下拉（1.3.0 里这些字段都是 ComboBox）：点行 → 在行下方浮出列表，不再弹模态窗。
    struct ComboItem { std::wstring label; std::wstring value; };
    void DrawCombo(float w, float h);
    void OpenCombo(const std::wstring& id);
    void CloseCombo();
    void ChooseComboItem(int index);
    void FillComboItems(const std::wstring& id);
    // 下拉当前选中项的文字（用于在浮层里打勾）：直接从 config_ 推，避免另存一份状态。
    std::wstring CurrentComboLabel(const std::wstring& id) const;

    // 蓝牙设备选择：1.3.0 是独立窗口，这里在同一个 HWND 里画模态面板（遮罩 + 居中列表），
    // 免得为了一个列表再养一个窗口类和一套消息循环。
    void OpenBleDialog();
    void CloseBleDialog();
    void DrawBleDialog(float w, float h);
    void ChooseBleDevice(int index);  // -1 = 清除当前选择

    // 首次运行向导：语言 + 数据（配置与日志）存放位置，确认后一次性交给宿主
    void OpenSetupDialog();
    void DrawSetupDialog(float w, float h);
    void PickSetupFolder();
    void FinishSetup();

    // widgets
    float Card(float x, float y, float w, const std::wstring& title, float height);
    void RowButton(float x, float y, float w, float h, const std::wstring& id, const std::wstring& title,
                   const std::wstring& subtitle, const std::wstring& value);
    void RowToggle(float x, float y, float w, float h, const std::wstring& id, const std::wstring& title,
                   const std::wstring& subtitle, bool on);
    // 值用 double：平滑窗口 0.5–30 s、采样间隔 0.1–5.0 s 都需要小数，显示文本由调用方给。
    void RowSlider(float x, float y, float w, float h, const std::wstring& id, const std::wstring& title,
                   const std::wstring& subtitle, double value, double lo, double hi,
                   const std::wstring& valueText);
    // 下拉行：右侧「当前值 + 箭头」，点开由 DrawCombo 画浮层
    void RowCombo(float x, float y, float w, float h, const std::wstring& id, const std::wstring& title,
                  const std::wstring& subtitle, const std::wstring& value);
    // 连接状态行：右侧是值 + 一个「重新连接」小按钮
    void RowStatus(float x, float y, float w, float h, const std::wstring& id, const std::wstring& title,
                   const std::wstring& subtitle, const std::wstring& value);
    // 关于页的链接行：标题 + 说明 + 可点的 URL（点击走 url:<href> 命中 id）
    void RowLink(float x, float y, float w, float h, const std::wstring& title, const std::wstring& subtitle,
                 const std::wstring& url);
    // 滑块几何：立即模式绘制时记下轨道位置，点击时按 x 反算取值（点哪里就是多少，而不是循环跳档）。
    struct SliderGeom {
        float trackL = 0;
        float trackR = 0;
        double lo = 0;
        double hi = 100;
    };
    double SliderValueAt(const std::wstring& id, int clickX, double fallback) const;
    // 拖动中的滑块：按下只记 id，移动时连续改本地值，松手才发命令（1.3.0 的 Slider 手感）
    bool IsSliderRow(const std::wstring& id) const;
    double SliderRowValue(const std::wstring& id) const;
    void ApplySliderValue(const std::wstring& id, double value, bool send);
    void RestoreSliderValue();
    std::unordered_map<std::wstring, SliderGeom> sliders_;

    void Switch(float x, float y, bool on);
    void Chip(float x, float y, float w, float h, const std::wstring& text, bool accent,
              const std::wstring& id = L"");
    // 文本渲染宽度（DIP）。英文标签明显比中文长，固定宽度会溢出，按钮宽度按它算。
    float MeasureText(const std::wstring& text, IDWriteTextFormat* format) const;

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
    IDWriteTextFormat* fMono_ = nullptr;  // 日志时间列等宽
    std::unordered_map<uint32_t, ID2D1SolidColorBrush*> brushes_;
    std::unordered_map<int, IDWriteTextFormat*> glyphFormats_;
    mutable std::unordered_map<std::wstring, float> measureCache_;

    Theme theme_;
    bool dark_ = true;
    bool hasInitial_ = false;
    bool initialDark_ = true;
    float scale_ = 1.0f;
    Page page_ = Page::Dashboard;
    float scroll_ = 0.0f;
    float contentHeight_ = 0.0f;
    float pageHeight_[5] = { 0, 0, 0, 0, 0 };
    float viewH_ = 0.0f;

    std::vector<Hit> hits_;
    std::wstring hover_;
    int mouseX_ = 0, mouseY_ = 0;

    Snapshot snap_;
    std::deque<std::pair<double, double>> cpuHist_;
    std::deque<std::pair<double, double>> gpuHist_;
    std::deque<std::pair<double, double>> pwmHist_;
    std::deque<std::pair<double, double>> rpmHist_;
    double t_ = 0.0;

    std::vector<Point2> curve_;
    std::vector<Point2> rpmCurve_;
    int curveActive_ = 0;  // 当前编辑哪条曲线：0=温度曲线 1=转速曲线（点哪张图就是哪条）
    D2D1_RECT_F curvePlot_{};
    D2D1_RECT_F rpmPlot_{};
    int dragIndex_ = -1;
    bool curveDragMoved_ = false;
    // 曲线改动即写盘，没有「保存」按钮；撤回栈存整份快照（两张图一起回退），最多 32 步
    std::deque<CurveUndo> curveUndo_;
    int smoothMs_ = 800;
    bool autostart_ = false;
    bool notify_ = true;

    std::wstring comboOpen_;
    int comboScroll_ = 0;  // 打开中的下拉 id，空 = 全关
    std::vector<ComboItem> comboItems_;
    bool comboBusy_ = false;
    D2D1_RECT_F comboAnchor_{};
    // 蓝牙设备弹窗状态（列表内容来自宿主的 scanBle 结果）
    bool bleDialogOpen_ = false;
    bool bleDialogBusy_ = false;
    int bleDialogScroll_ = 0;
    std::vector<fc::DialogItem> bleDialogItems_;
    // 曲线页当前选中的点（删除点用），-1 = 没有
    int curveSelected_ = -1;
    // SystemConfig 侧状态
    bool logEnabled_ = true;
    std::wstring userDataLocation_ = L"AppData";
    std::wstring userDataPath_;
    // 运行日志页状态
    std::deque<fc::LogEntry> logs_;
    long long logSeq_ = 0;
    int logFilter_ = -1;       // -1 = 全部，其余按级别过滤
    bool logPaused_ = false;
    bool logFollow_ = true;    // 停在底部时自动跟着新条目走
    float logScroll_ = 0.0f;
    D2D1_RECT_F logListRect_{};
    unsigned long long logNextPoll_ = 0;
    // 首次运行向导状态
    bool setupOpen_ = false;
    bool setupDone_ = false;  // 本次会话里点过「开始使用」：宿主中途推的旧状态别再弹一次
    std::wstring setupLocation_ = L"AppData";
    std::wstring setupPath_;
    std::wstring setupLanguage_ = L"zh-CN";
    std::wstring toast_;
    int toastSeverity_ = 2;
    DWORD toastUntil_ = 0;

    // 拖动中的滑块 id 与当前值（空 = 没在拖）
    std::wstring sliderDrag_;
    double sliderValue_ = 0.0;

    // 关于页「检查更新」的状态机（结果由宿主回 update 事件）
    enum class UpdateState { Idle, Checking, Current, Newer, None, Failed };
    UpdateState updateState_ = UpdateState::Idle;
    std::wstring updateTag_;
    std::wstring updateUrl_;
    std::wstring updateDetail_;

    // ---- 与后台（同进程内的 C# 侧）的交互 ----
    void SendValue(const char* key, const char* valueJson);
    void ApplyConfig(const fc::Config& config);
    void ApplyState(const fc::State& state);
    void PushCurveToBackend(int which);
    std::wstring ThemeLabel() const;
    std::wstring UnitLabel() const;
    std::wstring LangLabel() const;
    std::wstring ModeLabel(const std::wstring& mode) const;
    std::wstring DeviceLabel() const;
    std::wstring GpuSourceLabel() const;
    std::wstring DataDirLabel() const;
    std::wstring ConnectionLabel() const;
    void SendSystem(const char* key, const std::string& valueJson);
    void ReloadFromBackend();
    // 设置行点击分发：下拉行开浮层、滑块按位置取值、开关取反（1.3.0 的设置项都走这里）
    void HandleSettingClick(const std::wstring& id, D2D1_RECT_F rect, int clickX);
    // 曲线页：在 plot 内某点插入一个控制点（空白处点击 / 「添加点」）
    void AddCurvePointAt(int which, float sx, float sy);
    void SendString(const char* key, const std::wstring& value);
    void SendAutostart(bool enabled);

    std::atomic<bool> backendUp_{ false };
    fc::Config config_;
    bool hasConfig_ = false;
    long long lastSampleMs_ = 0;
};
