#include "app.h"

#include <shlobj.h>
#include <shobjidl.h>
#include <dwmapi.h>

#include <winver.h>

#include <cmath>

#include "i18n.h"
#include "payload.h"
#include "sysutil.h"

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
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
    if (size == 0) return L"1.4.0";
    std::vector<BYTE> data(size);
    if (!GetFileVersionInfoW(path.c_str(), 0, size, data.data())) return L"1.4.0";
    VS_FIXEDFILEINFO* info = nullptr;
    UINT len = 0;
    if (!VerQueryValueW(data.data(), L"\\", reinterpret_cast<void**>(&info), &len) || !info) return L"1.4.0";
    return Format(L"%u.%u.%u", HIWORD(info->dwFileVersionMS), LOWORD(info->dwFileVersionMS),
                  HIWORD(info->dwFileVersionLS));
}

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
    if (!gfx_.Init(hwnd_, d2d_, dw_)) return false;

    exePath_ = GetModulePath();
    version_ = ReadOwnVersion();
    preferSlim_ = HasDesktopRuntime8();

    // 卸载器跟随已安装时选定的语言；安装器跟随系统语言。
    InstallInfo info;
    const bool haveInfo = ReadInstallInfo(&info);
    if (mode_ == SetupMode::Uninstall && haveInfo && !info.language.empty()) {
        chinese_ = info.language.rfind(L"zh", 0) == 0;
    } else {
        chinese_ = GetSystemLocaleName().rfind(L"zh", 0) == 0;
    }
    SetUiLanguage(chinese_);

    installDir_ = (mode_ == SetupMode::Uninstall && haveInfo && !info.installDir.empty()) ? info.installDir
                                                                                          : DefaultInstallDir();
    if (mode_ == SetupMode::Uninstall) {
        dataRoot_ = info.dataRoot.empty() ? ResolveDataRoot(info.dataMode, installDir_, L"") : info.dataRoot;
        page_ = Page::Confirm;
        dataMode_ = info.dataMode;
    } else {
        dataMode_ = DataMode::AppData;
        page_ = Page::Configure;
    }
    status_ = T(Str::StepPreparing);

    PayloadSet payload;
    if (PayloadSet::Load(exePath_, &payload, nullptr)) {
        if (const PayloadSegment* seg = payload.Select(preferSlim_)) payloadBytes_ = seg->rawSize;
    }
    RefreshExistingConfig();
    ApplyStrings();
    ApplyDwm();
    return true;
}

void App::Destroy() {
    if (worker_.joinable()) worker_.detach();
    gfx_.Shutdown();
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
    return ResolveDataRoot(dataMode_, installDir_, customDir_);
}

