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
    // 整体界面缩放：布局按 700x578 的设计坐标写，窗口与绘制按这个系数一起缩小。
    static constexpr float kUiScale = 0.86f;

    bool    Create(HWND hwnd, SetupMode mode);
    void    Destroy();
    bool    Paint();
    LRESULT HandleMessage(UINT msg, WPARAM wp, LPARAM lp);

private:
    enum class Page { Configure, Data, Progress, Done, Confirm, UninstallProgress, UninstallDone };
    enum class Hit {
        None,
        LangZh,
        LangEn,
        PickSlim,
        PickFull,
        PathField,
        BrowseInstall,
        DataAppData,
        DataExeDir,
        DataCustom,
        BrowseCustom,
        OptDesktop,
        OptStartMenu,
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
    float PaintHero(float top);   // 居中版页首：图标 + 名称 + 版本
    float PaintLocation();
    float PaintDataPage();
    void  PaintPathField(float x, float y, float w, float h);
    void  FocusPath(bool on);
    void  InsertText(const std::wstring& text);
    void  CommitPath();
    bool  PathValid() const;
    // 输入框光标与选区
    bool   HasPathSelection() const { return selAnchor_ != caret_; }
    size_t PathSelBegin() const { return selAnchor_ < caret_ ? selAnchor_ : caret_; }
    size_t PathSelEnd() const { return selAnchor_ < caret_ ? caret_ : selAnchor_; }
    void   SetCaretPos(size_t pos, bool extend);
    void   SelectAllPath();
    bool   DeletePathSelection();
    size_t PathIndexAtX(float relX);
    std::wstring SelectedPathText() const;
    float PaintProgress(const wchar_t* title);
    float PaintDone(const wchar_t* title, const wchar_t* subtitle, bool uninstall);
    float PaintConfirm();
    void  PaintFooter(float y, const wchar_t* primary, Hit primaryHit, const wchar_t* secondary,
                      bool centered = false, bool danger = false);
    void  Card(float x, float y, float w, float h);
    void  LanguageSwitch(float x, float y);
    void  ToggleChip(float x, float y, bool on);
    void  RadioRow(float x, float y, float w, bool selected, const wchar_t* title, const wchar_t* hint, Hit id);
    void  Checkbox(float x, float y, const wchar_t* label, bool on, Hit id);
    void  TextButton(const D2D1_RECT_F& r, const wchar_t* label, Hit id, bool primary, bool danger = false);
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
    // 输入框里就是最终安装目录（浏览选择时已补上 \FanControl）。
    std::wstring InstallTargetDir() const;

    HWND           hwnd_ = nullptr;
    SetupMode      mode_ = SetupMode::Install;
    ID2D1Factory*  d2d_ = nullptr;
    IDWriteFactory* dw_ = nullptr;
    Gfx            gfx_;
    HICON          appIcon_ = nullptr;   // 应用图标（setup.rc 的 101）

    Page          page_ = Page::Configure;
    bool          chinese_ = true;
    std::wstring  exePath_;
    std::wstring  version_;
    std::wstring  installDir_;
    std::wstring  customDir_;
    std::wstring  dataRoot_;
    DataMode      dataMode_ = DataMode::AppData;
    bool          desktopShortcut_ = true;
    bool          startMenuShortcut_ = true;
    bool          autostart_ = false;
    bool          launchAfter_ = true;
    bool          deleteData_ = false;
    bool          preferSlim_ = true;
    uint64_t      bytesSlim_ = 0;
    uint64_t      bytesFull_ = 0;
    bool          runtimeAvailable_ = false;
    bool          existingConfig_ = false;
    bool          upgrade_ = false;               // 注册表里已有安装记录：走升级流程
    std::wstring  installedVersion_;
    bool          pathFocused_ = false;
    size_t        caret_ = 0;
    size_t        selAnchor_ = 0;      // 选区另一端；与 caret_ 相同表示没有选区
    bool          selecting_ = false;  // 鼠标拖选进行中
    bool          caretOn_ = true;
    D2D1_RECT_F   pathFieldRect_{};    // 输入框上一帧的位置，用来把鼠标坐标换算成文字索引
    float         pathScroll_ = 0.0f;  // 上一帧文本的水平滚动量

    int           percent_ = 0;
    std::wstring  status_;
    std::wstring  error_;
    bool          busy_ = false;
    std::thread   worker_;

    Hit           hoverId_ = Hit::None;
    Hit           pressedId_ = Hit::None;
    std::vector<Hotspot> hotspots_;
    float         contentHeight_ = 0.0f;
    float         dataPageHeight_ = 0.0f;   // 第二页实测高度，首页与它等高
    float         lastHeight_ = -1.0f;
    bool          sized_ = false;
};

}  // namespace fc
