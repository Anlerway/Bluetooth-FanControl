#include "install.h"

#include <windows.h>
#include <winver.h>

#include <cstdint>
#include <string>
#include <vector>

#include "i18n.h"
#include "payload.h"
#include "sysutil.h"

// 版本资源 API（version.lib）与注册表写值（advapi32）由本文件直接引用，
// 用 pragma 自动链接，构建脚本不必额外维护库列表。
#pragma comment(lib, "version.lib")
#pragma comment(lib, "advapi32.lib")

namespace fc {
namespace {

// ---------------- 产品常量 ----------------
constexpr wchar_t kProductName[]    = L"FanControl";
constexpr wchar_t kAppExe[]         = L"FanControl.exe";
constexpr wchar_t kUninstallExe[]   = L"uninstall.exe";
constexpr wchar_t kShortcutName[]   = L"FanControl.lnk";
constexpr wchar_t kTaskName[]       = L"FanControl";
constexpr wchar_t kFallbackVersion[] = L"1.4.0";

constexpr wchar_t kMetaKey[]      = L"SOFTWARE\\FanControl";
constexpr wchar_t kUninstallKey[] = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\FanControl";
constexpr wchar_t kRunKey[]       = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Run";
// Inno Setup 旧版的卸载信息键（HKLM / HKCU 都可能有）
constexpr wchar_t kInnoKey[] =
    L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\{8F2B6C41-7E5A-4D2F-9B31-2A8C6E1D5F90}_is1";

constexpr wchar_t kUserdataDir[]    = L"Userdata";
constexpr wchar_t kConfigDir[]      = L"Config";
constexpr wchar_t kSystemJson[]     = L"system.json";
constexpr wchar_t kAppConfigJson[]  = L"appconfig.json";
constexpr wchar_t kLocationTxt[]    = L"location.txt";

constexpr int   kKillTimeoutMs = 10000;
constexpr DWORD kTaskTimeoutMs = 20000;

// 进度区间：解压 5..70，删文件 26..80（与规格书一致）
constexpr int kExtractFrom = 5;
constexpr int kExtractTo   = 70;
constexpr int kRemoveFrom  = 26;
constexpr int kRemoveTo    = 80;

// ---------------- 进度上报 ----------------
struct Progress {
    ProgressFn fn   = nullptr;
    void*      user = nullptr;

