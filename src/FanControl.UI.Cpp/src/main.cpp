#include "app.h"
#include "ipc.h"
#include <windowsx.h>
#include <cstdio>
#include <string>
#include <dwmapi.h>
#include <algorithm>
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")

namespace {
App g_app;

// 加入守护进程的命名进程组（Local\FanControl.Group）。界面通常由守护进程拉起，
// 创建时已经自动继承；这里兜住“界面先被单独启动”的情况。
// 加入后必须立刻关句柄：留着会让 Job 在守护进程退出后依然活着，
// KILL_ON_JOB_CLOSE 就不会把界面带走。
void JoinProcessGroup() {
    HANDLE job = OpenJobObjectW(JOB_OBJECT_ASSIGN_PROCESS, FALSE, L"Local\\FanControl.Group");
    if (job == nullptr) {
        return;
    }
    AssignProcessToJobObject(job, GetCurrentProcess());
    CloseHandle(job);
}

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

int RunDump(const wchar_t* path) {
    std::string report;
    fc::IpcClient client;
    const bool connected = client.Connect(8000);
    report += connected ? "connect=ok\n" : "connect=fail\n";
    fc::Message message;
    if (connected) {
        for (int i = 0; i < 4; ++i) {
            if (!client.ReadMessage(4000, message)) {
                report += "read=fail\n";
                break;
            }
            char line[256];
            std::snprintf(line, sizeof(line), "msg%d event=%s state=%d config=%d\n", i,
                          fc::WideToUtf8(message.event).c_str(), message.hasState ? 1 : 0,
                          message.hasConfig ? 1 : 0);
            report += line;
            if (message.hasState) {
                const fc::State& s = message.state;
                std::snprintf(line, sizeof(line), "  cpu=%.1f gpu=%.1f rpm=%.0f pwm=%.0f conn=%s device=%s error=%s\n", s.cpu,
                              s.gpu, s.rpm, s.pwm, fc::WideToUtf8(s.connectionState).c_str(),
                              fc::WideToUtf8(s.deviceName).c_str(), fc::WideToUtf8(s.lastError).c_str());
                report += line;
            }
            if (message.hasConfig) {
                const fc::Config& c = message.config;
                std::snprintf(line, sizeof(line), "  theme=%s unit=%s poll=%d curve=%zu smoothing=%.1f\n",
                              fc::WideToUtf8(c.theme).c_str(), fc::WideToUtf8(c.temperatureUnit).c_str(),
                              c.pollIntervalMilliseconds, c.curve.size(), c.pwmSmoothingWindowSeconds);
                report += line;
            }
        }
        client.SetValue("theme", "\"System\"");
        for (int i = 0; i < 4; ++i) {
            if (!client.ReadMessage(3000, message)) break;
            if (message.event == L"ok" || message.event == L"error") {
                report += "reply event=" + fc::WideToUtf8(message.event) +
                          " message=" + fc::WideToUtf8(message.error) + "\n";
                break;
            }
        }
        client.Close();
    }
    HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(file, report.data(), static_cast<DWORD>(report.size()), &written, nullptr);
        CloseHandle(file);
    }
    return connected ? 0 : 2;

}
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE:
        if (!g_app.Init(hwnd)) return -1;
        SetTimer(hwnd, 1, 1000, nullptr);
        return 0;
    case WM_DESTROY:
        KillTimer(hwnd, 1);
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
    case WM_APP + 2:
        g_app.OnIpcStatus(wp != 0);
        return 0;
    case WM_SETTINGCHANGE:
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}
} // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, LPWSTR cmdLine, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    JoinProcessGroup();
    if (cmdLine) {
        const wchar_t* dumpArg = wcsstr(cmdLine, L"--dump=");
        if (dumpArg) {
            wchar_t path[MAX_PATH] = {};
            wcscpy_s(path, dumpArg + 7);
            const size_t len = wcslen(path);
            if (len >= 2 && path[0] == L'"' && path[len - 1] == L'"') {
                path[len - 1] = L'\0';
                memmove(path, path + 1, (len - 1) * sizeof(wchar_t));
            }
            return RunDump(path);
        }
    }
    // 单实例：已有窗口时激活并退出，避免重复打开
    HANDLE instance = CreateMutexW(nullptr, TRUE, L"Local\\FanControl.UI");
    if (instance != nullptr && GetLastError() == ERROR_ALREADY_EXISTS) {
        if (HWND existing = FindWindowW(L"FanControlUI", nullptr)) {
            ShowWindow(existing, SW_RESTORE);
            SetForegroundWindow(existing);
        }
        return 0;
    }
    if (cmdLine && wcsstr(cmdLine, L"--light")) g_app.SetInitialTheme(false);
    if (cmdLine && wcsstr(cmdLine, L"--dark")) g_app.SetInitialTheme(true);
    if (cmdLine) {
        const wchar_t* pageArg = wcsstr(cmdLine, L"--page=");
        if (pageArg) g_app.SetInitialPage(_wtoi(pageArg + 7));
    }
    // Segoe Fluent Icons 只在 Win11 存在，缺失时由 DirectWrite 回退到 Segoe MDL2 Assets
    WNDCLASSEXW wc{ sizeof(wc) };
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(inst, L"IDI_ICON1");
    wc.lpszClassName = L"FanControlUI";
    RegisterClassExW(&wc);

    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"FanControl", WS_OVERLAPPEDWINDOW,
                                CW_USEDEFAULT, CW_USEDEFAULT, 900, 620, nullptr, nullptr, inst, nullptr);
    if (!hwnd) return 1;

    // 以 DIP 为单位选定客户区尺寸，再换算成物理像素并居中（高 DPI 下不会把布局挤扁）
    SIZE win = SizeWindowForDips(hwnd, 1120, 740);
    MONITORINFO mi{ sizeof(MONITORINFO) };
    GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY), &mi);
    int wx = mi.rcWork.left + ((mi.rcWork.right - mi.rcWork.left) - win.cx) / 2;
    int wy = mi.rcWork.top + ((mi.rcWork.bottom - mi.rcWork.top) - win.cy) / 2;
    SetWindowPos(hwnd, nullptr, wx, wy, win.cx, win.cy, SWP_NOZORDER | SWP_NOACTIVATE);

    ApplyWindowStyle(hwnd, g_app.IsDark());
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return static_cast<int>(msg.wParam);
}
