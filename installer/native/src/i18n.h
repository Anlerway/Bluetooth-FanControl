#pragma once
// 安装器/卸载器双语字符串表：一处定义（FC_STRINGS）同时生成枚举与查表，避免对不齐。
#include <string>

namespace fc {

// clang-format off
#define FC_STRINGS(X)                                                                        \
    X(AppName,          L"FanControl",                  L"FanControl")                       \
    X(SetupTitle,       L"FanControl 安装程序",         L"FanControl Setup")                 \
    X(UninstallTitle,   L"FanControl 卸载程序",         L"FanControl Uninstall")             \
    X(Subtitle,         L"蓝牙风扇控制器",               L"Bluetooth fan controller")         \
    X(VersionLabel,     L"版本 %s",                     L"Version %s")                       \
    X(SectionInstall,   L"安装位置",                    L"Install location")                 \
    X(Change,           L"更改…",                       L"Change…")                          \
    X(SectionData,      L"数据存储",                    L"Data storage")                     \
    X(DataHint,         L"配置与日志的保存位置，安装后可以在设置页更改。",                    \
                        L"Where settings and logs live. You can change this later in Settings.") \
    X(DataAppData,      L"应用数据目录",                L"App data folder")                  \
    X(DataAppDataPath,  L"%APPDATA%\\FanControl",       L"%APPDATA%\\FanControl")            \
    X(DataExeDir,       L"安装目录",                    L"Install folder")                   \
    X(DataExeDirPath,   L"程序目录下的 Userdata",       L"Userdata next to the program")     \
    X(DataExeDirHint,   L"便携或 U 盘使用，不写入用户配置文件",                               \
                        L"Portable or USB use — nothing goes to your profile")               \
    X(DataCustom,       L"自定义文件夹",                L"Custom folder")                    \
    X(DataCustomHint,   L"配置与日志写入你选择的目录",   L"Settings and logs go to the folder you pick") \
    X(Browse,           L"浏览…",                       L"Browse…")                          \
    X(SectionOptions,   L"选项",                        L"Options")                          \
    X(OptDesktop,       L"创建桌面快捷方式",            L"Create a desktop shortcut")        \
    X(OptAutostart,     L"开机自动启动",                L"Start with Windows")               \
    X(OptAutostartHint, L"登录后在托盘自动运行（计划任务，需要管理员）",                      \
                        L"Starts in the tray after sign-in (scheduled task, admin)")         \
    X(OptLaunch,        L"安装完成后启动 FanControl",   L"Launch FanControl when finished")  \
    X(InstallAction,    L"安装",                        L"Install")                          \
    X(CancelAction,     L"取消",                        L"Cancel")                           \
    X(UninstallAction,  L"卸载",                        L"Uninstall")                        \
    X(FinishAction,     L"完成",                        L"Finish")                           \
    X(DiskUsage,        L"需要约 %s",                   L"About %s required")                \
    X(RuntimeFull,      L"完整版 · 自带 .NET 运行时",   L"Full · bundled .NET runtime")      \
    X(RuntimeSlim,      L"精简版 · 使用系统 .NET 运行时", L"Slim · uses the installed .NET runtime") \
    X(ExistingConfig,   L"检测到已有配置，将保留",       L"Existing settings found — kept as is") \
    X(ProgressInstall,  L"正在安装",                    L"Installing")                       \
    X(ProgressUninstall,L"正在卸载",                    L"Uninstalling")                     \
    X(DoneTitle,        L"安装完成",                    L"Setup complete")                   \
    X(DoneSubtitle,     L"语言与数据位置已保存，首次启动不再弹出向导。",                      \
                        L"Language and data folder saved — no first-run wizard next time.")  \
    X(UninstallDoneTitle,   L"卸载完成",                L"Uninstall complete")               \
    X(UninstallDoneSubtitle,L"FanControl 已从这台电脑移除。",                                 \
                        L"FanControl has been removed from this computer.")                  \
    X(ConfirmUninstall, L"卸载 FanControl",             L"Uninstall FanControl")             \
    X(ConfirmUninstallHint, L"将关闭正在运行的程序，并移除开机自启、快捷方式与安装文件。",      \
                        L"Running FanControl will be closed; autostart, shortcuts and program files are removed.") \
    X(DeleteData,       L"同时删除配置与日志",          L"Also delete settings and logs")    \
    X(DeleteDataHint,   L"包括数据目录下的全部配置、日志与历史记录",                          \
                        L"Removes every setting, log and history entry in the data folder")  \
    X(DataKept,         L"配置与日志会保留在 %s",       L"Settings and logs stay in %s")     \
    X(SumInstall,       L"安装位置",                    L"Installed to")                     \
    X(SumData,          L"数据位置",                    L"Data folder")                      \
    X(SumLanguage,      L"界面语言",                    L"Language")                         \
    X(StepPreparing,    L"正在准备…",                   L"Preparing…")                       \
    X(StepStopping,     L"正在关闭正在运行的程序…",      L"Closing running instance…")        \
    X(StepExtracting,   L"正在写入文件（%d/%d）",       L"Writing files (%d/%d)")            \
    X(StepConfig,       L"正在写入配置…",               L"Writing settings…")                \
    X(StepShortcuts,    L"正在创建快捷方式…",           L"Creating shortcuts…")              \
    X(StepAutostart,    L"正在设置开机自启…",           L"Updating autostart…")              \
    X(StepRegistry,     L"正在登记卸载信息…",           L"Registering uninstaller…")         \
    X(StepLaunch,       L"正在启动 FanControl…",        L"Launching FanControl…")            \
    X(StepRemoveAuto,   L"正在移除开机自启…",           L"Removing autostart…")              \
    X(StepRemoveLinks,  L"正在删除快捷方式…",           L"Deleting shortcuts…")              \
    X(StepRemoveReg,    L"正在清理注册表…",             L"Cleaning registry…")               \
    X(StepRemoveFiles,  L"正在删除文件…",               L"Deleting files…")                  \
    X(StepRemoveData,   L"正在删除配置与日志…",         L"Deleting settings and logs…")      \
    X(StepFinalize,     L"正在收尾…",                   L"Finishing up…")                    \
    X(ErrElevate,       L"需要管理员权限才能继续，但提权被取消。",                            \
                        L"Administrator rights are required, but elevation was cancelled.")  \
    X(ErrGeneric,       L"操作失败：%s",                L"Operation failed: %s")             \
    X(ErrPayload,       L"安装包损坏：找不到内置文件。", L"This package is damaged: payload missing.") \
    X(ErrExtract,       L"解压文件失败：%s",            L"Failed to extract files: %s")      \
    X(ErrWrite,         L"无法写入 %s",                 L"Cannot write %s")                  \
    X(ErrRegistry,      L"写入注册表失败。",            L"Failed to update the registry.")   \
    X(ErrShortcut,      L"创建快捷方式失败。",          L"Failed to create the shortcut.")   \
    X(ErrAutostart,     L"设置开机自启失败。",          L"Failed to update autostart.")      \
    X(ErrCloseApp,      L"FanControl 正在运行，关闭失败后无法继续。",                         \
                        L"FanControl is running and could not be closed.")                   \
    X(LangChinese,      L"中文",                        L"中文")                             \
    X(LangEnglish,      L"English",                     L"English")                          \
    X(Language,         L"语言",                        L"Language")
// clang-format on

enum class Str {
#define FC_ENUM(id, zh, en) id,
    FC_STRINGS(FC_ENUM)
#undef FC_ENUM
        Count
};

// 切换界面语言；chinese=true 用中文，false 用英文。
void SetUiLanguage(bool chinese);
bool IsChineseUi();
// 取字符串（未设置时默认中文）。参数化字符串请配合 fc::Format 使用。
const wchar_t* T(Str id);

}  // namespace fc