    void operator()(int percent, const wchar_t* status) const {
        if (fn != nullptr) fn(percent, status != nullptr ? status : L"", user);
    }
};

// ---------------- 字符串/路径小工具 ----------------
bool SameText(const std::wstring& a, const std::wstring& b) {
    if (a.empty() || b.empty()) return false;
    return CompareStringOrdinal(a.c_str(), static_cast<int>(a.size()), b.c_str(), static_cast<int>(b.size()), TRUE) ==
           CSTR_EQUAL;
}

std::wstring TrimSeparator(std::wstring path) {
    while (!path.empty() && (path.back() == L'\\' || path.back() == L'/')) path.pop_back();
    return path;
}

bool PathEquals(const std::wstring& a, const std::wstring& b) {
    if (a.empty() || b.empty()) return false;
    return SameText(TrimSeparator(a), TrimSeparator(b));
}

bool IsAbsolutePath(const std::wstring& path) {
    if (path.size() >= 3 && path[1] == L':' && (path[2] == L'\\' || path[2] == L'/')) {
        const wchar_t drive = path[0];
        const bool letter = (drive >= L'A' && drive <= L'Z') || (drive >= L'a' && drive <= L'z');
        if (letter) return true;
    }
    // UNC：\\server\share
    return path.size() >= 2 && path[0] == L'\\' && path[1] == L'\\';
}

// %APPDATA%\FanControl —— 唯一无条件可写的根（location.txt 也放在这里）
std::wstring AppDataRoot() {
    std::wstring base = GetAppDataRoaming();
    if (base.empty()) base = ExpandEnv(L"%APPDATA%");
    if (base.empty()) base = ExpandEnv(L"%USERPROFILE%\\AppData\\Roaming");
    base = NormalizeDir(base);
    if (base.empty()) return std::wstring();
    return Join(base, kProductName);
}

std::wstring StartMenuDir() {
    std::wstring dir = NormalizeDir(GetProgramsMenuDir());
    if (dir.empty()) dir = NormalizeDir(ExpandEnv(L"%ProgramData%\\Microsoft\\Windows\\Start Menu\\Programs"));
    return dir;
}

const wchar_t* DataLocationName(DataMode mode) {
    switch (mode) {
        case DataMode::ExeDirectory:
            return L"ExeDirectory";
        case DataMode::Custom:
            return L"Custom";
        case DataMode::AppData:
        default:
            return L"AppData";
    }
}

DataMode DataModeFromName(const std::wstring& name) {
    if (SameText(name, L"ExeDirectory")) return DataMode::ExeDirectory;
    if (SameText(name, L"Custom")) return DataMode::Custom;
    return DataMode::AppData;
}

// ---------------- 版本号 ----------------
// 读 <installDir>\FanControl.exe 的 VS_FIXEDFILEINFO，输出 "a.b.c"；读不到返回空串。
std::wstring ReadProductVersion(const std::wstring& exePath) {
    DWORD ignored = 0;
    const DWORD size = GetFileVersionInfoSizeW(exePath.c_str(), &ignored);
    if (size == 0) return std::wstring();

    std::vector<uint8_t> block(size);
    if (!GetFileVersionInfoW(exePath.c_str(), 0, size, block.data())) return std::wstring();

    VS_FIXEDFILEINFO* fixed = nullptr;
    UINT length = 0;
    if (!VerQueryValueW(block.data(), L"\\", reinterpret_cast<void**>(&fixed), &length)) return std::wstring();
    if (fixed == nullptr || length < sizeof(VS_FIXEDFILEINFO)) return std::wstring();
    if (fixed->dwSignature != 0xFEEF04BDu) return std::wstring();

    const unsigned major = static_cast<unsigned>(HIWORD(fixed->dwFileVersionMS));
    const unsigned minor = static_cast<unsigned>(LOWORD(fixed->dwFileVersionMS));
    const unsigned build = static_cast<unsigned>(HIWORD(fixed->dwFileVersionLS));
    return Format(L"%u.%u.%u", major, minor, build);
}

// ---------------- JSON 配置 ----------------
std::string JsonEscape(const std::string& text) {
    static const char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(text.size() + 8);
    for (const char ch : text) {
        switch (ch) {
            case '\\':
                out += "\\\\";
                break;
            case '"':
                out += "\\\"";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default: {
                const unsigned char byte = static_cast<unsigned char>(ch);
                if (byte < 0x20) {
                    out += "\\u00";
                    out.push_back(kHex[(byte >> 4) & 0xF]);
                    out.push_back(kHex[byte & 0xF]);
                } else {
                    out.push_back(ch);
                }
                break;
            }
        }
    }
    return out;
}

// 与主程序 ConfigManager 的 SystemConfig 对齐：camelCase、枚举写字符串（无 BOM、\n 行尾）。
std::string BuildSystemJson(DataMode mode, const std::wstring& customDir) {
    std::string json = "{\n  \"userDataLocation\": \"";
    json += WideToUtf8(DataLocationName(mode));
    json += "\",\n";
    if (mode == DataMode::Custom && !customDir.empty()) {
        json += "  \"userDataPath\": \"";
        json += JsonEscape(WideToUtf8(customDir));
        json += "\",\n";
    }
    json += "  \"logEnabled\": true,\n";
    json += "  \"maxLogFiles\": 20\n";
    json += "}\n";
    return json;
}

std::string BuildAppConfigJson(bool chinese) {
    std::string json = "{\n  \"language\": \"";
    json += chinese ? "zh-CN" : "en-US";
    json += "\"\n}\n";
    return json;
}

// 写 system.json / appconfig.json 与 location.txt 指针。返回空串表示成功。
std::wstring WriteInstallerConfig(DataMode mode, const std::wstring& dataRoot, const std::wstring& customDir,
                                  bool chinese) {
    if (dataRoot.empty()) return std::wstring(T(Str::ErrWrite));

    const std::wstring configDir  = Join(dataRoot, kConfigDir);
    const std::wstring systemPath = Join(configDir, kSystemJson);
    const std::wstring appPath    = Join(configDir, kAppConfigJson);

    // 升级安装：已有的 system.json 保留用户设置，绝不覆盖
    if (!FileExists(systemPath)) {
        if (!EnsureDirForFile(systemPath)) return Format(T(Str::ErrWrite), systemPath.c_str());
        if (!WriteAllText(systemPath, BuildSystemJson(mode, customDir))) {
            return Format(T(Str::ErrWrite), systemPath.c_str());
        }
    }

    // appconfig.json 只在缺失时写，避免改掉用户选的语言
    if (!FileExists(appPath)) {
        if (!EnsureDirForFile(appPath)) return Format(T(Str::ErrWrite), appPath.c_str());
        if (!WriteAllText(appPath, BuildAppConfigJson(chinese))) return Format(T(Str::ErrWrite), appPath.c_str());
    }

    // 自定义数据根的指路文件：Custom 写绝对路径（无 BOM、无换行），其余模式删掉
    const std::wstring appData = AppDataRoot();
    if (appData.empty()) return std::wstring();
    const std::wstring pointer = Join(appData, kLocationTxt);
    if (mode == DataMode::Custom) {
        if (!EnsureDirForFile(pointer)) return Format(T(Str::ErrWrite), pointer.c_str());
        if (!WriteAllText(pointer, WideToUtf8(customDir))) return Format(T(Str::ErrWrite), pointer.c_str());
    } else {
        DeleteFileIfExists(pointer);
    }
    return std::wstring();
}

// ---------------- 快捷方式 ----------------
bool CreateInstallerShortcuts(const std::wstring& installDir, bool desktop) {
    const std::wstring target = Join(installDir, kAppExe);
    const std::wstring args;                 // 无启动参数
    bool ok = CreateShortcut(Join(StartMenuDir(), kShortcutName), target, args, installDir, kProductName);

    if (desktop) {
        const std::wstring common = NormalizeDir(GetCommonDesktopDir());
        if (!common.empty()) {
            ok = CreateShortcut(Join(common, kShortcutName), target, args, installDir, kProductName) && ok;
        }
    }
    return ok;
}

void RemoveInstallerShortcuts() {
    const std::wstring common = NormalizeDir(GetCommonDesktopDir());
    if (!common.empty()) DeleteFileIfExists(Join(common, kShortcutName));
    const std::wstring desktop = NormalizeDir(GetDesktopDir());
    if (!desktop.empty()) DeleteFileIfExists(Join(desktop, kShortcutName));
    const std::wstring programs = StartMenuDir();
    if (!programs.empty()) DeleteFileIfExists(Join(programs, kShortcutName));
}

// ---------------- 注册表 ----------------
// 删单个值（sysutil 只提供整键删除）；键或值不存在都算成功。
bool DeleteRegValue(HKEY root, const std::wstring& subKey, const std::wstring& name) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(root, subKey.c_str(), 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS) return false;
    const LSTATUS status = RegDeleteValueW(key, name.c_str());
    RegCloseKey(key);
    return status == ERROR_SUCCESS || status == ERROR_FILE_NOT_FOUND;
}

DWORD EstimateSizeKb(uint64_t rawBytes) {
    const uint64_t kb = rawBytes / 1024;
    return kb > 0xFFFFFFFFull ? 0xFFFFFFFFu : static_cast<DWORD>(kb);
}

bool WriteInstallerRegistry(const std::wstring& installDir, const std::wstring& dataRoot, DataMode mode, bool chinese,
                            const PayloadSegment& segment) {
    const std::wstring exePath     = Join(installDir, kAppExe);
    const std::wstring uninstaller = Join(installDir, kUninstallExe);

    std::wstring version = ReadProductVersion(exePath);
    if (version.empty()) version = kFallbackVersion;

    // 元数据：安装目录 + 数据根 + 语言 + 版本（卸载器/主程序据此定位）
    bool ok = true;
    ok = RegSetString(HKEY_LOCAL_MACHINE, kMetaKey, L"InstallDir", installDir) && ok;
    ok = RegSetString(HKEY_LOCAL_MACHINE, kMetaKey, L"DataRoot", dataRoot) && ok;
    ok = RegSetString(HKEY_LOCAL_MACHINE, kMetaKey, L"DataLocation", DataLocationName(mode)) && ok;
    ok = RegSetString(HKEY_LOCAL_MACHINE, kMetaKey, L"Language", chinese ? L"zh-CN" : L"en-US") && ok;
    ok = RegSetString(HKEY_LOCAL_MACHINE, kMetaKey, L"Version", version) && ok;

    // 控制面板「应用和功能」条目
    ok = RegSetString(HKEY_LOCAL_MACHINE, kUninstallKey, L"DisplayName", kProductName) && ok;
    ok = RegSetString(HKEY_LOCAL_MACHINE, kUninstallKey, L"DisplayVersion", version) && ok;
    ok = RegSetString(HKEY_LOCAL_MACHINE, kUninstallKey, L"Publisher", kProductName) && ok;
    ok = RegSetString(HKEY_LOCAL_MACHINE, kUninstallKey, L"DisplayIcon", exePath) && ok;
    ok = RegSetString(HKEY_LOCAL_MACHINE, kUninstallKey, L"InstallLocation", installDir) && ok;
    ok = RegSetString(HKEY_LOCAL_MACHINE, kUninstallKey, L"UninstallString", L"\"" + uninstaller + L"\"") && ok;
    ok = RegSetDword(HKEY_LOCAL_MACHINE, kUninstallKey, L"NoModify", 1) && ok;
    ok = RegSetDword(HKEY_LOCAL_MACHINE, kUninstallKey, L"NoRepair", 1) && ok;
    ok = RegSetDword(HKEY_LOCAL_MACHINE, kUninstallKey, L"EstimatedSize", EstimateSizeKb(segment.rawSize)) && ok;
    return ok;
}

// 旧版 Inno Setup 残留：卸载信息键（两个根）+ unins000.*
void CleanLegacyInstall(const std::wstring& installDir) {
    RegDeleteTree(HKEY_LOCAL_MACHINE, kInnoKey);
    RegDeleteTree(HKEY_CURRENT_USER, kInnoKey);
    if (installDir.empty()) return;
    DeleteFileIfExists(Join(installDir, L"unins000.exe"));
    DeleteFileIfExists(Join(installDir, L"unins000.dat"));
}

// ---------------- 计划任务 ----------------
bool CreateAutostartTask(const std::wstring& installDir) {
    const std::wstring exe = Join(installDir, kAppExe);
    // /TR 的内层引号用 \" 转义：任务动作里要保留 "exe" --autostart 的原始形态
    std::wstring cmd = L"schtasks.exe /Create /TN \"";
    cmd += kTaskName;
    cmd += L"\" /TR \"\\\"";
    cmd += exe;
    cmd += L"\\\" --autostart\" /SC ONLOGON /RL HIGHEST /F";

    DWORD exitCode = 1;
    if (!RunHidden(cmd, kTaskTimeoutMs, &exitCode)) return false;
    return exitCode == 0;
}

void DeleteAutostartTask() {
    std::wstring cmd = L"schtasks.exe /Delete /TN \"";
    cmd += kTaskName;
    cmd += L"\" /F";
    DWORD exitCode = 1;
    RunHidden(cmd, kTaskTimeoutMs, &exitCode);   // 失败忽略：任务本来就不存在
}

// ---------------- 删除 ----------------
void DeletePath(const std::wstring& path) {
    if (path.empty()) return;
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) return;
    if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        DeleteTree(path);
        return;
    }
    if ((attributes & FILE_ATTRIBUTE_READONLY) != 0) {
        SetFileAttributesW(path.c_str(), attributes & ~static_cast<DWORD>(FILE_ATTRIBUTE_READONLY));
    }
    DeleteFileIfExists(path);
}

