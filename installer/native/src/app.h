#pragma once
// 安装器界面：单窗口、五页状态机（配置/进度/完成 + 卸载确认/进度/完成）。
// 绘制采用即时模式：每次 Paint 重建热区清单，鼠标事件回查清单。
#include <windows.h>

#include <d2d1.h>
#include <dwrite.h>

#include <string>
#include <thread>
#include <vector>

#include "install.h"
#include "theme.h"
#include "ui.h"

namespace fc {

enum class SetupMode { Install, Uninstall };

class App {
public:
    static constexpr UINT kMsgProgress = WM_APP + 11;
    static constexpr UINT kMsgFinished = WM_APP + 12;

    bool    Create(HWND hwnd, SetupMode mode);
    void    Destroy();
    bool    Paint();
    LRESULT HandleMessage(UINT msg, WPARAM wp, LPARAM lp);

private:
    enum class Page { Configure, Progress, Done, Confirm, UninstallProgress, UninstallDone };
    enum class Hit {
        None,
        LangZh,
        LangEn,
        ChangeDir,
        DataAppData,
        DataExeDir,
        DataCustom,
        BrowseCustom,
        OptDesktop,
        OptAutostart,
        OptLaunch,
        OptDeleteData,
        Primary,
        Secondary,
    };
    struct Hotspot {
        Hit          id;
        D2D1_RECT_F  rect;
    };

    float PaintHeader();
    float PaintConfigure();
    float PaintProgress(const wchar_t* title);
    float PaintDone(const wchar_t* title, const wchar_t* subtitle, bool uninstall);
    float PaintConfirm();
    void  PaintFooter(float y, const wchar_t* primary, Hit primaryHit, const wchar_t* secondary);
    void  Card(float x, float y, float w, float h);
    void  LanguageSwitch(float x, float y);
    void  ToggleChip(float x, float y, bool on);
    void  RadioRow(float x, float y, float w, bool selected, const wchar_t* title, const wchar_t* hint, Hit id);
    void  TextButton(const D2D1_RECT_F& r, const wchar_t* label, Hit id, bool primary);
    Hit   HitAt(float x, float y);
    void  Activate(Hit id);
    void  BeginInstall();
    void  BeginUninstall();
    void  SyncWindowSize();
    void  ApplyStrings();
    void  ApplyDwm();
    bool  PickFolder(std::wstring* out, const std::wstring& initial);
    void  RefreshExistingConfig();
    std::wstring DataRootText() const;

    HWND           hwnd_ = nullptr;
    SetupMode      mode_ = SetupMode::Install;
    ID2D1Factory*  d2d_ = nullptr;
    IDWriteFactory* dw_ = nullptr;
    Gfx            gfx_;

    Page          page_ = Page::Configure;
    bool          chinese_ = true;
    std::wstring  exePath_;
    std::wstring  version_;
    std::wstring  installDir_;
    std::wstring  customDir_;
    std::wstring  dataRoot_;
    DataMode      dataMode_ = DataMode::AppData;
    bool          desktopShortcut_ = true;
    bool          autostart_ = false;
    bool          launchAfter_ = true;
    bool          deleteData_ = false;
    bool          preferSlim_ = true;
    uint64_t      payloadBytes_ = 0;
    bool          existingConfig_ = false;

    int           percent_ = 0;
    std::wstring  status_;
    std::wstring  error_;
    bool          busy_ = false;
    std::thread   worker_;

    Hit           hoverId_ = Hit::None;
    Hit           pressedId_ = Hit::None;
    std::vector<Hotspot> hotspots_;
    float         contentHeight_ = 0.0f;
    float         lastHeight_ = -1.0f;
    bool          sized_ = false;
};

}  // namespace fc
