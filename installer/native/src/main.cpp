// 安装器入口：先自提权，再按 exe 名（或 --uninstall）分派到安装/卸载界面。
#include <windows.h>

#include <objbase.h>
#include <shellapi.h>

#include <cwctype>
#include <string>

#include "app.h"
#include "i18n.h"
#include "install.h"
#include "payload.h"
#include "sysutil.h"

namespace {

constexpr int kIconResourceId = 101;   // 见 setup.rc

fc::App g_app;
bool    g_appReady = false;

LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_DESTROY) PostQuitMessage(0);
    if (g_appReady) return g_app.HandleMessage(msg, wp, lp);
    return DefWindowProcW(hwnd, msg, wp, lp);
}

std::wstring LowerCase(std::wstring text) {
    for (wchar_t& c : text) c = static_cast<wchar_t>(std::towlower(c));
    return text;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE hinst, HINSTANCE, PWSTR, int) {
    const std::wstring exePath = fc::GetModulePath();
    bool uninstall = LowerCase(fc::FileNameOf(exePath)).find(L"uninstall") != std::wstring::npos;
    if (std::wstring(GetCommandLineW()).find(L"--uninstall") != std::wstring::npos) uninstall = true;
    const fc::SetupMode mode = uninstall ? fc::SetupMode::Uninstall : fc::SetupMode::Install;

    bool chinese = fc::GetSystemLocaleName().rfind(L"zh", 0) == 0;
    fc::SetUiLanguage(chinese);
    const wchar_t* title = fc::T(uninstall ? fc::Str::UninstallTitle : fc::Str::SetupTitle);

    if (!fc::IsElevated()) {
        if (fc::RelaunchElevated(uninstall ? L"--uninstall" : L"")) return 0;
        MessageBoxW(nullptr, fc::T(fc::Str::ErrElevate), title, MB_ICONERROR | MB_OK);
        return 1;
    }
    if (!uninstall && !fc::PayloadAvailable(exePath)) {
        MessageBoxW(nullptr, fc::T(fc::Str::ErrPayload), title, MB_ICONERROR | MB_OK);
        return 1;
    }
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE))) return 1;

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = hinst;
    wc.hIcon = LoadIconW(hinst, MAKEINTRESOURCEW(kIconResourceId));
    wc.hIconSm = wc.hIcon;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"FanControlSetupWindow";
    if (!RegisterClassExW(&wc)) {
        CoUninitialize();
        return 1;
    }

    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    RECT frame{0, 0, 700, 640};
    AdjustWindowRectEx(&frame, style, FALSE, 0);
    const int width = frame.right - frame.left;
    const int height = frame.bottom - frame.top;
    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    const int x = work.left + ((work.right - work.left) - width) / 2;
    const int y = work.top + ((work.bottom - work.top) - height) / 2;

    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, title, style, x, y, width, height, nullptr, nullptr, hinst,
                                nullptr);
    if (!hwnd) {
        CoUninitialize();
        return 1;
    }
    if (!g_app.Create(hwnd, mode)) {
        MessageBoxW(nullptr, fc::T(fc::Str::ErrGeneric), title, MB_ICONERROR | MB_OK);
        DestroyWindow(hwnd);
        CoUninitialize();
        return 1;
    }
    g_appReady = true;
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    g_appReady = false;
    g_app.Destroy();
    CoUninitialize();
    return 0;
}