// 递归删掉已经空掉/空着的子目录（DeleteTree 已尽力，这里补刀）
void RemoveEmptyDirs(const std::wstring& dir) {
    std::vector<std::wstring> subdirs;
    WIN32_FIND_DATAW data{};
    HANDLE find = FindFirstFileW(Join(dir, L"*").c_str(), &data);
    if (find == INVALID_HANDLE_VALUE) return;
    do {
        if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) continue;
        const std::wstring name = data.cFileName;
        if (name == L"." || name == L"..") continue;
        subdirs.push_back(Join(dir, name));
    } while (FindNextFileW(find, &data) != FALSE);
    FindClose(find);

    for (const std::wstring& sub : subdirs) {
        RemoveEmptyDirs(sub);
        RemoveDirectoryW(sub.c_str());
    }
}

bool IsSelfFile(const std::wstring& path, const std::wstring& selfPath, const std::wstring& installDir) {
    if (PathEquals(path, selfPath)) return true;
    if (PathEquals(path, GetModulePath())) return true;
    return PathEquals(path, Join(installDir, kUninstallExe));
}

// 删 installDir 下除卸载器自身外的全部内容（顶层逐项，进度 26..80）
void RemoveInstalledFiles(const std::wstring& installDir, const std::wstring& selfPath, const Progress& report) {
    if (!DirExists(installDir)) return;

    std::vector<std::wstring> entries;
    WIN32_FIND_DATAW data{};
    HANDLE find = FindFirstFileW(Join(installDir, L"*").c_str(), &data);
    if (find != INVALID_HANDLE_VALUE) {
        do {
            const std::wstring name = data.cFileName;
            if (name == L"." || name == L"..") continue;
            entries.push_back(Join(installDir, name));
        } while (FindNextFileW(find, &data) != FALSE);
        FindClose(find);
    }

    const size_t total = entries.size();
    for (size_t i = 0; i < total; ++i) {
        int percent = kRemoveTo;
        if (total > 0) {
            const uint64_t span = static_cast<uint64_t>(kRemoveTo - kRemoveFrom);
            percent = kRemoveFrom + static_cast<int>((static_cast<uint64_t>(i) * span) / total);
        }
        if (percent > kRemoveTo) percent = kRemoveTo;
        report(percent, T(Str::StepRemoveFiles));
        if (IsSelfFile(entries[i], selfPath, installDir)) continue;
        DeletePath(entries[i]);
    }
    RemoveEmptyDirs(installDir);
}

