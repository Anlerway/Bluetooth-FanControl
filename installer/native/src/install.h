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
    bool         autostart = false;
    bool         launchAfter = true;
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
void ForgetInstallInfo();

// 默认安装目录：%ProgramFiles%\FanControl
std::wstring DefaultInstallDir();
// 解析数据根：AppData -> %APPDATA%\FanControl；ExeDirectory -> <installDir>\Userdata；Custom -> customDir
std::wstring ResolveDataRoot(DataMode mode, const std::wstring& installDir, const std::wstring& customDir);
// 是否已存在 system.json（首启判定），用于「检测到已有配置」提示
bool HasExistingConfig(const std::wstring& installDir, const std::wstring& dataRoot);
// 系统是否装了 .NET 8 桌面运行时（决定用精简版还是完整版负载）
bool HasDesktopRuntime8();
// 调用方（UI）预览时使用：负载里精简版是否存在，以及需要多少磁盘
bool PayloadAvailable(const std::wstring& exePath);

}  // namespace fc