void App::RefreshExistingConfig() {
    existingConfig_ = HasExistingConfig(installDir_, ResolveDataRoot(dataMode_, installDir_, customDir_));
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
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        BeginPaint(hwnd_, &ps);
        Paint();
        EndPaint(hwnd_, &ps);
        return 0;
    }
    case WM_MOUSEMOVE: {
        const float x = static_cast<float>(GET_X_LPARAM(lp)) / gfx_.dpi();
        const float y = static_cast<float>(GET_Y_LPARAM(lp)) / gfx_.dpi();
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
        const float x = static_cast<float>(GET_X_LPARAM(lp)) / gfx_.dpi();
        const float y = static_cast<float>(GET_Y_LPARAM(lp)) / gfx_.dpi();
        pressedId_ = HitAt(x, y);
        return 0;
    }
    case WM_LBUTTONUP: {
        ReleaseCapture();
        const float x = static_cast<float>(GET_X_LPARAM(lp)) / gfx_.dpi();
        const float y = static_cast<float>(GET_Y_LPARAM(lp)) / gfx_.dpi();
        const Hit hit = HitAt(x, y);
        const Hit pressed = pressedId_;
        pressedId_ = Hit::None;
        if (hit != Hit::None && hit == pressed) Activate(hit);
        return 0;
    }
    case WM_SETCURSOR:
        SetCursor(LoadCursorW(nullptr, hoverId_ != Hit::None ? IDC_HAND : IDC_ARROW));
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
        gfx_.Resize(static_cast<UINT>(rect->right - rect->left), static_cast<UINT>(rect->bottom - rect->top));
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
    case Hit::ChangeDir: {
        std::wstring picked;
        if (PickFolder(&picked, installDir_)) {
            installDir_ = picked;
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
            BeginInstall();
        } else if (page_ == Page::Confirm) {
            BeginUninstall();
        } else {
            DestroyWindow(hwnd_);
        }
        break;
    case Hit::Secondary:
        if (page_ == Page::Progress || page_ == Page::UninstallProgress) {
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
    req.installDir = installDir_;
    req.dataMode = dataMode_;
    req.customDir = customDir_;
    req.desktopShortcut = desktopShortcut_;
    req.autostart = autostart_;
    req.launchAfter = launchAfter_;
    req.chinese = chinese_;
    req.preferSlim = preferSlim_;

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
    const float scale = gfx_.dpi();
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

void App::TextButton(const D2D1_RECT_F& r, const wchar_t* label, Hit id, bool primary) {
    const Theme& th = gfx_.theme();
    hotspots_.push_back({id, r});
    const bool hover = hoverId_ == id;
    if (primary) {
        gfx_.FillRounded(r, 8.0f, hover ? th.accentHover : th.accent);
        gfx_.Text(label, r, gfx_.fonts().strong14, th.accentText, DWRITE_TEXT_ALIGNMENT_CENTER,
                  DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    } else {
        gfx_.FillRounded(r, 8.0f, hover ? th.controlHover : th.controlBg);
        gfx_.StrokeRounded(r, 8.0f, th.controlBorder);
        gfx_.Text(label, r, gfx_.fonts().strong14, th.textPrimary, DWRITE_TEXT_ALIGNMENT_CENTER,
                  DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }
}

float App::PaintHeader() {
    const Theme& th = gfx_.theme();
    const float scale = 1.0f;
    // 风扇标记：圆环 + 四片斜叶，和主程序图标同色系
    const float cx = kMargin + 20.0f;
    const float cy = 22.0f + 20.0f;
    gfx_.FillRounded(Rect(kMargin, 22.0f, 40.0f, 40.0f), 10.0f, th.controlBg);
    gfx_.StrokeRounded(Rect(kMargin + 0.5f, 22.5f, 39.0f, 39.0f), 10.0f, th.cardBorder);
    gfx_.StrokeCircle(cx, cy, 11.0f * scale, Alpha(th.accent, 0.55f), 1.4f);
    for (int i = 0; i < 4; ++i) {
        const float angle = static_cast<float>(i) * 1.5707963f + 0.6f;
        const float bx = cx + std::cos(angle) * 7.0f;
        const float by = cy + std::sin(angle) * 7.0f;
        gfx_.FillEllipse(bx, by, 4.6f, 2.6f, th.accent);
    }
    gfx_.FillCircle(cx, cy, 2.2f, th.accent);

    gfx_.Text(T(Str::AppName), Rect(kMargin + 54.0f, 20.0f, 260.0f, 28.0f), gfx_.fonts().title20, th.textPrimary);
    const std::wstring sub = std::wstring(T(Str::Subtitle)) + L"  ·  " + Format(T(Str::VersionLabel), version_.c_str());
    gfx_.Text(sub, Rect(kMargin + 54.0f, 48.0f, 360.0f, 18.0f), gfx_.fonts().small12, th.textTertiary);
    LanguageSwitch(gfx_.width() - kMargin - 132.0f, 24.0f);
    return 84.0f;
}

float App::PaintConfigure() {
    const Theme& th = gfx_.theme();
    const float w = gfx_.width() - kMargin * 2;
    float y = PaintHeader();

    // 卡片 1：安装位置
    Card(kMargin, y, w, 88.0f);
    gfx_.Text(T(Str::SectionInstall), Rect(kMargin + 18, y + 12, w - 36, 20), gfx_.fonts().h16, th.textPrimary);
    const D2D1_RECT_F pathBox = Rect(kMargin + 18, y + 38, w - 36 - 100, 20);
    gfx_.PushClip(pathBox);
    gfx_.Text(installDir_, pathBox, gfx_.fonts().body14, th.textSecondary);
    gfx_.PopClip();
    std::wstring hint = T(preferSlim_ ? Str::RuntimeSlim : Str::RuntimeFull);
    if (payloadBytes_ > 0) hint += L"  ·  " + Format(T(Str::DiskUsage), MbText(payloadBytes_).c_str());
    gfx_.Text(hint, Rect(kMargin + 18, y + 60, w - 36 - 100, 18), gfx_.fonts().small12, th.textTertiary);
    TextButton(Rect(kMargin + w - 18 - 84, y + 23, 84, 28), T(Str::Change), Hit::ChangeDir, false);
    y += 88.0f + 12.0f;

    // 卡片 2：数据存储
    const bool customPicked = (dataMode_ == DataMode::Custom);
    const float hintH = existingConfig_ ? 18.0f : 0.0f;
    float rowsTop = 58.0f;
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
    y += cardH + 12.0f;

    // 卡片 3：选项
    Card(kMargin, y, w, 166.0f);
    gfx_.Text(T(Str::SectionOptions), Rect(kMargin + 18, y + 12, w - 36, 20), gfx_.fonts().h16, th.textPrimary);
    struct Option {
        Str  label;
        bool on;
        Hit  id;
        Str  hint;   // Count 表示没有副标题
    };
    const Option options[3] = {
        {Str::OptDesktop, desktopShortcut_, Hit::OptDesktop, Str::Count},
        {Str::OptAutostart, autostart_, Hit::OptAutostart, Str::OptAutostartHint},
        {Str::OptLaunch, launchAfter_, Hit::OptLaunch, Str::Count},
    };
    float optY = y + 42.0f;
    for (const Option& opt : options) {
        const D2D1_RECT_F row = Rect(kMargin + 12, optY, w - 24, 40.0f);
        hotspots_.push_back({opt.id, row});
        if (hoverId_ == opt.id) gfx_.FillRounded(row, 8.0f, Alpha(th.controlBg, 0.6f));
        const bool hasHint = opt.hint != Str::Count;
        gfx_.Text(T(opt.label), Rect(kMargin + 24, optY + (hasHint ? 3.0f : 2.0f), w - 24 - 80, 18),
                  gfx_.fonts().body14, th.textPrimary);
        if (hasHint) {
            gfx_.Text(T(opt.hint), Rect(kMargin + 24, optY + 21.0f, w - 24 - 80, 16), gfx_.fonts().small12,
                      th.textTertiary);
        }
        ToggleChip(kMargin + w - 18 - 40, optY + (hasHint ? 9.0f : 9.0f), opt.on);
        optY += 40.0f;
    }
    y += 166.0f;

    const float footerY = y + 16.0f;
    PaintFooter(footerY, T(Str::InstallAction), Hit::Primary, T(Str::CancelAction));
    return footerY + 36.0f + 20.0f;
}

float App::PaintProgress(const wchar_t* title) {
    const Theme& th = gfx_.theme();
    const float w = gfx_.width() - kMargin * 2;
    const float y = PaintHeader();
    const bool failed = !error_.empty() && !busy_;
    Card(kMargin, y, w, 108.0f);
    gfx_.Text(title, Rect(kMargin + 18, y + 14, w - 36, 26), gfx_.fonts().title20, th.textPrimary);
    if (failed) {
        const std::wstring msg = Format(T(Str::ErrGeneric), error_.c_str());
        gfx_.PushClip(Rect(kMargin + 18, y + 46, w - 36, 44));
        gfx_.Text(msg, Rect(kMargin + 18, y + 46, w - 36, 44), gfx_.fonts().body14, th.danger);
        gfx_.PopClip();
        PaintFooter(y + 108.0f + 16.0f, T(Str::FinishAction), Hit::Primary, nullptr);
        return y + 108.0f + 16.0f + 36.0f + 20.0f;
    }
    gfx_.Text(status_, Rect(kMargin + 18, y + 46, w - 36, 20), gfx_.fonts().body14, th.textSecondary);
    const float barW = w - 36.0f;
    gfx_.FillRounded(Rect(kMargin + 18, y + 74, barW, 8.0f), 4.0f, th.track);
    const float fill = barW * static_cast<float>(percent_) / 100.0f;
    if (fill > 1.0f) gfx_.FillRounded(Rect(kMargin + 18, y + 74, fill, 8.0f), 4.0f, th.accent);
    gfx_.Text(Format(L"%d%%", percent_), Rect(kMargin + 18, y + 86, barW, 18), gfx_.fonts().small12,
              th.textTertiary, DWRITE_TEXT_ALIGNMENT_TRAILING);
    return y + 108.0f + 24.0f;
}

float App::PaintDone(const wchar_t* title, const wchar_t* subtitle, bool uninstall) {
    const Theme& th = gfx_.theme();
    const float w = gfx_.width() - kMargin * 2;
    float y = PaintHeader();

    const std::wstring dataRoot =
        uninstall ? dataRoot_ : ResolveDataRoot(dataMode_, installDir_, customDir_);
    const float rowsTop = 100.0f;
    const float cardH = rowsTop + (uninstall ? 50.0f : 120.0f) + 10.0f;
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
        rows.push_back({T(Str::SumInstall), installDir_});
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
    y += cardH + 16.0f;
    PaintFooter(y, T(Str::FinishAction), Hit::Primary, nullptr);
    return y + 36.0f + 20.0f;
}

float App::PaintConfirm() {
    const Theme& th = gfx_.theme();
    const float w = gfx_.width() - kMargin * 2;
    float y = PaintHeader();

    const float cardH = 200.0f;
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
    y += cardH + 16.0f;
    PaintFooter(y, T(Str::UninstallAction), Hit::Primary, T(Str::CancelAction));
    return y + 36.0f + 20.0f;
}

void App::PaintFooter(float y, const wchar_t* primary, Hit primaryHit, const wchar_t* secondary) {
    const float w = gfx_.width() - kMargin * 2;
    const float primaryW = 104.0f;
    const D2D1_RECT_F primaryRect = Rect(kMargin + w - primaryW, y, primaryW, 36.0f);
    bool showPrimary = (primary != nullptr);
    if (secondary) {
        const float secondaryW = 88.0f;
        TextButton(Rect(kMargin + w - primaryW - 10.0f - secondaryW, y, secondaryW, 36.0f), secondary,
                   Hit::Secondary, false);
    }
    if (showPrimary) TextButton(primaryRect, primary, primaryHit, true);
}

bool App::Paint() {
    const Theme& th = gfx_.theme();
    if (!gfx_.Begin(th.windowBg)) return false;
    hotspots_.clear();
    float height = 0.0f;
    switch (page_) {
    case Page::Configure:
        height = PaintConfigure();
        break;
    case Page::Progress:
        height = PaintProgress(T(Str::ProgressInstall));
        break;
    case Page::UninstallProgress:
        height = PaintProgress(T(Str::ProgressUninstall));
        break;
    case Page::Done:
        height = PaintDone(T(Str::DoneTitle), T(Str::DoneSubtitle), false);
        break;
    case Page::UninstallDone:
        height = PaintDone(T(Str::UninstallDoneTitle), T(Str::UninstallDoneSubtitle), true);
        break;
    case Page::Confirm:
        height = PaintConfirm();
        break;
    }
    contentHeight_ = height;
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