// 删除根目录前的保险：必须是绝对路径，且不是盘根/用户目录/系统目录本身
bool IsSafeDeleteRoot(const std::wstring& path) {
    const std::wstring dir = NormalizeDir(path);
    if (!IsAbsolutePath(dir)) return false;
    if (dir.size() <= 3) return false;   // "C:\" 之类的盘根

    const std::wstring guarded[] = {
        NormalizeDir(ExpandEnv(L"%USERPROFILE%")),
        NormalizeDir(ExpandEnv(L"%APPDATA%")),
        NormalizeDir(ExpandEnv(L"%SystemRoot%")),
        NormalizeDir(GetProgramFiles()),
        NormalizeDir(GetProgramFilesX86()),
    };
    for (const std::wstring& item : guarded) {
        if (!item.empty() && PathEquals(dir, item)) return false;
    }
    return true;
}

// 用户数据清理：优先用记录下来的数据根；没有记录时只清能确定属于本程序的目录
void RemoveUserData(const std::wstring& dataRoot, const std::wstring& installDir) {
    const std::wstring appData = AppDataRoot();

    const std::wstring root = NormalizeDir(dataRoot);
    if (!root.empty() && IsSafeDeleteRoot(root)) {
        DeleteTree(root);
    } else {
        // 没有可信记录：跳过 %APPDATA%\FanControl 之外的猜测（自定义目录可能是任意位置）
        if (!appData.empty()) DeleteTree(appData);
        const std::wstring userdata = Join(installDir, kUserdataDir);
        if (DirExists(userdata)) DeleteTree(userdata);
    }

    if (!appData.empty()) DeleteFileIfExists(Join(appData, kLocationTxt));
}

