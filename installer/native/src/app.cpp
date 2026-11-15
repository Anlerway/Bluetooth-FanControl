#include "app.h"

#include <shlobj.h>
#include <shobjidl.h>
#include <dwmapi.h>

#include <winver.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <cstring>

#include "i18n.h"
#include "payload.h"
#include "sysutil.h"

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "uuid.lib")
#pragma comment(lib, "version.lib")

namespace fc {
namespace {

constexpr float kWindowWidth = 700.0f;
constexpr float kMargin = 28.0f;
constexpr float kCardRadius = 10.0f;

D2D1_COLOR_F Alpha(const D2D1_COLOR_F& c, float a) {
    D2D1_COLOR_F out = c;
    out.a = a;
    return out;
}

std::wstring MbText(uint64_t bytes) {
    wchar_t buf[32]{};
    swprintf_s(buf, L"%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
    return buf;
}

std::wstring ReadOwnVersion() {
    const std::wstring path = GetModulePath();
    DWORD dummy = 0;
    const DWORD size = GetFileVersionInfoSizeW(path.c_str(), &dummy);
    if (size == 0) return L"2.0.0";
    std::vector<BYTE> data(size);
    if (!GetFileVersionInfoW(path.c_str(), 0, size, data.data())) return L"2.0.0";
    VS_FIXEDFILEINFO* info = nullptr;
    UINT len = 0;
    if (!VerQueryValueW(data.data(), L"\\", reinterpret_cast<void**>(&info), &len) || !info) return L"2.0.0";
    return Format(L"%u.%u.%u", HIWORD(info->dwFileVersionMS), LOWORD(info->dwFileVersionMS),
                  HIWORD(info->dwFileVersionLS));
}

}  // namespace

namespace {

// 统一成反斜杠并去掉末尾分隔符（盘根 D:\ 除外）：输入框里不显示尾部分隔符更干净。
std::wstring StripSeparators(const std::wstring& in) {
    std::wstring out = in;
    while (!out.empty() && (out.back() == L' ' || out.back() == L'\t')) out.pop_back();
    for (wchar_t& c : out) {
        if (c == L'/') c = L'\\';
    }
    while (!out.empty() && (out.front() == L' ')) out.erase(out.begin());
    while (out.size() > 3 && out.back() == L'\\') out.pop_back();
    return out;
}

// 路径输入框的粘贴来源：去掉换行等控制字符，避免粘进一段说明文字就毁掉路径。
std::wstring ClipboardPath() {
    std::wstring out;
    if (!OpenClipboard(nullptr)) return out;
    if (HANDLE handle = GetClipboardData(CF_UNICODETEXT)) {
        if (const wchar_t* text = static_cast<const wchar_t*>(GlobalLock(handle))) {
            out.assign(text);
            GlobalUnlock(handle);
        }
    }
    CloseClipboard();
    for (wchar_t& c : out) {
        if (c < 32) c = L' ';
    }
    return StripSeparators(out);
}

// 输入框复制出去的只有纯文本，不带任何格式。
void CopyToClipboard(const std::wstring& text) {
    if (text.empty()) return;
    if (!OpenClipboard(nullptr)) return;
    EmptyClipboard();
    const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    if (HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes)) {
        if (void* dst = GlobalLock(mem)) {
            std::memcpy(dst, text.c_str(), bytes);
            GlobalUnlock(mem);
            SetClipboardData(CF_UNICODETEXT, mem);
        } else {
            GlobalFree(mem);
        }
    }
    CloseClipboard();
}

// 「浏览…」选中的是父目录，落盘时补一层产品文件夹；已经以它结尾就不再叠加。
constexpr wchar_t kProductFolder[] = L"FanControl";

std::wstring LowerAscii(std::wstring text) {
    for (wchar_t& c : text) {
        if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c - L'A' + L'a');
    }
    return text;
}

std::wstring WithProductFolder(const std::wstring& parent) {
    const std::wstring base = StripSeparators(parent);
    if (base.empty()) return base;
    const std::wstring tail = std::wstring(L"\\") + kProductFolder;
    if (base.size() >= tail.size() &&
        LowerAscii(base.substr(base.size() - tail.size())) == LowerAscii(tail)) {
        return base;
    }
    if (base.back() == L'\\') return base + kProductFolder;
    return base + L"\\" + kProductFolder;
}

}  // namespace

namespace {
// 安装页与卸载页共用的窗口高度基准（两页等高；第二页选自定义目录时会略高）。
constexpr float kPageHeight = 578.0f;
}  // namespace

bool App::Create(HWND hwnd, SetupMode mode) {
    hwnd_ = hwnd;
    mode_ = mode;
    D2D1_FACTORY_OPTIONS options{};
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, options, &d2d_))) return false;
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                   reinterpret_cast<IUnknown**>(&dw_)))) {
        return false;
    }
    gfx_.SetUiScale(kUiScale);
    if (!gfx_.Init(hwnd_, d2d_, dw_)) return false;

    // 图标：取 128 像素那档（256 那档是 PNG 压缩，WIC 的 CreateBitmapFromHICON 解不了）。
    appIcon_ = static_cast<HICON>(
        LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(101), IMAGE_ICON, 128, 128, LR_DEFAULTCOLOR));
    gfx_.LoadIcon(appIcon_);

    exePath_ = GetModulePath();
    version_ = ReadOwnVersion();
    runtimeAvailable_ = HasDotnetRuntime8();
    preferSlim_ = runtimeAvailable_;

    // 卸载器跟随已安装时选定的语言；安装器跟随系统语言。
    InstallInfo info;
    const bool haveInfo = ReadInstallInfo(&info);
    if (mode_ == SetupMode::Uninstall && haveInfo && !info.language.empty()) {
        chinese_ = info.language.rfind(L"zh", 0) == 0;
    } else {
        chinese_ = GetSystemLocaleName().rfind(L"zh", 0) == 0;
    }
    SetUiLanguage(chinese_);

    if (mode_ == SetupMode::Uninstall) {
        installDir_ = StripSeparators((haveInfo && !info.installDir.empty()) ? info.installDir : DefaultInstallDir());
        dataRoot_ = info.dataRoot.empty() ? ResolveDataRoot(info.dataMode, installDir_, L"") : info.dataRoot;
        page_ = Page::Confirm;
        dataMode_ = info.dataMode;
    } else {
        // 已经装过同一个程序：就地升级，沿用原安装目录与数据位置，开关恢复成当前实况。
        upgrade_ = haveInfo && info.installed && !info.installDir.empty();
        installedVersion_ = info.version;
        // 输入框里始终是最终安装目录：全新安装默认 %ProgramFiles%\FanControl，升级沿用原目录。
        installDir_ = StripSeparators(upgrade_ ? info.installDir : DefaultInstallDir());
        dataMode_ = upgrade_ ? info.dataMode : DataMode::AppData;
        if (upgrade_ && dataMode_ == DataMode::Custom) customDir_ = info.dataRoot;
        if (upgrade_) {
            // 升级只有一页，选项页看不到：开关必须先按当前实况恢复，否则会被默认值覆盖
            autostart_ = AutostartEnabled();
            desktopShortcut_ = FileExists(Join(NormalizeDir(GetCommonDesktopDir()), L"FanControl.lnk"));
            startMenuShortcut_ =
                FileExists(Join(NormalizeDir(GetProgramsMenuDir()), L"FanControl.lnk"));
        }
        page_ = Page::Configure;
    }
    status_ = T(Str::StepPreparing);

    PayloadSet payload;
    if (PayloadSet::Load(exePath_, &payload, nullptr)) {
        bytesFull_ = payload.seg[0].present ? payload.seg[0].rawSize : 0;
        bytesSlim_ = payload.seg[1].present ? payload.seg[1].rawSize : 0;
    }
    RefreshExistingConfig();
    ApplyStrings();
    ApplyDwm();
    return true;
}

