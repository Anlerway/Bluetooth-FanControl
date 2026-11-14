// FanControl.UI.dll：C++/Direct2D 界面，由 FanControl.exe（C# 宿主）在进程内加载。
// 单一进程：没有命名管道、没有启动器；宿主把状态序列化成 JSON 用 FcUiPush() 推过来，
// 界面上的操作通过 FcUiSetCommandSink() 注册的回调直接回到宿主。

#include "app.h"
#include "ipc.h"

#include <dwmapi.h>
#include <windowsx.h>

#include <algorithm>
#include <cstdio>
#include <string>

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")

namespace {

App g_app;
HWND g_hwnd = nullptr;

void ApplyWindowStyle(HWND hwnd, bool dark) {
    BOOL useDark = dark ? TRUE : FALSE;
    DwmSetWindowAttribute(hwnd, 20, &useDark, sizeof(useDark));   // DWMWA_USE_IMMERSIVE_DARK_MODE
    int corner = 2;                                                // DWMWCP_ROUND
    DwmSetWindowAttribute(hwnd, 33, &corner, sizeof(corner));      // DWMWA_WINDOW_CORNER_PREFERENCE
    // 标题栏与内容同色（COLORREF 为 0x00BBGGRR）
    COLORREF caption = dark ? RGB(0x20, 0x20, 0x20) : RGB(0xF3, 0xF3, 0xF3);
    COLORREF text = dark ? RGB(0xFF, 0xFF, 0xFF) : RGB(0x1B, 0x1B, 0x1B);
    COLORREF border = dark ? RGB(0x2C, 0x2C, 0x2C) : RGB(0xE5, 0xE5, 0xE5);
    DwmSetWindowAttribute(hwnd, 35, &caption, sizeof(caption));    // DWMWA_CAPTION_COLOR
    DwmSetWindowAttribute(hwnd, 36, &text, sizeof(text));          // DWMWA_TEXT_COLOR
    DwmSetWindowAttribute(hwnd, 34, &border, sizeof(border));      // DWMWA_BORDER_COLOR
}

SIZE SizeWindowForDips(HWND hwnd, int clientW, int clientH) {
    UINT dpi = GetDpiForWindow(hwnd);
    float s = dpi / 96.0f;
    MONITORINFO mi{ sizeof(MONITORINFO) };
    GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY), &mi);
    int workW = static_cast<int>((mi.rcWork.right - mi.rcWork.left) / s);
    int workH = static_cast<int>((mi.rcWork.bottom - mi.rcWork.top) / s);
    clientW = std::min(clientW, workW - 48);
    clientH = std::min(clientH, workH - 48);
    // clientW/clientH 是 DIP，AdjustWindowRectExForDpi 只按 dpi 放大非客户区，
    // 客户区必须自己换算成像素，否则高 DPI 下窗口会比预期小一半以上。
    RECT rc{ 0, 0, static_cast<LONG>(clientW * s), static_cast<LONG>(clientH * s) };
    AdjustWindowRectExForDpi(&rc, WS_OVERLAPPEDWINDOW, FALSE, 0, dpi);
    return SIZE{ rc.right - rc.left, rc.bottom - rc.top };
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE:
        if (!g_app.Init(hwnd)) return -1;
        SetTimer(hwnd, 1, 1000, nullptr);
        return 0;
    case WM_DESTROY:
        KillTimer(hwnd, 1);
        // 窗口销毁后进程还要在托盘里待着：通知宿主把界面占的工作集还给系统
        fc::Send("{\"cmd\":\"uiClosed\"}");
        g_app.Release();
        PostQuitMessage(0);
        return 0;
    case WM_SIZE:
        g_app.Resize(LOWORD(lp), HIWORD(lp));
        return 0;
    case WM_GETMINMAXINFO: {
        auto mmi = reinterpret_cast<MINMAXINFO*>(lp);
        mmi->ptMinTrackSize.x = static_cast<LONG>(760 * g_app.DpiScale());
        mmi->ptMinTrackSize.y = static_cast<LONG>(620 * g_app.DpiScale());
        return 0;
    }
    case WM_DPICHANGED: {
        auto rc = reinterpret_cast<RECT*>(lp);
        SetWindowPos(hwnd, nullptr, rc->left, rc->top, rc->right - rc->left, rc->bottom - rc->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        g_app.Resize(static_cast<UINT>(rc->right - rc->left), static_cast<UINT>(rc->bottom - rc->top));
        return 0;
    }
    case WM_TIMER:
        g_app.Tick();
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        g_app.Paint();
        EndPaint(hwnd, &ps);
        (void)hdc;
        return 0;
    }
    case WM_MOUSEMOVE: {
        TRACKMOUSEEVENT tme{ sizeof(TRACKMOUSEEVENT), TME_LEAVE, hwnd, 0 };
        TrackMouseEvent(&tme);
        g_app.OnMouseMove(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        return 0;
    }
    case WM_MOUSELEAVE:
        g_app.OnMouseLeave();
        return 0;
    case WM_LBUTTONDOWN:
        SetCapture(hwnd);
        g_app.OnLButtonDown(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        return 0;
    case WM_LBUTTONUP:
        ReleaseCapture();
        g_app.OnLButtonUp(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        return 0;
    case WM_MOUSEWHEEL:
        g_app.OnMouseWheel(GET_WHEEL_DELTA_WPARAM(wp));
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case WM_APP + 1:
        g_app.OnIpcMessage(reinterpret_cast<void*>(lp));
        return 0;
    case WM_APP + 9: {  // 宿主推来的状态/配置 JSON
        std::unique_ptr<std::string> text(static_cast<std::string*>(reinterpret_cast<void*>(lp)));
        if (text) {
            auto* message = new fc::Message();
            if (fc::ParseMessage(*text, *message)) {
                g_app.OnIpcMessage(message);
            } else {
                delete message;
            }
        }
        return 0;
    }
    case WM_APP + 8:  // 托盘/第二个实例要求把窗口叫到前台
        if (IsIconic(hwnd)) ShowWindow(hwnd, SW_RESTORE);
        SetForegroundWindow(hwnd);
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case WM_SETTINGCHANGE:
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}
}  // namespace

// ---- 导出给 C# 宿主的 C ABI ----

extern "C" __declspec(dllexport) int __stdcall FcUiRun(void) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    HMODULE module = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&FcUiRun), &module);

    WNDCLASSEXW wc{ sizeof(wc) };
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = module;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(module, L"IDI_ICON1");
    if (wc.hIcon == nullptr) wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wc.lpszClassName = L"FanControlUI";
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return 1;

    g_hwnd = CreateWindowExW(0, wc.lpszClassName, L"FanControl", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
                             CW_USEDEFAULT, 900, 620, nullptr, nullptr, module, nullptr);
    if (!g_hwnd) return 1;

    // 以 DIP 为单位选定客户区尺寸，再换算成物理像素并居中（高 DPI 下不会把布局挤扁）
    SIZE win = SizeWindowForDips(g_hwnd, 1120, 740);
    MONITORINFO mi{ sizeof(MONITORINFO) };
    GetMonitorInfoW(MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTOPRIMARY), &mi);
    const int wx = mi.rcWork.left + ((mi.rcWork.right - mi.rcWork.left) - win.cx) / 2;
    const int wy = mi.rcWork.top + ((mi.rcWork.bottom - mi.rcWork.top) - win.cy) / 2;
    SetWindowPos(g_hwnd, nullptr, wx, wy, win.cx, win.cy, SWP_NOZORDER | SWP_NOACTIVATE);

    ApplyWindowStyle(g_hwnd, g_app.IsDark());
    ShowWindow(g_hwnd, SW_SHOW);
    UpdateWindow(g_hwnd);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    g_hwnd = nullptr;
    return static_cast<int>(msg.wParam);
}