// ---------------- 自删除 ----------------
std::wstring CmdExePath() {
    const std::wstring comspec = ExpandEnv(L"%ComSpec%");
    if (!comspec.empty() && FileExists(comspec)) return comspec;

    wchar_t buffer[MAX_PATH] = {};
    const UINT length = GetSystemDirectoryW(buffer, MAX_PATH);
    if (length > 0 && length < MAX_PATH) return Join(buffer, L"cmd.exe");
    return std::wstring(L"cmd.exe");
}

std::wstring SelfDeleteCommandLine(const std::wstring& installDir, const std::wstring& selfPath) {
    std::wstring cmd = CmdExePath();
    cmd += L" /c ping -n 3 127.0.0.1 >nul & del /f /q \"";
    cmd += selfPath;
    cmd += L"\"";
    const std::wstring dir = TrimSeparator(installDir);
    if (!dir.empty()) {
        cmd += L" & rmdir /s /q \"";
        cmd += dir;
        cmd += L"\"";
    }
    return cmd;
}

// 不等待子进程：卸载器自己马上就要退出，删除动作随后由 cmd 完成
bool SpawnDetached(const std::wstring& cmdLine) {
    if (cmdLine.empty()) return false;

    std::vector<wchar_t> buffer(cmdLine.begin(), cmdLine.end());
    buffer.push_back(L'\0');

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION info{};
    const BOOL created = CreateProcessW(nullptr, buffer.data(), nullptr, nullptr, FALSE,
                                        CREATE_NO_WINDOW | DETACHED_PROCESS, nullptr, nullptr, &startup, &info);
    if (!created) return false;
    CloseHandle(info.hThread);
    CloseHandle(info.hProcess);
    return true;
}

