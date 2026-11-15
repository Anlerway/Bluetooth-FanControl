#pragma once
// 安装/卸载引擎：与 UI 完全解耦，进度通过回调上报，错误以可读文本返回。
#include <string>

namespace fc {

// 数据存储位置，与主程序 ConfigLocation 枚举一致。
enum class DataMode : int { ExeDirectory = 0, AppData = 1, Custom = 2 };

struct InstallRequest {
    std::wstring exePath;        // 安装包自身路径（负载从这里读）
    std::wstring installDir;     // 目标目录（绝对路径）
    DataMode     dataMode = DataMode::AppData;
    std::wstring customDir;      // dataMode == Custom 时有效
    bool         desktopShortcut = true;
    bool         startMenuShortcut = true;
    bool         autostart = false;
    bool         chinese = true;
    bool         preferSlim = true;   // 有精简版负载时优先用
};

struct UninstallRequest {
    std::wstring exePath;        // 卸载器自身（installDir\uninstall.exe）
    std::wstring installDir;
    std::wstring dataRoot;       // 已安装记录的数据根（可能为空）
    bool         deleteUserData = false;
    bool         chinese = true;
};

// percent: 0..100；status: 已本地化的状态文本
using ProgressFn = void (*)(int percent, const wchar_t* status, void* user);

// 返回空字符串表示成功，否则为本地化错误文本。
std::wstring RunInstall(const InstallRequest& req, ProgressFn progress, void* user);
std::wstring RunUninstall(const UninstallRequest& req, ProgressFn progress, void* user);

struct InstallInfo {
    bool         installed = false;
    std::wstring installDir;
    std::wstring version;
    std::wstring language;    // zh-CN / en-US
    DataMode     dataMode = DataMode::AppData;
    std::wstring dataRoot;
};

// 读取 HKLM\SOFTWARE\FanControl 元数据
bool ReadInstallInfo(InstallInfo* info);
// 计划任务 FanControl 是否已存在（升级时恢复「开机自动启动」开关用）
bool AutostartEnabled();
void ForgetInstallInfo();

// 默认安装目录：%ProgramFiles%\FanControl
std::wstring DefaultInstallDir();
// 解析数据根：AppData -> %APPDATA%\FanControl；ExeDirectory -> <installDir>\Userdata；Custom -> customDir
std::wstring ResolveDataRoot(DataMode mode, const std::wstring& installDir, const std::wstring& customDir);
// 是否已存在 system.json（首启判定），用于「检测到已有配置」提示
bool HasExistingConfig(const std::wstring& installDir, const std::wstring& dataRoot);
// 系统是否装了 .NET 8 运行时（只需要 Microsoft.NETCore.App，不需要 Windows Desktop Runtime；
// 决定用精简版还是完整版负载）
bool HasDotnetRuntime8();
// 调用方（UI）预览时使用：负载里精简版是否存在，以及需要多少磁盘
bool PayloadAvailable(const std::wstring& exePath);

// 卸载收尾：把卸载器自己复制到 %TEMP%，用 --cleanup 启动副本；副本负责删掉原 uninstall.exe、
// 删掉已经变空的安装目录，最后把副本自己登记成重启时删除。成功返回 true（调用方应立刻退出）。
bool StartSelfCleanup(const std::wstring& installDir, const std::wstring& selfPath);
// 副本进程入口：main.cpp 见到 --cleanup <installDir> <selfPath> <parentPid> 时调用。
int RunCleanupWorker(const std::wstring& installDir, const std::wstring& selfPath, unsigned long parentPid);

}  // namespace fc