void App::Destroy() {
    if (worker_.joinable()) worker_.detach();
    gfx_.Shutdown();
    if (appIcon_) {
        DestroyIcon(appIcon_);
        appIcon_ = nullptr;
    }
    if (dw_) {
        dw_->Release();
        dw_ = nullptr;
    }
    if (d2d_) {
        d2d_->Release();
        d2d_ = nullptr;
    }
}

std::wstring App::DataRootText() const {
    if (mode_ == SetupMode::Uninstall && !dataRoot_.empty()) return dataRoot_;
    return ResolveDataRoot(dataMode_, InstallTargetDir(), customDir_);
}

std::wstring App::InstallTargetDir() const {
    return StripSeparators(installDir_);
}

void App::RefreshExistingConfig() {
    const std::wstring target = InstallTargetDir();
    existingConfig_ = HasExistingConfig(target, ResolveDataRoot(dataMode_, target, customDir_));
}

void App::ApplyStrings() {
    SetUiLanguage(chinese_);
    SetWindowTextW(hwnd_, T(mode_ == SetupMode::Install ? Str::SetupTitle : Str::UninstallTitle));
}

void App::ApplyDwm() {
    const Theme& th = gfx_.theme();
    const bool dark = !SystemUsesLightTheme();
    const BOOL darkFlag = dark ? TRUE : FALSE;
    DwmSetWindowAttribute(hwnd_, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &darkFlag, sizeof(darkFlag));
    const DWORD round = 2 /*DWMWCP_ROUND*/;
    DwmSetWindowAttribute(hwnd_, 33 /*DWMWA_WINDOW_CORNER_PREFERENCE*/, &round, sizeof(round));

    auto toColorRef = [](const D2D1_COLOR_F& c) {
        return COLORREF((BYTE(std::lround(c.r * 255)) | (BYTE(std::lround(c.g * 255)) << 8) |
                         (BYTE(std::lround(c.b * 255)) << 16)));
    };
    const COLORREF caption = toColorRef(th.windowBg);
    const COLORREF text = toColorRef(th.textPrimary);
    const COLORREF border = toColorRef(th.cardBorder);
    DwmSetWindowAttribute(hwnd_, 35 /*DWMWA_CAPTION_COLOR*/, &caption, sizeof(caption));
    DwmSetWindowAttribute(hwnd_, 36 /*DWMWA_TEXT_COLOR*/, &text, sizeof(text));
    DwmSetWindowAttribute(hwnd_, 34 /*DWMWA_BORDER_COLOR*/, &border, sizeof(border));
}

bool App::PickFolder(std::wstring* out, const std::wstring& initial) {
    IFileOpenDialog* dialog = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) {
        return false;
    }
    bool ok = false;
    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    dialog->SetTitle(T(Str::SectionData));
    if (!initial.empty()) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(SHCreateItemFromParsingName(initial.c_str(), nullptr, IID_PPV_ARGS(&item))) && item) {
            dialog->SetFolder(item);
            item->Release();
        }
    }
    if (SUCCEEDED(dialog->Show(hwnd_))) {
        IShellItem* result = nullptr;
        if (SUCCEEDED(dialog->GetResult(&result)) && result) {
            PWSTR path = nullptr;
            if (SUCCEEDED(result->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path) {
                *out = NormalizeDir(path);
                CoTaskMemFree(path);
                ok = true;
            }
            result->Release();
        }
    }
    dialog->Release();
    return ok;
}