// ---------------- 多版本运行时 ----------------
bool HasDesktopRuntime8In(const std::wstring& programFiles) {
    if (programFiles.empty()) return false;
    const std::wstring root = Join(Join(Join(programFiles, L"dotnet"), L"shared"), L"Microsoft.WindowsDesktop.App");
    WIN32_FIND_DATAW data{};
    HANDLE find = FindFirstFileW(Join(root, L"8.*").c_str(), &data);
    if (find == INVALID_HANDLE_VALUE) return false;
    bool found = false;
    do {
        if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            found = true;
            break;
        }
    } while (FindNextFileW(find, &data) != FALSE);
    FindClose(find);
    return found;
}

}  // namespace

// ---------------- 公共接口 ----------------
std::wstring DefaultInstallDir() {
    std::wstring programFiles = NormalizeDir(GetProgramFiles());
    if (programFiles.empty()) programFiles = L"C:\\Program Files";
    return Join(programFiles, kProductName);
}

std::wstring ResolveDataRoot(DataMode mode, const std::wstring& installDir, const std::wstring& customDir) {
    switch (mode) {
        case DataMode::ExeDirectory: {
            const std::wstring dir = NormalizeDir(installDir);
            return dir.empty() ? AppDataRoot() : Join(dir, kUserdataDir);
        }
        case DataMode::Custom: {
            const std::wstring custom = NormalizeDir(customDir);
            // 自定义路径为空或不是绝对路径：与主程序一致，退回 AppData
            if (IsAbsolutePath(custom)) return custom;
            return AppDataRoot();
        }
        case DataMode::AppData:
        default:
            return AppDataRoot();
    }
}

bool HasExistingConfig(const std::wstring& installDir, const std::wstring& dataRoot) {
    std::vector<std::wstring> candidates;

    const std::wstring root = NormalizeDir(dataRoot);
    if (!root.empty()) candidates.push_back(Join(Join(root, kConfigDir), kSystemJson));

    const std::wstring appData = AppDataRoot();
    if (!appData.empty()) candidates.push_back(Join(Join(appData, kConfigDir), kSystemJson));

    const std::wstring dir = NormalizeDir(installDir);
    if (!dir.empty()) candidates.push_back(Join(Join(Join(dir, kUserdataDir), kConfigDir), kSystemJson));

    for (const std::wstring& path : candidates) {
        if (FileExists(path)) return true;
    }
    return false;
}

