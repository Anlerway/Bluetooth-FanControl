// 安装器入口：先自提权，再按 exe 名（或 --uninstall）分派到安装/卸载界面。
#include <windows.h>

#include <objbase.h>
#include <shellapi.h>

#include <cwchar>
#include <cwctype>
#include <string>

#include "app.h"
#include "i18n.h"
#include "install.h"
#include "payload.h"
#include "sysutil.h"

namespace {

constexpr int kIconResourceId = 101;   // 见 setup.rc
constexpr wchar_t kWindowClass[] = L"FanControlSetupWindow";
constexpr wchar_t kInstanceMutex[] = L"Local\\FanControl.Setup.Instance";

HANDLE g_instanceMutex = nullptr;

// 单实例：安装器与卸载器共用一把命名互斥体，已有实例在跑时第二个实例只提示并退出。
bool AcquireSingleInstance() {
    g_instanceMutex = CreateMutexW(nullptr, TRUE, kInstanceMutex);
    if (g_instanceMutex != nullptr) return GetLastError() != ERROR_ALREADY_EXISTS;
    const DWORD err = GetLastError();
    if (err == ERROR_ACCESS_DENIED) return false;  // 名字被更高权限的实例占用
    return true;                                   // 无法判定时不阻止启动
}

void ReleaseSingleInstance() {
    if (g_instanceMutex == nullptr) return;
    ReleaseMutex(g_instanceMutex);
    CloseHandle(g_instanceMutex);
    g_instanceMutex = nullptr;
}

void ActivateExistingInstance() {
    HWND existing = FindWindowW(kWindowClass, nullptr);
    if (existing == nullptr) return;
    if (IsIconic(existing)) ShowWindow(existing, SW_RESTORE);
    SetForegroundWindow(existing);
}

struct InstanceGuard {
    ~InstanceGuard() { ReleaseSingleInstance(); }
};

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
    // 卸载收尾副本（StartSelfCleanup 起的那一个）：不建窗口、不弹任何提示，删完立刻退出。
    if (std::wstring(GetCommandLineW()).find(L"--cleanup") != std::wstring::npos) {
        int argc = 0;
        LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        std::wstring cleanupDir;
        std::wstring cleanupSelf;
        unsigned long parentPid = 0;
        if (argv != nullptr) {
            if (argc >= 3) cleanupDir = argv[2];
            if (argc >= 4) cleanupSelf = argv[3];
            if (argc >= 5) parentPid = static_cast<unsigned long>(std::wcstoul(argv[4], nullptr, 10));
            LocalFree(argv);
        }
        return fc::RunCleanupWorker(cleanupDir, cleanupSelf, parentPid) == 0 ? 0 : 1;
    }

    const std::wstring exePath = fc::GetModulePath();
    bool uninstall = LowerCase(fc::FileNameOf(exePath)).find(L"uninstall") != std::wstring::npos;
    if (std::wstring(GetCommandLineW()).find(L"--uninstall") != std::wstring::npos) uninstall = true;
    const fc::SetupMode mode = uninstall ? fc::SetupMode::Uninstall : fc::SetupMode::Install;

    bool chinese = fc::GetSystemLocaleName().rfind(L"zh", 0) == 0;
    fc::SetUiLanguage(chinese);
    const wchar_t* title = fc::T(uninstall ? fc::Str::UninstallTitle : fc::Str::SetupTitle);

    if (!AcquireSingleInstance()) {
        ActivateExistingInstance();
        MessageBoxW(nullptr, fc::T(fc::Str::AlreadyRunning), title, MB_ICONINFORMATION | MB_OK);
        return 0;
    }
    const InstanceGuard instanceGuard;
    (void)instanceGuard;

    // FCSETUP_NO_ELEVATE=1 仅用于开发期在未提权环境预览界面（安装本身仍需要管理员权限）。
    wchar_t devFlag[8]{};
    const bool skipElevation = GetEnvironmentVariableW(L"FCSETUP_NO_ELEVATE", devFlag, 8) > 0;
    if (!fc::IsElevated() && !skipElevation) {
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
    wc.style = CS_DBLCLKS;   // 输入框双击全选需要

    wc.lpfnWndProc = WindowProc;
    wc.hInstance = hinst;
    wc.hIcon = LoadIconW(hinst, MAKEINTRESOURCEW(kIconResourceId));
    wc.hIconSm = wc.hIcon;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kWindowClass;
    if (!RegisterClassExW(&wc)) {
        CoUninitialize();
        return 1;
    }

    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    const float systemScale = static_cast<float>(GetDpiForSystem()) / 96.0f;
    // 设计尺寸 700x578，再按界面缩放系数（见 App::kUiScale）整体缩小。
    const float uiScale = systemScale * fc::App::kUiScale;
    RECT frame{0, 0, static_cast<LONG>(700 * uiScale + 0.5f), static_cast<LONG>(578 * uiScale + 0.5f)};
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