/// 把窗口叫到前台（托盘菜单、第二个实例启动时调用）。任意线程可调。
extern "C" __declspec(dllexport) void __stdcall FcUiActivate(void) {
    if (g_hwnd) PostMessageW(g_hwnd, WM_APP + 8, 0, 0);
}

/// 宿主推一帧状态/配置 JSON（与 1.4.0 的管道载荷同形）。任意线程可调，立即返回。
extern "C" __declspec(dllexport) void __stdcall FcUiPush(const char* utf8Json) {
    if (g_hwnd == nullptr || utf8Json == nullptr) return;
    auto* payload = new std::string(utf8Json);
    if (!PostMessageW(g_hwnd, WM_APP + 9, 0, reinterpret_cast<LPARAM>(payload))) delete payload;
}

/// 关闭界面（宿主退出时调用）。
extern "C" __declspec(dllexport) void __stdcall FcUiQuit(void) {
    if (g_hwnd) PostMessageW(g_hwnd, WM_CLOSE, 0, 0);
}

/// 注册命令回调：界面上的操作（改配置、重连、扫描设备…）原样回调给宿主。
extern "C" __declspec(dllexport) void __stdcall FcUiSetCommandSink(fc::CommandSink sink) {
    fc::SetCommandSink(sink);
}

/// 宿主启动时告诉界面初始主题（0=浅色 1=深色，-1=跟随系统）。
extern "C" __declspec(dllexport) void __stdcall FcUiSetInitialTheme(int dark) {
    if (dark >= 0) g_app.SetInitialTheme(dark != 0);
}

/// 宿主启动时告诉界面初始页面（0=仪表盘 1=设置 2=曲线 3=关于）。
extern "C" __declspec(dllexport) void __stdcall FcUiSetInitialPage(int page) {
    g_app.SetInitialPage(page);
}