bool HasDesktopRuntime8() {
    return HasDesktopRuntime8In(GetProgramFiles()) || HasDesktopRuntime8In(GetProgramFilesX86());
}

bool PayloadAvailable(const std::wstring& exePath) {
    PayloadSet payloads;
    std::string error;
    if (!PayloadSet::Load(exePath, &payloads, &error)) return false;
    return payloads.seg[0].present || payloads.seg[1].present;
}

bool ReadInstallInfo(InstallInfo* info) {
    if (info == nullptr) return false;
    *info = InstallInfo{};

    std::wstring installDir;
    if (!RegGetString(HKEY_LOCAL_MACHINE, kMetaKey, L"InstallDir", &installDir) || installDir.empty()) return false;

    info->installed  = true;
    info->installDir = installDir;
    RegGetString(HKEY_LOCAL_MACHINE, kMetaKey, L"Version", &info->version);
    RegGetString(HKEY_LOCAL_MACHINE, kMetaKey, L"Language", &info->language);
    RegGetString(HKEY_LOCAL_MACHINE, kMetaKey, L"DataRoot", &info->dataRoot);

    std::wstring location;
    if (RegGetString(HKEY_LOCAL_MACHINE, kMetaKey, L"DataLocation", &location)) {
        info->dataMode = DataModeFromName(location);
    }
    return true;
}

void ForgetInstallInfo() {
    RegDeleteTree(HKEY_LOCAL_MACHINE, kMetaKey);
    RegDeleteTree(HKEY_LOCAL_MACHINE, kUninstallKey);
}

std::wstring RunInstall(const InstallRequest& req, ProgressFn progress, void* user) {
    SetUiLanguage(req.chinese);
    const Progress report{progress, user};

    std::wstring installDir = NormalizeDir(req.installDir);
    if (installDir.empty()) installDir = DefaultInstallDir();
    const std::wstring customDir =
        req.dataMode == DataMode::Custom ? NormalizeDir(req.customDir) : std::wstring();
    const std::wstring dataRoot = ResolveDataRoot(req.dataMode, installDir, customDir);
    // 首启判定要在写配置之前取（升级安装不该再弹向导，也不该自动启动）
    const bool hadConfig = HasExistingConfig(installDir, dataRoot);

    const std::wstring packagePath = req.exePath.empty() ? GetModulePath() : NormalizeDir(req.exePath);

    // 1) 定位负载
    report(0, T(Str::StepPreparing));
    PayloadSet payloads;
    std::string loadError;
    if (!PayloadSet::Load(packagePath, &payloads, &loadError)) return std::wstring(T(Str::ErrPayload));
    const PayloadSegment* segment = payloads.Select(req.preferSlim);
    if (segment == nullptr || !segment->present) return std::wstring(T(Str::ErrPayload));

    // 2) 关闭正在运行的实例
    report(3, T(Str::StepStopping));
    KillProcessByExeName(kAppExe, kKillTimeoutMs);
    if (FindProcessId(kAppExe) != 0) return std::wstring(T(Str::ErrCloseApp));

    // 3) 安装目录
    report(5, T(Str::StepPreparing));
    if (!EnsureDir(installDir)) return Format(T(Str::ErrWrite), installDir.c_str());

    // 4) 解压落盘（5..70）
    ExtractCallbacks callbacks;
    callbacks.onFile = [&report](const std::wstring&, int index, int count, uint64_t doneBytes,
                                 uint64_t totalBytes) {
        int percent = kExtractFrom;
        if (totalBytes != 0) {
            const uint64_t span = static_cast<uint64_t>(kExtractTo - kExtractFrom);
            percent = kExtractFrom + static_cast<int>((doneBytes * span) / totalBytes);
        }
        if (percent > kExtractTo) percent = kExtractTo;
        const std::wstring status = Format(T(Str::StepExtracting), index, count);
        report(percent, status.c_str());
        return true;   // 不让解压中断；失败交给 ExtractPayload 的返回值
    };

    std::string extractError;
    if (!ExtractPayload(packagePath, *segment, installDir, callbacks, &extractError, nullptr)) {
        const std::wstring detail = Utf8ToWide(extractError);
        return Format(T(Str::ErrExtract), detail.c_str());
    }

    // 5) 配置（与主程序 ConfigManager 同布局）
    report(72, T(Str::StepConfig));
    const std::wstring configError = WriteInstallerConfig(req.dataMode, dataRoot, customDir, req.chinese);
    if (!configError.empty()) return configError;

    // 6) 快捷方式
    report(80, T(Str::StepShortcuts));
    if (!CreateInstallerShortcuts(installDir, req.desktopShortcut)) return std::wstring(T(Str::ErrShortcut));

    // 7) 开机自启
    report(86, T(Str::StepAutostart));
    if (req.autostart) {
        if (!CreateAutostartTask(installDir)) return std::wstring(T(Str::ErrAutostart));
    } else {
        DeleteAutostartTask();
    }

    // 8) 注册表
    report(92, T(Str::StepRegistry));
    if (!WriteInstallerRegistry(installDir, dataRoot, req.dataMode, req.chinese, *segment)) {
        return std::wstring(T(Str::ErrRegistry));
    }
    CleanLegacyInstall(installDir);

    // 9) 完成页由 UI 处理
    report(96, T(Str::StepLaunch));
    if (req.launchAfter && !hadConfig) (void)ShellOpen(Join(installDir, kAppExe));

    report(100, L"");
    return std::wstring();
}