LRESULT App::HandleMessage(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_SIZE:
        // 客户区变了必须重建 D2D 位图，否则画面会被整体拉伸（高 DPI 下尤其明显）。
        gfx_.Resize(static_cast<UINT>(LOWORD(lp)), static_cast<UINT>(HIWORD(lp)));
        InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        BeginPaint(hwnd_, &ps);
        Paint();
        EndPaint(hwnd_, &ps);
        return 0;
    }
    case WM_MOUSEMOVE: {
        const float x = static_cast<float>(GET_X_LPARAM(lp)) / gfx_.scale();
        const float y = static_cast<float>(GET_Y_LPARAM(lp)) / gfx_.scale();
        if (selecting_ && pathFocused_) {
            const float relX = x - pathFieldRect_.left - 12.0f + pathScroll_;
            SetCaretPos(PathIndexAtX(relX), true);
            return 0;
        }
        const Hit hit = HitAt(x, y);
        if (hit != hoverId_) {
            hoverId_ = hit;
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        if (hoverId_ != Hit::None) {
            hoverId_ = Hit::None;
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
        return 0;
    case WM_LBUTTONDOWN: {
        SetCapture(hwnd_);
        const float x = static_cast<float>(GET_X_LPARAM(lp)) / gfx_.scale();
        const float y = static_cast<float>(GET_Y_LPARAM(lp)) / gfx_.scale();
        pressedId_ = HitAt(x, y);
        if (pressedId_ == Hit::PathField && !upgrade_) {
            FocusPath(true);
            const bool shiftDown = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
            const float relX = x - pathFieldRect_.left - 12.0f + pathScroll_;
            SetCaretPos(PathIndexAtX(relX), shiftDown);
            selecting_ = true;
        } else if (pathFocused_) {
            FocusPath(false);
        }
        return 0;
    }
    case WM_LBUTTONUP: {
        ReleaseCapture();
        selecting_ = false;
        const float x = static_cast<float>(GET_X_LPARAM(lp)) / gfx_.scale();
        const float y = static_cast<float>(GET_Y_LPARAM(lp)) / gfx_.scale();
        const Hit hit = HitAt(x, y);
        const Hit pressed = pressedId_;
        pressedId_ = Hit::None;
        if (hit != Hit::None && hit == pressed) Activate(hit);
        return 0;
    }
    case WM_LBUTTONDBLCLK:
        if (!upgrade_ && HitAt(static_cast<float>(GET_X_LPARAM(lp)) / gfx_.scale(),
                               static_cast<float>(GET_Y_LPARAM(lp)) / gfx_.scale()) == Hit::PathField) {
            FocusPath(true);
            SelectAllPath();
            selecting_ = false;
        }
        return 0;
    case WM_CHAR:
        if (pathFocused_) {
            if (wp >= 32 && wp != 127) {
                const wchar_t ch = static_cast<wchar_t>(wp);
                if (std::wstring(L"<>|\"?*").find(ch) == std::wstring::npos) InsertText(std::wstring(1, ch));
            }
            return 0;
        }
        break;
    case WM_KEYDOWN:
        if (pathFocused_) {
            const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
            const bool shiftDown = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
            const size_t len = installDir_.size();
            switch (wp) {
            case VK_LEFT:
                if (!shiftDown && HasPathSelection()) {
                    SetCaretPos(PathSelBegin(), false);
                } else if (caret_ > 0) {
                    SetCaretPos(caret_ - 1, shiftDown);
                }
                break;
            case VK_RIGHT:
                if (!shiftDown && HasPathSelection()) {
                    SetCaretPos(PathSelEnd(), false);
                } else if (caret_ < len) {
                    SetCaretPos(caret_ + 1, shiftDown);
                }
                break;
            case VK_HOME:
                SetCaretPos(0, shiftDown);
                break;
            case VK_END:
                SetCaretPos(len, shiftDown);
                break;
            case VK_BACK:
                if (!DeletePathSelection() && caret_ > 0) {
                    installDir_.erase(caret_ - 1, 1);
                    SetCaretPos(caret_ - 1, false);
                    RefreshExistingConfig();
                }
                break;
            case VK_DELETE:
                if (!DeletePathSelection() && caret_ < len) {
                    installDir_.erase(caret_, 1);
                    RefreshExistingConfig();
                }
                break;
            case 'A':
                if (ctrl) SelectAllPath();
                break;
            case 'C':
                if (ctrl) CopyToClipboard(HasPathSelection() ? SelectedPathText() : installDir_);
                break;
            case 'X':
                if (ctrl && HasPathSelection()) {
                    CopyToClipboard(SelectedPathText());
                    (void)DeletePathSelection();
                }
                break;
            case VK_RETURN:
                Activate(Hit::Primary);
                return 0;
            case VK_ESCAPE:
                FocusPath(false);
                return 0;
            case 'V':
                if (ctrl) InsertText(ClipboardPath());
                break;
            default:
                break;
            }
            caretOn_ = true;
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        }
        break;
    case WM_TIMER:
        if (wp == 1) {
            caretOn_ = !caretOn_;
            if (pathFocused_) InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        }
        break;
    case WM_SETCURSOR:
        SetCursor(LoadCursorW(nullptr, hoverId_ == Hit::PathField ? IDC_IBEAM
                                          : (hoverId_ != Hit::None ? IDC_HAND : IDC_ARROW)));
        return TRUE;
    case App::kMsgProgress: {
        std::wstring* text = reinterpret_cast<std::wstring*>(lp);
        percent_ = static_cast<int>(wp);
        if (text) {
            status_ = *text;
            delete text;
        }
        InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;
    }
    case App::kMsgFinished: {
        std::wstring* text = reinterpret_cast<std::wstring*>(lp);
        const std::wstring err = text ? *text : std::wstring();
        delete text;
        if (worker_.joinable()) worker_.join();
        busy_ = false;
        error_ = err;
        if (err.empty()) {
            page_ = (mode_ == SetupMode::Install) ? Page::Done : Page::UninstallDone;
        }
        InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;
    }
    case WM_CLOSE:
        if (busy_) return 0;
        DestroyWindow(hwnd_);
        return 0;
    case WM_DPICHANGED: {
        const RECT* rect = reinterpret_cast<const RECT*>(lp);
        SetWindowPos(hwnd_, nullptr, rect->left, rect->top, rect->right - rect->left, rect->bottom - rect->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        RECT client{};
        GetClientRect(hwnd_, &client);
        gfx_.Resize(static_cast<UINT>(client.right), static_cast<UINT>(client.bottom));
        InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;
    }
    default:
        break;
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
}

App::Hit App::HitAt(float x, float y) {
    for (auto it = hotspots_.rbegin(); it != hotspots_.rend(); ++it) {
        if (Contains(it->rect, x, y)) return it->id;
    }
    return Hit::None;
}

void App::Activate(Hit id) {
    if (busy_ && id != Hit::None) return;
    switch (id) {
    case Hit::LangZh:
    case Hit::LangEn: {
        const bool zh = (id == Hit::LangZh);
        if (zh != chinese_) {
            chinese_ = zh;
            ApplyStrings();
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
        break;
    }
    case Hit::PickSlim:
        if (runtimeAvailable_ && !preferSlim_) {
            preferSlim_ = true;
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
        break;
    case Hit::PickFull:
        if (preferSlim_) {
            preferSlim_ = false;
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
        break;
    case Hit::PathField:
        if (upgrade_) break;   // 升级页面位置锁定
        FocusPath(true);
        InvalidateRect(hwnd_, nullptr, FALSE);
        break;
    case Hit::BrowseInstall: {
        if (upgrade_) break;   // 升级页面不提供换目录
        std::wstring picked;
        if (PickFolder(&picked, installDir_)) {
            // 选中的是父目录：直接把 <父目录>\FanControl 写回输入框，所见即所得。
            installDir_ = StripSeparators(WithProductFolder(picked));
            caret_ = installDir_.size();
            selAnchor_ = caret_;
            RefreshExistingConfig();
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
        break;
    }
    case Hit::DataAppData:
    case Hit::DataExeDir:
    case Hit::DataCustom: {
        const DataMode mode = (id == Hit::DataAppData)   ? DataMode::AppData
                              : (id == Hit::DataExeDir)  ? DataMode::ExeDirectory
                                                          : DataMode::Custom;
        if (mode == DataMode::Custom && customDir_.empty()) {
            std::wstring picked;
            if (PickFolder(&picked, GetAppDataRoaming())) customDir_ = picked;
        }
        dataMode_ = mode;
        RefreshExistingConfig();
        InvalidateRect(hwnd_, nullptr, FALSE);
        break;
    }
    case Hit::BrowseCustom: {
        std::wstring picked;
        if (PickFolder(&picked, customDir_.empty() ? GetAppDataRoaming() : customDir_)) {
            customDir_ = picked;
            dataMode_ = DataMode::Custom;
            RefreshExistingConfig();
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
        break;
    }
    case Hit::OptDesktop:
        desktopShortcut_ = !desktopShortcut_;
        InvalidateRect(hwnd_, nullptr, FALSE);
        break;
    case Hit::OptStartMenu:
        startMenuShortcut_ = !startMenuShortcut_;
        InvalidateRect(hwnd_, nullptr, FALSE);
        break;
    case Hit::OptAutostart:
        autostart_ = !autostart_;
        InvalidateRect(hwnd_, nullptr, FALSE);
        break;
    case Hit::OptLaunch:
        launchAfter_ = !launchAfter_;
        InvalidateRect(hwnd_, nullptr, FALSE);
        break;
    case Hit::OptDeleteData:
        deleteData_ = !deleteData_;
        InvalidateRect(hwnd_, nullptr, FALSE);
        break;
    case Hit::Primary:
        if (page_ == Page::Configure) {
            if (upgrade_) {
                // 升级只有一页：直接开始覆盖安装，位置、数据目录与开关全部沿用原状
                BeginInstall();
                break;
            }
            if (!PathValid()) {
                FocusPath(true);
                InvalidateRect(hwnd_, nullptr, FALSE);
                break;
            }
            CommitPath();
            page_ = Page::Data;
            InvalidateRect(hwnd_, nullptr, FALSE);
        } else if (page_ == Page::Data) {
            BeginInstall();
        } else if (page_ == Page::Confirm) {
            // 二次确认：默认按钮放在「否」，避免顺手一路回车把程序卸了。
            if (MessageBoxW(hwnd_, T(Str::ConfirmTwice), T(Str::UninstallTitle),
                            MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) == IDYES) {
                BeginUninstall();
            }
        } else if (page_ == Page::Done) {
            // 原先在安装线程里就启动了主程序；现在等用户点「完成」再开，免得界面还没看完程序先跳出来。
            if (launchAfter_) (void)ShellOpen(Join(InstallTargetDir(), L"FanControl.exe"));
            DestroyWindow(hwnd_);
        } else {
            DestroyWindow(hwnd_);
        }
        break;
    case Hit::Secondary:
        if (page_ == Page::Data) {
            CommitPath();
            page_ = Page::Configure;
            InvalidateRect(hwnd_, nullptr, FALSE);
        } else if (page_ == Page::Progress || page_ == Page::UninstallProgress) {
            InvalidateRect(hwnd_, nullptr, FALSE);
        } else {
            DestroyWindow(hwnd_);
        }
        break;
    default:
        break;
    }
}

void App::BeginInstall() {
    if (!error_.empty()) error_.clear();
    if (dataMode_ == DataMode::Custom && customDir_.empty()) {
        std::wstring picked;
        if (PickFolder(&picked, GetAppDataRoaming())) customDir_ = picked;
    }
    if (installDir_.empty() || (dataMode_ == DataMode::Custom && customDir_.empty())) {
        MessageBoxW(hwnd_, T(Str::DataHint), T(Str::SetupTitle), MB_ICONINFORMATION | MB_OK);
        return;
    }
    InstallRequest req;
    req.exePath = exePath_;
    req.installDir = InstallTargetDir();
    req.dataMode = dataMode_;
    req.customDir = customDir_;
    req.desktopShortcut = desktopShortcut_;
    req.startMenuShortcut = startMenuShortcut_;
    req.autostart = autostart_;
    req.chinese = chinese_;
    req.preferSlim = preferSlim_ && runtimeAvailable_;

    busy_ = true;
    page_ = Page::Progress;
    percent_ = 0;
    status_ = T(Str::StepPreparing);
    InvalidateRect(hwnd_, nullptr, FALSE);

    worker_ = std::thread([this, req]() {
        HWND target = hwnd_;
        auto progress = [](int percent, const wchar_t* text, void* user) {
            HWND hwnd = static_cast<HWND>(user);
            PostMessageW(hwnd, App::kMsgProgress, static_cast<WPARAM>(percent),
                         reinterpret_cast<LPARAM>(new std::wstring(text ? text : L"")));
        };
        const std::wstring err = RunInstall(req, progress, target);
        PostMessageW(target, App::kMsgFinished, 0, reinterpret_cast<LPARAM>(new std::wstring(err)));
    });
}

void App::BeginUninstall() {
    if (!error_.empty()) error_.clear();
    UninstallRequest req;
    req.exePath = exePath_;
    req.installDir = installDir_;
    req.dataRoot = dataRoot_.empty() ? ResolveDataRoot(dataMode_, installDir_, L"") : dataRoot_;
    req.deleteUserData = deleteData_;
    req.chinese = chinese_;

    busy_ = true;
    page_ = Page::UninstallProgress;
    percent_ = 0;
    status_ = T(Str::StepPreparing);
    InvalidateRect(hwnd_, nullptr, FALSE);

    worker_ = std::thread([this, req]() {
        HWND target = hwnd_;
        auto progress = [](int percent, const wchar_t* text, void* user) {
            HWND hwnd = static_cast<HWND>(user);
            PostMessageW(hwnd, App::kMsgProgress, static_cast<WPARAM>(percent),
                         reinterpret_cast<LPARAM>(new std::wstring(text ? text : L"")));
        };
        const std::wstring err = RunUninstall(req, progress, target);
        PostMessageW(target, App::kMsgFinished, 0, reinterpret_cast<LPARAM>(new std::wstring(err)));
    });
}

void App::SyncWindowSize() {
    const float scale = gfx_.scale();
    RECT frame{0, 0, static_cast<LONG>(kWindowWidth * scale + 0.5f),
               static_cast<LONG>(contentHeight_ * scale + 0.5f)};
    const DWORD style = static_cast<DWORD>(GetWindowLongPtrW(hwnd_, GWL_STYLE));
    if (!AdjustWindowRectExForDpi(&frame, style, FALSE, 0, GetDpiForWindow(hwnd_))) return;
    RECT current{};
    GetWindowRect(hwnd_, &current);
    const int width = frame.right - frame.left;
    const int height = frame.bottom - frame.top;
    if ((current.right - current.left) == width && (current.bottom - current.top) == height) return;
    SetWindowPos(hwnd_, nullptr, 0, 0, width, height, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

void App::Card(float x, float y, float w, float h) {
    const Theme& th = gfx_.theme();
    gfx_.FillRounded(Rect(x, y, w, h), kCardRadius, th.cardBg);
    gfx_.StrokeRounded(Rect(x + 0.5f, y + 0.5f, w - 1.0f, h - 1.0f), kCardRadius, th.cardBorder);
}

void App::LanguageSwitch(float x, float y) {
    const Theme& th = gfx_.theme();
    const float w = 132.0f;
    const float h = 28.0f;
    gfx_.FillRounded(Rect(x, y, w, h), h / 2, th.controlBg);
    gfx_.StrokeRounded(Rect(x + 0.5f, y + 0.5f, w - 1.0f, h - 1.0f), h / 2, th.controlBorder);
    const float half = w / 2;
    const D2D1_RECT_F zhRect = Rect(x + 2, y + 2, half - 2, h - 4);
    const D2D1_RECT_F enRect = Rect(x + half, y + 2, half - 2, h - 4);
    hotspots_.push_back({Hit::LangZh, zhRect});
    hotspots_.push_back({Hit::LangEn, enRect});
    if (chinese_) {
        gfx_.FillRounded(zhRect, (h - 4) / 2, th.accent);
    } else {
        gfx_.FillRounded(enRect, (h - 4) / 2, th.accent);
    }
    gfx_.Text(T(Str::LangChinese), zhRect, gfx_.fonts().small12, chinese_ ? th.accentText : th.textSecondary,
              DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    gfx_.Text(T(Str::LangEnglish), enRect, gfx_.fonts().small12, chinese_ ? th.textSecondary : th.accentText,
              DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
}

void App::ToggleChip(float x, float y, bool on) {
    const Theme& th = gfx_.theme();
    const float w = 40.0f;
    const float h = 22.0f;
    gfx_.FillRounded(Rect(x, y, w, h), h / 2, on ? th.accent : th.track);
    if (!on) gfx_.StrokeRounded(Rect(x + 0.5f, y + 0.5f, w - 1.0f, h - 1.0f), h / 2, th.controlBorder);
    const float knob = 8.0f;
    const float cx = on ? (x + w - 11.0f) : (x + 11.0f);
    gfx_.FillCircle(cx, y + h / 2, knob, on ? th.accentText : th.textSecondary);
}

void App::RadioRow(float x, float y, float w, bool selected, const wchar_t* title, const wchar_t* hint, Hit id) {
    const Theme& th = gfx_.theme();
    const float h = 44.0f;
    const D2D1_RECT_F row = Rect(x, y, w, h);
    hotspots_.push_back({id, row});
    if (hoverId_ == id && !selected) gfx_.FillRounded(row, 8.0f, Alpha(th.controlBg, 0.6f));
    const float cx = x + 20.0f;
    const float cy = y + 16.0f;
    gfx_.StrokeCircle(cx, cy, 7.0f, selected ? th.accent : th.controlBorder, 1.5f);
    if (selected) gfx_.FillCircle(cx, cy, 3.5f, th.accent);
    gfx_.Text(title, Rect(x + 36.0f, y + 6.0f, w - 44.0f, 18.0f), gfx_.fonts().body14, th.textPrimary);
    if (hint && *hint) {
        gfx_.Text(hint, Rect(x + 36.0f, y + 25.0f, w - 44.0f, 16.0f), gfx_.fonts().small12, th.textTertiary);
    }
}

void App::TextButton(const D2D1_RECT_F& r, const wchar_t* label, Hit id, bool primary, bool danger) {
    const Theme& th = gfx_.theme();
    hotspots_.push_back({id, r});
    const bool hover = hoverId_ == id;
    if (primary) {
        const D2D1_COLOR_F fill = danger ? (hover ? th.dangerHover : th.danger) : (hover ? th.accentHover : th.accent);
        gfx_.FillRounded(r, 8.0f, fill);
        gfx_.Text(label, r, gfx_.fonts().strong14, danger ? th.dangerText : th.accentText,
                  DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    } else {
        gfx_.FillRounded(r, 8.0f, hover ? th.controlHover : th.controlBg);
        gfx_.StrokeRounded(r, 8.0f, th.controlBorder);
        gfx_.Text(label, r, gfx_.fonts().strong14, th.textPrimary, DWRITE_TEXT_ALIGNMENT_CENTER,
                  DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }
}

float App::PaintHeader() {
    const Theme& th = gfx_.theme();
    // 应用图标，和主程序标题栏用的是同一张 ico
    gfx_.DrawIcon(Rect(kMargin, 20.0f, 44.0f, 44.0f));

    gfx_.Text(T(Str::AppName), Rect(kMargin + 58.0f, 20.0f, 260.0f, 28.0f), gfx_.fonts().title20, th.textPrimary);
    const std::wstring sub = Format(T(Str::VersionLabel), version_.c_str());
    gfx_.Text(sub, Rect(kMargin + 58.0f, 48.0f, 360.0f, 18.0f), gfx_.fonts().small12, th.textTertiary);
    // 卸载器的语言跟随安装时保存的选择，不再提供切换。
    if (mode_ == SetupMode::Install) LanguageSwitch(gfx_.width() - kMargin - 132.0f, 24.0f);
    if (page_ == Page::Data) {
        gfx_.Text(L"2 / 2", Rect(gfx_.width() - kMargin - 132.0f, 58.0f, 132.0f, 16.0f), gfx_.fonts().small12,
                  th.textTertiary, DWRITE_TEXT_ALIGNMENT_TRAILING);
    }
    return 84.0f;
}

// 居中版页首：图标 → 名称 → 版本（安装首页与卸载确认页共用）
float App::PaintHero(float top) {
    const Theme& th = gfx_.theme();
    const float cx = gfx_.width() / 2.0f;
    float y = top;
    gfx_.DrawIcon(Rect(cx - 32.0f, y, 64.0f, 64.0f));
    y += 64.0f + 16.0f;
    gfx_.Text(T(Str::AppName), Rect(cx - 220.0f, y, 440.0f, 30.0f), gfx_.fonts().title20, th.textPrimary,
              DWRITE_TEXT_ALIGNMENT_CENTER);
    y += 30.0f + 4.0f;
    gfx_.Text(Format(T(Str::VersionLabel), version_.c_str()), Rect(cx - 300.0f, y, 600.0f, 18.0f),
              gfx_.fonts().small12, th.textTertiary, DWRITE_TEXT_ALIGNMENT_CENTER);
    y += 18.0f;
    return y;
}

float App::PaintLocation() {
    const Theme& th = gfx_.theme();
    const float width = gfx_.width();
    const float w = width - kMargin * 2;
    const float cx = width / 2.0f;

    // 和第二页等高：以第二页实测高度为基准（还没画过时用默认高度），内容整组垂直居中。
    const float pageH = dataPageHeight_ > 0.0f ? dataPageHeight_ : kPageHeight;

    auto withSize = [](const std::wstring& info, uint64_t bytes) {
        if (bytes == 0) return info;
        return info + L"  ·  " + Format(T(Str::DiskUsage), MbText(bytes).c_str());
    };
    const std::wstring slimHint = withSize(T(Str::RuntimeSlimInfo), bytesSlim_);
    const std::wstring fullHint = withSize(T(Str::RuntimeFullInfo), bytesFull_);
    const float runtimeCardH = runtimeAvailable_ ? 140.0f : 118.0f;
    const bool pathOk = PathValid();
    const float installCardH = 116.0f;

    // 页首整组居中：图标 → 名称 → 版本 → 语言
    const float heroH = 64.0f + 16.0f + 30.0f + 4.0f + 18.0f + 14.0f + 28.0f;
    const float blockH = heroH + 18.0f + runtimeCardH + 12.0f + installCardH;
    const float footerY = std::max(pageH - 36.0f - 18.0f, blockH + 40.0f);
    float y = std::max(20.0f, (footerY - 18.0f - blockH) / 2.0f);

    // 升级只有这一页：不画步骤号，免得出现「1 / 2」却永远到不了第二页
    if (!upgrade_) {
        gfx_.Text(L"1 / 2", Rect(width - kMargin - 60.0f, 18.0f, 60.0f, 16.0f), gfx_.fonts().small12,
                  th.textTertiary, DWRITE_TEXT_ALIGNMENT_TRAILING);
    }
    y = PaintHero(y) + 14.0f;
    LanguageSwitch(cx - 66.0f, y);
    y += 28.0f + 18.0f;

    // 卡片 1：运行环境（检测到 .NET 8 运行时才允许选精简版，否则强制完整版）
    Card(kMargin, y, w, runtimeCardH);
    gfx_.Text(T(Str::SectionRuntime), Rect(kMargin + 18, y + 12, w - 36, 20), gfx_.fonts().h16, th.textPrimary);
    if (runtimeAvailable_) {
        RadioRow(kMargin + 12, y + 40, w - 24, preferSlim_, T(Str::RuntimeSlimName), slimHint.c_str(), Hit::PickSlim);
        RadioRow(kMargin + 12, y + 84, w - 24, !preferSlim_, T(Str::RuntimeFullName), fullHint.c_str(),
                 Hit::PickFull);
    } else {
        RadioRow(kMargin + 12, y + 40, w - 24, true, T(Str::RuntimeFullName), fullHint.c_str(), Hit::PickFull);
        gfx_.Text(T(Str::RuntimeMissing), Rect(kMargin + 36, y + 90, w - 60, 18), gfx_.fonts().small12,
                  th.textTertiary);
    }
    y += runtimeCardH + 12.0f;

    // 卡片 2：安装位置（路径可直接编辑，右侧「浏览…」仅作兜底）
    Card(kMargin, y, w, installCardH);
    gfx_.Text(T(Str::SectionInstall), Rect(kMargin + 18, y + 12, w - 36, 20), gfx_.fonts().h16, th.textPrimary);
    // 升级是就地覆盖原目录：位置锁定，输入框只读，也不提供「浏览…」
    if (upgrade_) {
        PaintPathField(kMargin + 18, y + 40, w - 36, 40);
    } else {
        PaintPathField(kMargin + 18, y + 40, w - 36 - 96, 40);
        TextButton(Rect(kMargin + w - 18 - 84, y + 40, 84, 40), T(Str::Browse), Hit::BrowseInstall, false);
    }
    if (!pathOk) {
        gfx_.Text(T(Str::PathInvalid), Rect(kMargin + 18, y + 88, w - 36, 18), gfx_.fonts().small12, th.danger);
    } else if (upgrade_) {
        const std::wstring note = installedVersion_.empty()
                                      ? std::wstring(T(Str::UpgradeDetectedNoVer))
                                      : Format(T(Str::UpgradeDetected), installedVersion_.c_str());
        gfx_.PushClip(Rect(kMargin + 18, y + 88, w - 36, 18));
        gfx_.Text(note, Rect(kMargin + 18, y + 88, w - 36, 18), gfx_.fonts().small12, th.accent);
        gfx_.PopClip();
    }
    y += installCardH;

    PaintFooter(footerY, upgrade_ ? T(Str::UpgradeAction) : T(Str::NextAction), Hit::Primary,
                T(Str::CancelAction), true);
    return footerY + 36.0f + 18.0f;
}

float App::PaintDataPage() {
    const Theme& th = gfx_.theme();
    const float w = gfx_.width() - kMargin * 2;
    float y = PaintHeader();

    // 卡片 1：选项（开机自启、桌面快捷方式）
    struct Option {
        Str  label;
        bool on;
        Hit  id;
        Str  hint;   // Count 表示没有副标题
    };
    const Option options[3] = {
        {Str::OptAutostart, autostart_, Hit::OptAutostart, Str::OptAutostartHint},
        {Str::OptDesktop, desktopShortcut_, Hit::OptDesktop, Str::Count},
        {Str::OptStartMenu, startMenuShortcut_, Hit::OptStartMenu, Str::Count},
    };
    constexpr float kRowH = 40.0f;
    constexpr float kRowHintH = 58.0f;
    float optContentH = 0.0f;
    for (const Option& opt : options) optContentH += (opt.hint != Str::Count ? kRowHintH : kRowH);
    const float optionsH = 42.0f + optContentH + 8.0f;
    Card(kMargin, y, w, optionsH);
    gfx_.Text(T(Str::SectionOptions), Rect(kMargin + 18, y + 12, w - 36, 20), gfx_.fonts().h16, th.textPrimary);
    float optY = y + 42.0f;
    for (const Option& opt : options) {
        const bool hasHint = opt.hint != Str::Count;
        const float rowH = hasHint ? kRowHintH : kRowH;
        const D2D1_RECT_F row = Rect(kMargin + 12, optY, w - 24, rowH);
        hotspots_.push_back({opt.id, row});
        if (hoverId_ == opt.id) gfx_.FillRounded(row, 8.0f, Alpha(th.controlBg, 0.6f));
        const float labelY = optY + (hasHint ? 8.0f : (rowH - 18.0f) / 2.0f);
        gfx_.Text(T(opt.label), Rect(kMargin + 24, labelY, w - 24 - 80, 18), gfx_.fonts().body14, th.textPrimary);
        if (hasHint) {
            gfx_.Text(T(opt.hint), Rect(kMargin + 24, labelY + 21.0f, w - 24 - 80, 16), gfx_.fonts().small12,
                      th.textTertiary);
        }
        ToggleChip(kMargin + w - 18 - 40, labelY - 2.0f, opt.on);
        optY += rowH;
    }
    y += optionsH + 12.0f;

    // 卡片 2：配置与日志位置（整页的核心决定）
    const bool customPicked = (dataMode_ == DataMode::Custom);
    const float hintH = existingConfig_ ? 18.0f : 0.0f;
    const float rowsTop = 58.0f;
    const float customRowH = customPicked ? 44.0f : 0.0f;
    const float cardH = rowsTop + 132.0f + customRowH + hintH + 12.0f;
    Card(kMargin, y, w, cardH);
    gfx_.Text(T(Str::SectionData), Rect(kMargin + 18, y + 12, w - 36, 20), gfx_.fonts().h16, th.textPrimary);
    gfx_.Text(T(Str::DataHint), Rect(kMargin + 18, y + 34, w - 36, 18), gfx_.fonts().small12, th.textSecondary);

    float rowY = y + rowsTop;
    RadioRow(kMargin + 12, rowY, w - 24, dataMode_ == DataMode::AppData, T(Str::DataAppData),
             Join(GetAppDataRoaming(), L"FanControl").c_str(), Hit::DataAppData);
    rowY += 44.0f;
    RadioRow(kMargin + 12, rowY, w - 24, dataMode_ == DataMode::ExeDirectory, T(Str::DataExeDir),
             T(Str::DataExeDirHint), Hit::DataExeDir);
    rowY += 44.0f;
    RadioRow(kMargin + 12, rowY, w - 24, customPicked, T(Str::DataCustom),
             customDir_.empty() ? T(Str::DataCustomHint) : customDir_.c_str(), Hit::DataCustom);
    rowY += 44.0f;
    if (customPicked) {
        gfx_.FillRounded(Rect(kMargin + 12, rowY + 2, w - 24, 36), 8.0f, Alpha(th.controlBg, 0.65f));
        const D2D1_RECT_F dirBox = Rect(kMargin + 24, rowY + 2, w - 24 - 100, 36);
        gfx_.PushClip(dirBox);
        gfx_.Text(customDir_.empty() ? T(Str::DataCustomHint) : customDir_, dirBox, gfx_.fonts().small12,
                  customDir_.empty() ? th.textTertiary : th.textSecondary, DWRITE_TEXT_ALIGNMENT_LEADING,
                  DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        gfx_.PopClip();
        TextButton(Rect(kMargin + w - 18 - 78, rowY + 7, 78, 26), T(Str::Browse), Hit::BrowseCustom, false);
        rowY += customRowH;
    }
    if (existingConfig_) {
        gfx_.Text(T(Str::ExistingConfig), Rect(kMargin + 18, rowY + 4, w - 36, 18), gfx_.fonts().small12, th.accent);
    }
    y += cardH + 18.0f;

    Checkbox(kMargin + 2.0f, y + 9.0f, T(Str::OptLaunch), launchAfter_, Hit::OptLaunch);
    PaintFooter(y, upgrade_ ? T(Str::UpgradeAction) : T(Str::InstallAction), Hit::Primary, T(Str::BackAction));
    dataPageHeight_ = y + 36.0f + 20.0f;   // 首页按这个高度排版，两页窗口大小一致
    return dataPageHeight_;
}

void App::PaintPathField(float x, float y, float w, float h) {
    const Theme& th = gfx_.theme();
    const D2D1_RECT_F field = Rect(x, y, w, h);
    // 升级页面路径锁定：不登记热区、不画光标，点击与键盘输入都落不到这里
    const bool editable = !upgrade_;
    if (editable) hotspots_.push_back({Hit::PathField, field});
    pathFieldRect_ = field;   // 点击换算文字索引要用这一帧的实际位置

    const bool invalid = !PathValid();
    const bool focused = pathFocused_ && editable;
    gfx_.FillRounded(field, 8.0f, editable ? th.controlBg : Alpha(th.controlBg, 0.6f));
    const D2D1_COLOR_F border = focused ? th.accent : (invalid ? Alpha(th.danger, 0.8f) : th.controlBorder);
    gfx_.StrokeRounded(Rect(x + 0.5f, y + 0.5f, w - 1.0f, h - 1.0f), 8.0f, border, focused ? 1.6f : 1.0f);

    const float padX = 12.0f;
    const float avail = w - padX * 2.0f;
    const size_t caret = caret_ < installDir_.size() ? caret_ : installDir_.size();
    const float caretX = gfx_.Measure(installDir_.substr(0, caret), gfx_.fonts().body14);
    const float offset = caretX > avail - 6.0f ? caretX - (avail - 6.0f) : 0.0f;
    pathScroll_ = offset;

    const D2D1_RECT_F textBox = Rect(x + padX - offset, y + (h - 18.0f) / 2.0f, 4000.0f, 18.0f);
    gfx_.PushClip(Rect(x + 2.0f, y + 2.0f, w - 4.0f, h - 4.0f));
    if (focused && HasPathSelection()) {
        const float left = gfx_.Measure(installDir_.substr(0, PathSelBegin()), gfx_.fonts().body14);
        const float right = gfx_.Measure(installDir_.substr(0, PathSelEnd()), gfx_.fonts().body14);
        gfx_.FillRounded(Rect(x + padX - offset + left, y + 9.0f, right - left, h - 18.0f), 3.0f,
                         Alpha(th.accent, 0.35f));
    }
    gfx_.Text(installDir_, textBox, gfx_.fonts().body14, th.textPrimary);
    if (focused && caretOn_ && !HasPathSelection()) {
        const float cx = x + padX + caretX - offset;
        gfx_.DrawLine(cx, y + h / 2 - 9.0f, cx, y + h / 2 + 9.0f, th.textPrimary, 1.4f);
    }
    gfx_.PopClip();
}

void App::FocusPath(bool on) {
    if (upgrade_ && on) return;   // 升级页面路径锁定，不接受焦点
    if (pathFocused_ == on) return;
    pathFocused_ = on;
    caretOn_ = true;
    if (on) {
        SetTimer(hwnd_, 1, 530, nullptr);
        caret_ = installDir_.size();
        selAnchor_ = caret_;
        selecting_ = false;
    } else {
        KillTimer(hwnd_, 1);
        CommitPath();
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void App::InsertText(const std::wstring& text) {
    (void)DeletePathSelection();   // 有选区时输入即替换
    if (caret_ > installDir_.size()) caret_ = installDir_.size();
    installDir_.insert(caret_, text);
    caret_ += text.size();
    selAnchor_ = caret_;
    caretOn_ = true;
    RefreshExistingConfig();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void App::CommitPath() {
    std::wstring p = StripSeparators(installDir_);
    if (p.empty()) p = StripSeparators(DefaultInstallDir());
    installDir_ = p;
    caret_ = installDir_.size();
    selAnchor_ = caret_;
    RefreshExistingConfig();
}

// extend=true 时保留原有选区另一端（按住 Shift 或鼠标拖选）。
void App::SetCaretPos(size_t pos, bool extend) {
    if (pos > installDir_.size()) pos = installDir_.size();
    caret_ = pos;
    if (!extend) selAnchor_ = pos;
    caretOn_ = true;
    if (pathFocused_) {
        KillTimer(hwnd_, 1);
        SetTimer(hwnd_, 1, 530, nullptr);
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

void App::SelectAllPath() {
    selAnchor_ = 0;
    caret_ = installDir_.size();
    caretOn_ = true;
    InvalidateRect(hwnd_, nullptr, FALSE);
}

bool App::DeletePathSelection() {
    if (!HasPathSelection()) return false;
    const size_t begin = PathSelBegin();
    installDir_.erase(begin, PathSelEnd() - begin);
    caret_ = begin;
    selAnchor_ = begin;
    RefreshExistingConfig();
    InvalidateRect(hwnd_, nullptr, FALSE);
    return true;
}

// 水平坐标 → 字符索引：以每个字符左右边界的中间点为界，点哪半边就落在哪。
size_t App::PathIndexAtX(float relX) {
    if (relX <= 0.0f) return 0;
    const size_t len = installDir_.size();
    float prev = 0.0f;
    for (size_t i = 1; i <= len; ++i) {
        const float cur = gfx_.Measure(installDir_.substr(0, i), gfx_.fonts().body14);
        if (relX < (prev + cur) / 2.0f) return i - 1;
        prev = cur;
    }
    return len;
}

std::wstring App::SelectedPathText() const {
    if (!HasPathSelection()) return std::wstring();
    return installDir_.substr(PathSelBegin(), PathSelEnd() - PathSelBegin());
}

bool App::PathValid() const {
    const std::wstring& p = installDir_;
    if (p.size() < 3) return false;
    const wchar_t c0 = p[0];
    const bool letter = (c0 >= L'A' && c0 <= L'Z') || (c0 >= L'a' && c0 <= L'z');
    bool ok = letter && p[1] == L':' && (p[2] == L'\\' || p[2] == L'/');
    if (!ok && p.size() >= 2 && p[0] == L'\\' && p[1] == L'\\') ok = true;
    if (!ok) return false;
    for (const wchar_t c : p) {
        if (c < 32) return false;
        if (c == L'<' || c == L'>' || c == L'|' || c == L'"' || c == L'?' || c == L'*') return false;
    }
    return true;
}

void App::Checkbox(float x, float y, const wchar_t* label, bool on, Hit id) {
    const Theme& th = gfx_.theme();
    constexpr float kBox = 18.0f;
    const D2D1_RECT_F box = Rect(x, y, kBox, kBox);
    hotspots_.push_back({id, Rect(x - 4.0f, y - 9.0f, kBox + 260.0f, 36.0f)});
    if (on) {
        gfx_.FillRounded(box, 5.0f, hoverId_ == id ? th.accentHover : th.accent);
        gfx_.DrawLine(x + 4.4f, y + 9.4f, x + 7.4f, y + 12.6f, th.accentText, 1.8f);
        gfx_.DrawLine(x + 7.4f, y + 12.6f, x + 13.6f, y + 5.4f, th.accentText, 1.8f);
    } else {
        gfx_.StrokeRounded(box, 5.0f, hoverId_ == id ? th.textTertiary : th.controlBorder, 1.2f);
    }
    gfx_.Text(label, Rect(x + kBox + 10.0f, y - 2.0f, 400.0f, 22.0f), gfx_.fonts().body14, th.textSecondary,
              DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
}

float App::PaintProgress(const wchar_t* title) {
    const Theme& th = gfx_.theme();
    const float w = gfx_.width() - kMargin * 2;
    // 卸载器全程用居中页首，和确认页保持一套版式。
    const float top = (mode_ == SetupMode::Uninstall) ? PaintHero(20.0f) + 18.0f : PaintHeader();
    const bool failed = !error_.empty() && !busy_;
    // 所有页面统一用 kPageHeight：进度页内容少，就把卡片在页首与页脚之间居中。
    const float footerY = kPageHeight - 36.0f - 18.0f;
    const float y = failed ? (top + std::max(0.0f, (footerY - 18.0f - top - 108.0f) / 2.0f))
                           : (top + std::max(0.0f, (kPageHeight - top - 108.0f) / 2.0f));
    Card(kMargin, y, w, 108.0f);
    gfx_.Text(title, Rect(kMargin + 18, y + 14, w - 36, 26), gfx_.fonts().title20, th.textPrimary);
    if (failed) {
        const std::wstring msg = Format(T(Str::ErrGeneric), error_.c_str());
        gfx_.PushClip(Rect(kMargin + 18, y + 46, w - 36, 44));
        gfx_.Text(msg, Rect(kMargin + 18, y + 46, w - 36, 44), gfx_.fonts().body14, th.danger);
        gfx_.PopClip();
        PaintFooter(footerY, T(Str::FinishAction), Hit::Primary, nullptr, true);
        return kPageHeight;
    }
    gfx_.Text(status_, Rect(kMargin + 18, y + 46, w - 36, 20), gfx_.fonts().body14, th.textSecondary);
    const float barW = w - 36.0f;
    gfx_.FillRounded(Rect(kMargin + 18, y + 74, barW, 8.0f), 4.0f, th.track);
    const float fill = barW * static_cast<float>(percent_) / 100.0f;
    if (fill > 1.0f) gfx_.FillRounded(Rect(kMargin + 18, y + 74, fill, 8.0f), 4.0f, th.accent);
    gfx_.Text(Format(L"%d%%", percent_), Rect(kMargin + 18, y + 86, barW, 18), gfx_.fonts().small12,
              th.textTertiary, DWRITE_TEXT_ALIGNMENT_TRAILING);
    return kPageHeight;
}

float App::PaintDone(const wchar_t* title, const wchar_t* subtitle, bool uninstall) {
    const Theme& th = gfx_.theme();
    const float w = gfx_.width() - kMargin * 2;
    const float top = (mode_ == SetupMode::Uninstall) ? PaintHero(20.0f) + 18.0f : PaintHeader();

    const std::wstring dataRoot =
        uninstall ? dataRoot_ : ResolveDataRoot(dataMode_, InstallTargetDir(), customDir_);
    const float rowsTop = 100.0f;
    const float cardH = rowsTop + (uninstall ? 50.0f : 120.0f) + 10.0f;
    // 和进度页一样：窗口高度固定，卡片在页脚之上居中。
    const float footerY = kPageHeight - 36.0f - 18.0f;
    float y = top + std::max(0.0f, (footerY - 18.0f - top - cardH) / 2.0f);
    Card(kMargin, y, w, cardH);

    gfx_.FillCircle(kMargin + 34, y + 40, 15.0f, Alpha(th.ok, 0.18f));
    gfx_.StrokeCircle(kMargin + 34, y + 40, 15.0f, th.ok, 1.6f);
    gfx_.Text(L"\u2713", Rect(kMargin + 19, y + 26, 30, 28), gfx_.fonts().body14, th.ok,
              DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    gfx_.Text(title, Rect(kMargin + 60, y + 22, w - 80, 28), gfx_.fonts().title20, th.textPrimary);
    gfx_.PushClip(Rect(kMargin + 60, y + 52, w - 80, 40));
    gfx_.Text(subtitle, Rect(kMargin + 60, y + 52, w - 80, 40), gfx_.fonts().small12, th.textSecondary);
    gfx_.PopClip();
    gfx_.DrawLine(kMargin + 18, y + rowsTop - 12.0f, kMargin + w - 18, y + rowsTop - 12.0f, th.divider);

    struct SummaryRow {
        const wchar_t* label;
        std::wstring   value;
    };
    std::vector<SummaryRow> rows;
    if (uninstall) {
        rows.push_back({T(Str::SumData), dataRoot});
    } else {
        rows.push_back({T(Str::SumInstall), InstallTargetDir()});
        rows.push_back({T(Str::SumData), dataRoot});
        rows.push_back({T(Str::SumLanguage), chinese_ ? L"\u4e2d\u6587" : L"English"});
    }
    float ry = y + rowsTop;
    for (const SummaryRow& row : rows) {
        gfx_.Text(row.label, Rect(kMargin + 18, ry, 120, 16), gfx_.fonts().small12, th.textTertiary);
        gfx_.PushClip(Rect(kMargin + 18, ry + 18, w - 36, 20));
        gfx_.Text(row.value, Rect(kMargin + 18, ry + 18, w - 36, 20), gfx_.fonts().body14, th.textPrimary);
        gfx_.PopClip();
        ry += 40.0f;
    }
    PaintFooter(footerY, T(Str::FinishAction), Hit::Primary, nullptr, true);
    return kPageHeight;
}

float App::PaintConfirm() {
    const Theme& th = gfx_.theme();
    const float w = gfx_.width() - kMargin * 2;
    const float heroBottom = PaintHero(20.0f);

    // 和安装页同宽同高：卡片在页首与页脚之间垂直居中。
    const float cardH = 200.0f;
    const float footerY = kPageHeight - 36.0f - 18.0f;
    const float cardTop = heroBottom + 18.0f;
    float y = cardTop + std::max(0.0f, (footerY - 18.0f - cardTop - cardH) / 2.0f);
    Card(kMargin, y, w, cardH);
    gfx_.Text(T(Str::ConfirmUninstall), Rect(kMargin + 18, y + 16, w - 36, 28), gfx_.fonts().title20,
              th.textPrimary);
    gfx_.PushClip(Rect(kMargin + 18, y + 48, w - 36, 40));
    gfx_.Text(T(Str::ConfirmUninstallHint), Rect(kMargin + 18, y + 48, w - 36, 40), gfx_.fonts().small12,
              th.textSecondary);
    gfx_.PopClip();
    gfx_.DrawLine(kMargin + 18, y + 98, kMargin + w - 18, y + 98, th.divider);

    const D2D1_RECT_F row = Rect(kMargin + 12, y + 106, w - 24, 44.0f);
    hotspots_.push_back({Hit::OptDeleteData, row});
    if (hoverId_ == Hit::OptDeleteData) gfx_.FillRounded(row, 8.0f, Alpha(th.controlBg, 0.6f));
    gfx_.Text(T(Str::DeleteData), Rect(kMargin + 24, y + 111, w - 24 - 80, 18), gfx_.fonts().body14, th.textPrimary);
    gfx_.Text(T(Str::DeleteDataHint), Rect(kMargin + 24, y + 129, w - 24 - 80, 16), gfx_.fonts().small12,
              th.textTertiary);
    ToggleChip(kMargin + w - 18 - 40, y + 117, deleteData_);

    const std::wstring kept = Format(T(Str::DataKept), DataRootText().c_str());
    gfx_.PushClip(Rect(kMargin + 24, y + 160, w - 48, 30));
    gfx_.Text(deleteData_ ? std::wstring(T(Str::DeleteDataHint)) : kept, Rect(kMargin + 24, y + 160, w - 48, 30),
              gfx_.fonts().small12, deleteData_ ? th.danger : th.textTertiary);
    gfx_.PopClip();
    PaintFooter(footerY, T(Str::UninstallAction), Hit::Primary, T(Str::CancelAction), true, true);
    return footerY + 36.0f + 18.0f;
}

void App::PaintFooter(float y, const wchar_t* primary, Hit primaryHit, const wchar_t* secondary,
                       bool centered, bool danger) {
    const float w = gfx_.width() - kMargin * 2;
    const float primaryW = 104.0f;
    const float secondaryW = 88.0f;
    float primaryX = kMargin + w - primaryW;
    float secondaryX = primaryX - 10.0f - secondaryW;
    if (centered) {
        const float total = secondary ? primaryW + 10.0f + secondaryW : primaryW;
        secondaryX = gfx_.width() / 2.0f - total / 2.0f;
        primaryX = secondaryX + (secondary ? secondaryW + 10.0f : 0.0f);
    }
    if (secondary) {
        TextButton(Rect(secondaryX, y, secondaryW, 36.0f), secondary, Hit::Secondary, false);
    }
    if (primary) TextButton(Rect(primaryX, y, primaryW, 36.0f), primary, primaryHit, true, danger);
}

bool App::Paint() {
    const Theme& th = gfx_.theme();
    if (!gfx_.Begin(th.windowBg)) return false;
    hotspots_.clear();
    float height = 0.0f;
    switch (page_) {
    case Page::Configure:
        height = PaintLocation();
        break;
    case Page::Data:
        height = PaintDataPage();
        break;
    case Page::Progress:
        height = PaintProgress(upgrade_ ? T(Str::ProgressUpgrade) : T(Str::ProgressInstall));
        break;
    case Page::UninstallProgress:
        height = PaintProgress(T(Str::ProgressUninstall));
        break;
    case Page::Done:
        height = upgrade_ ? PaintDone(T(Str::DoneTitleUpgrade), T(Str::DoneSubtitleUpgrade), false)
                          : PaintDone(T(Str::DoneTitle), T(Str::DoneSubtitle), false);
        break;
    case Page::UninstallDone:
        height = PaintDone(T(Str::UninstallDoneTitle), T(Str::UninstallDoneSubtitle), true);
        break;
    case Page::Confirm:
        height = PaintConfirm();
        break;
    }
    // 窗口高度取整页基准：进度/完成页内容少也不会把窗口缩矮。
    contentHeight_ = std::max(kPageHeight, height);
    height = contentHeight_;
    gfx_.End();
    if (!sized_ || std::fabs(height - lastHeight_) > 0.5f) {
        lastHeight_ = height;
        sized_ = true;
        SyncWindowSize();
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
    return true;
}

}  // namespace fc