std::wstring RunUninstall(const UninstallRequest& req, ProgressFn progress, void* user) {
    SetUiLanguage(req.chinese);
    const Progress report{progress, user};

    std::wstring installDir = NormalizeDir(req.installDir);
    if (installDir.empty()) installDir = NormalizeDir(ParentDir(GetModulePath()));
    const std::wstring selfPath = req.exePath.empty() ? GetModulePath() : NormalizeDir(req.exePath);

    // 数据根要在删注册表之前读出来
    std::wstring dataRoot = NormalizeDir(req.dataRoot);
    if (dataRoot.empty()) {
        InstallInfo previous;
        if (ReadInstallInfo(&previous)) dataRoot = NormalizeDir(previous.dataRoot);
    }

    // 1) 关闭正在运行的实例（失败不阻断卸载）
    report(2, T(Str::StepStopping));
    KillProcessByExeName(kAppExe, kKillTimeoutMs);

    // 2) 计划任务 + 旧版 Run 项
    report(8, T(Str::StepRemoveAuto));
    DeleteAutostartTask();
    DeleteRegValue(HKEY_LOCAL_MACHINE, kRunKey, kProductName);
    DeleteRegValue(HKEY_CURRENT_USER, kRunKey, kProductName);

    // 3) 快捷方式
    report(14, T(Str::StepRemoveLinks));
    RemoveInstallerShortcuts();

    // 4) 注册表
    report(20, T(Str::StepRemoveReg));
    RegDeleteTree(HKEY_LOCAL_MACHINE, kUninstallKey);
    RegDeleteTree(HKEY_LOCAL_MACHINE, kMetaKey);
    CleanLegacyInstall(installDir);

    // 5) 程序文件（26..80）
    report(kRemoveFrom, T(Str::StepRemoveFiles));
    RemoveInstalledFiles(installDir, selfPath, report);

    // 6) 用户数据
    if (req.deleteUserData) {
        report(86, T(Str::StepRemoveData));
        RemoveUserData(dataRoot, installDir);
    }

    // 7) 自删除并由 UI 退出进程
    report(92, T(Str::StepFinalize));
    SpawnDetached(SelfDeleteCommandLine(installDir, selfPath));
    return std::wstring();
}

}  // namespace fc
