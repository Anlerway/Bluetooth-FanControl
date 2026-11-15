#include "install.h"

#include <windows.h>
#include <sddl.h>
#include <winver.h>

#include <algorithm>
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
constexpr wchar_t kAppArgsAutostart[] = L"--autostart";
constexpr wchar_t kPublisher[]      = L"Anlerway";
constexpr wchar_t kAppExe[]         = L"FanControl.exe";
constexpr wchar_t kUninstallExe[]   = L"uninstall.exe";
constexpr wchar_t kShortcutName[]   = L"FanControl.lnk";
constexpr wchar_t kTaskName[]       = L"FanControl";
constexpr wchar_t kFallbackVersion[] = L"2.0.0";

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

// 安装过程的极简日志：排查"某一步没生效"（比如计划任务没建出来）时，让用户把这份文件发回来即可。
// 位置：%ProgramData%\FanControl\setup.log；写失败就静默放弃，绝不因为日志打断安装。
void SetupLog(const std::wstring& text) {
    const std::wstring programData = ExpandEnv(L"%ProgramData%");
    if (programData.empty()) return;
    const std::wstring root = Join(programData, kProductName);
    if (!EnsureDir(root)) return;
    HANDLE file = CreateFileW(Join(root, L"setup.log").c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    SYSTEMTIME st{};
    GetLocalTime(&st);
    const std::wstring line = Format(L"%04d-%02d-%02d %02d:%02d:%02d  %s\r\n", static_cast<int>(st.wYear),
                                     static_cast<int>(st.wMonth), static_cast<int>(st.wDay), static_cast<int>(st.wHour),
                                     static_cast<int>(st.wMinute), static_cast<int>(st.wSecond), text.c_str());
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, line.c_str(), static_cast<int>(line.size()), nullptr, 0, nullptr, nullptr);
    if (bytes > 0) {
        std::string utf8(static_cast<size_t>(bytes), '\0');
        WideCharToMultiByte(CP_UTF8, 0, line.c_str(), static_cast<int>(line.size()), utf8.data(), bytes, nullptr, nullptr);
        DWORD written = 0;
        WriteFile(file, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
    }
    CloseHandle(file);
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
// 按开关落盘：关掉的不但不建，还要删掉上一次安装留下的（升级时用户可能取消勾选）。
bool CreateInstallerShortcuts(const std::wstring& installDir, bool desktop, bool startMenu) {
    const std::wstring target = Join(installDir, kAppExe);
    const std::wstring args;                 // 无启动参数
    bool ok = true;

    const std::wstring programs = StartMenuDir();
    if (startMenu) {
        if (!programs.empty()) {
            ok = CreateShortcut(Join(programs, kShortcutName), target, args, installDir, kProductName) && ok;
        }
    } else if (!programs.empty()) {
        DeleteFileIfExists(Join(programs, kShortcutName));
    }

    const std::wstring common = NormalizeDir(GetCommonDesktopDir());
    if (desktop) {
        if (!common.empty()) {
            ok = CreateShortcut(Join(common, kShortcutName), target, args, installDir, kProductName) && ok;
        }
    } else if (!common.empty()) {
        DeleteFileIfExists(Join(common, kShortcutName));
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
    ok = RegSetString(HKEY_LOCAL_MACHINE, kUninstallKey, L"Publisher", kPublisher) && ok;
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
std::wstring XmlEscape(const std::wstring& text) {
    std::wstring out;
    out.reserve(text.size() + 8);
    for (const wchar_t ch : text) {
        switch (ch) {
        case L'&': out += L"&amp;"; break;
        case L'<': out += L"&lt;"; break;
        case L'>': out += L"&gt;"; break;
        case L'"': out += L"&quot;"; break;
        case L'\'': out += L"&apos;"; break;
        default: out.push_back(ch); break;
        }
    }
    return out;
}

// 任务的运行身份要写死成当前用户：schtasks /XML 不带 UserId 时，不同版本的 schtasks 注册出来的
// 主体可能落到别处，表现出来就是"任务建了但登录时没跑"。这里直接用进程令牌里的 SID。
std::wstring CurrentUserSid() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return std::wstring();
    DWORD needed = 0;
    (void)GetTokenInformation(token, TokenUser, nullptr, 0, &needed);
    std::wstring sid;
    if (needed > 0) {
        std::vector<uint8_t> buffer(needed, 0);
        if (GetTokenInformation(token, TokenUser, buffer.data(), needed, &needed)) {
            const auto* user = reinterpret_cast<const TOKEN_USER*>(buffer.data());
            wchar_t* text = nullptr;
            if (ConvertSidToStringSidW(const_cast<PSID>(user->User.Sid), &text)) {
                sid.assign(text);
                LocalFree(text);
            }
        }
    }
    CloseHandle(token);
    return sid;
}

// schtasks /Create 建出来的任务默认带 <DisallowStartIfOnBatteries>true</DisallowStartIfOnBatteries>，
// 笔记本用电池登录时任务根本不启动——表现出来就是"开机自启没生效"。改成注册一整份任务 XML：
// 关掉电池条件、补上工作目录、RunLevel=HighestAvailable。
std::wstring AutostartTaskXml(const std::wstring& installDir) {
    const std::wstring exe = XmlEscape(Join(installDir, kAppExe));
    const std::wstring dir = XmlEscape(installDir);
    std::wstring xml;
    xml += L"<?xml version=\"1.0\" encoding=\"UTF-16\"?>\r\n";
    xml += L"<Task version=\"1.2\" xmlns=\"http://schemas.microsoft.com/windows/2004/02/mit/task\">\r\n";
    xml += L"  <RegistrationInfo>\r\n";
    xml += L"    <Author>FanControl</Author>\r\n";
    xml += L"    <Description>FanControl autostart</Description>\r\n";
    xml += L"  </RegistrationInfo>\r\n";
    xml += L"  <Triggers>\r\n";
    xml += L"    <LogonTrigger>\r\n      <Enabled>true</Enabled>\r\n    </LogonTrigger>\r\n";
    xml += L"  </Triggers>\r\n";
    xml += L"  <Principals>\r\n";
    xml += L"    <Principal id=\"Author\">\r\n";
    const std::wstring sid = CurrentUserSid();
    if (!sid.empty()) {
        xml += L"      <UserId>";
        xml += sid;
        xml += L"</UserId>\r\n";
    }
    xml += L"      <LogonType>InteractiveToken</LogonType>\r\n";
    xml += L"      <RunLevel>HighestAvailable</RunLevel>\r\n";
    xml += L"    </Principal>\r\n";
    xml += L"  </Principals>\r\n";
    xml += L"  <Settings>\r\n";
    xml += L"    <MultipleInstancesPolicy>IgnoreNew</MultipleInstancesPolicy>\r\n";
    xml += L"    <DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries>\r\n";
    xml += L"    <StopIfGoingOnBatteries>false</StopIfGoingOnBatteries>\r\n";
    xml += L"    <AllowHardTerminate>true</AllowHardTerminate>\r\n";
    xml += L"    <StartWhenAvailable>true</StartWhenAvailable>\r\n";
    xml += L"    <RunOnlyIfNetworkAvailable>false</RunOnlyIfNetworkAvailable>\r\n";
    xml += L"    <IdleSettings>\r\n";
    xml += L"      <StopOnIdleEnd>false</StopOnIdleEnd>\r\n";
    xml += L"      <RestartOnIdle>false</RestartOnIdle>\r\n";
    xml += L"    </IdleSettings>\r\n";
    xml += L"    <AllowStartOnDemand>true</AllowStartOnDemand>\r\n";
    xml += L"    <Enabled>true</Enabled>\r\n";
    xml += L"    <Hidden>false</Hidden>\r\n";
    xml += L"    <RunOnlyIfIdle>false</RunOnlyIfIdle>\r\n";
    xml += L"    <WakeToRun>false</WakeToRun>\r\n";
    xml += L"    <ExecutionTimeLimit>PT0S</ExecutionTimeLimit>\r\n";
    xml += L"    <Priority>7</Priority>\r\n";
    xml += L"  </Settings>\r\n";
    xml += L"  <Actions Context=\"Author\">\r\n";
    xml += L"    <Exec>\r\n";
    xml += L"      <Command>";
    xml += exe;
    xml += L"</Command>\r\n";
    xml += L"      <Arguments>--autostart</Arguments>\r\n";
    xml += L"      <WorkingDirectory>";
    xml += dir;
    xml += L"</WorkingDirectory>\r\n";
    xml += L"    </Exec>\r\n";
    xml += L"  </Actions>\r\n";
    xml += L"</Task>\r\n";
    return xml;
}

bool WriteUtf16File(const std::wstring& path, const std::wstring& text) {
    std::string bytes;
    bytes.reserve(text.size() * 2 + 2);
    bytes.push_back(static_cast<char>(0xFF));
    bytes.push_back(static_cast<char>(0xFE));
    for (const wchar_t ch : text) {
        const unsigned int value = static_cast<unsigned int>(ch);
        bytes.push_back(static_cast<char>(value & 0xFFu));
        bytes.push_back(static_cast<char>((value >> 8) & 0xFFu));
    }
    return WriteAllBytes(path, bytes.data(), bytes.size());
}

// 任务自己的文件：%SystemRoot%\System32\Tasks\<任务名>。schtasks 建成功时它一定存在，
// 内容是任务 XML（UTF-16）——用它核对"命令确实指向本次安装的 exe"，比只看退出码可靠。
std::wstring TaskFilePath() {
    wchar_t buffer[MAX_PATH] = {};
    const UINT length = GetSystemDirectoryW(buffer, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) return std::wstring();
    return Join(Join(buffer, L"Tasks"), kTaskName);
}

bool TaskFileMentions(const std::wstring& needle) {
    std::vector<uint8_t> bytes;
    if (needle.empty() || !ReadAllBytes(TaskFilePath(), &bytes) || bytes.empty()) return false;
    std::vector<uint8_t> pattern;
    pattern.reserve(needle.size() * 2);
    for (const wchar_t ch : needle) {
        pattern.push_back(static_cast<uint8_t>(ch & 0xFF));
        pattern.push_back(static_cast<uint8_t>((static_cast<unsigned int>(ch) >> 8) & 0xFF));
    }
    return std::search(bytes.begin(), bytes.end(), pattern.begin(), pattern.end()) != bytes.end();
}

void DeleteAutostartTask();

// 兜底的自启方式：写 HKCU\...\Run（不需要管理员，也不会被"电池供电"条件拦住）。
bool CreateRunKeyAutostart(const std::wstring& exe) {
    std::wstring value = L"\"";
    value += exe;
    value += L"\" ";
    value += kAppArgsAutostart;
    const bool ok = RegSetString(HKEY_CURRENT_USER, kRunKey, kProductName, value);
    SetupLog(Format(L"autostart: HKCU Run fallback ok=%d", ok ? 1 : 0));
    return ok;
}

bool CreateAutostartTask(const std::wstring& installDir) {
    const std::wstring exe = Join(installDir, kAppExe);
    if (!FileExists(exe)) {
        SetupLog(L"autostart: exe missing " + exe);
        return false;
    }

    // 先清掉旧任务：上一版可能用 /TR 建（带"电池供电不启动"），也可能指向别的安装目录。
    DeleteAutostartTask();

    // 首选：整份任务 XML（电池条件可控、带工作目录、运行身份写死成当前用户）
    const std::wstring xmlPath = Join(GetTempDir(), L"FanControl-autostart.xml");
    bool ran = false;
    DWORD exitCode = 1;
    if (WriteUtf16File(xmlPath, AutostartTaskXml(installDir))) {
        std::wstring cmd = L"schtasks.exe /Create /TN \"";
        cmd += kTaskName;
        cmd += L"\" /XML \"";
        cmd += xmlPath;
        cmd += L"\" /F";
        ran = RunHidden(cmd, kTaskTimeoutMs, &exitCode);
    } else {
        SetupLog(L"autostart: write task xml failed " + xmlPath);
    }
    DeleteFileIfExists(xmlPath);
    SetupLog(Format(L"autostart: schtasks /XML ran=%d exit=%lu taskFile=%d", ran ? 1 : 0, exitCode,
                    FileExists(TaskFilePath()) ? 1 : 0));
    if (ran && exitCode == 0 && TaskFileMentions(exe)) return true;

    // 回退：老的 /TR 写法。能建出任务，但 schtasks 会给它加上"电池供电时不启动"，笔记本上可能不触发。
    std::wstring cmd = L"schtasks.exe /Create /TN \"";
    cmd += kTaskName;
    cmd += L"\" /TR \"\\\"";
    cmd += exe;
    cmd += L"\\\" --autostart\" /SC ONLOGON /RL HIGHEST /F";

    ran = false;
    exitCode = 1;
    ran = RunHidden(cmd, kTaskTimeoutMs, &exitCode);
    SetupLog(Format(L"autostart: schtasks /TR ran=%d exit=%lu taskFile=%d", ran ? 1 : 0, exitCode,
                    FileExists(TaskFilePath()) ? 1 : 0));
    return ran && exitCode == 0 && TaskFileMentions(exe);
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
// Windows 不允许删除正在运行的镜像：实测 POSIX 语义删除（FileDispositionInfoEx +
// FILE_DISPOSITION_POSIX_SEMANTICS）返回 err=5，跨卷改名同样 err=5。主流安装器（Inno 的
// _iu14D2N.tmp、NSIS 的 $TEMP\ns*.tmp\Au_.exe）都是"另起一个临时副本进程替我收尾"，这里照做：
// 卸载器把自身复制到 %TEMP%，用 --cleanup 启动副本后立刻退出；副本等本进程结束后删掉
// uninstall.exe、尝试删掉已经变空的安装目录，最后把副本自己登记成重启时删除。
// 全程不用 cmd/ping 延时，不会弹出控制台窗口，也不依赖任何安装框架。
constexpr wchar_t kCleanupSwitch[] = L"--cleanup";
constexpr wchar_t kCleanupPrefix[] = L"FanControl-cleanup-";

// ---------------- 多版本运行时 ----------------
// 主程序是 net8.0-windows 但没有任何 WinForms/WPF 引用（托盘是裸 Shell_NotifyIcon），
// 发布的 FanControl.runtimeconfig.json 里 framework 只写了 Microsoft.NETCore.App 8.0.0，
// 所以精简版负载只需要基础的 .NET 运行时就够，不必要求 Windows Desktop Runtime。
bool HasRuntime8In(const std::wstring& programFiles) {
    if (programFiles.empty()) return false;
    const std::wstring root = Join(Join(Join(programFiles, L"dotnet"), L"shared"), L"Microsoft.NETCore.App");
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
namespace {
// 启动副本进程：不显示窗口，并优先脱离宿主的作业对象（否则父进程一退出副本就被连带杀掉）。
bool SpawnHidden(const std::wstring& exePath, const std::wstring& args) {
    std::wstring commandLine = L"\"";
    commandLine += exePath;
    commandLine += L"\" ";
    commandLine += args;
    std::vector<wchar_t> buffer(commandLine.begin(), commandLine.end());
    buffer.push_back(L'\0');

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION info{};
    BOOL created = CreateProcessW(exePath.c_str(), buffer.data(), nullptr, nullptr, FALSE,
                                  CREATE_NO_WINDOW | CREATE_BREAKAWAY_FROM_JOB, nullptr, nullptr, &startup, &info);
    if (!created) {
        startup = STARTUPINFOW{};
        startup.cb = sizeof(startup);
        info = PROCESS_INFORMATION{};
        created = CreateProcessW(exePath.c_str(), buffer.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                                 nullptr, &startup, &info);
    }
    if (!created) return false;
    CloseHandle(info.hThread);
    CloseHandle(info.hProcess);
    return true;
}
}  // namespace

bool StartSelfCleanup(const std::wstring& installDir, const std::wstring& selfPath) {
    if (selfPath.empty() || !FileExists(selfPath)) return false;

    const std::wstring temp = GetTempDir();
    if (temp.empty()) return false;
    const std::wstring copy =
        Join(temp, std::wstring(kCleanupPrefix) + std::to_wstring(GetCurrentProcessId()) + L".exe");
    DeleteFileIfExists(copy);
    if (!CopyFileW(selfPath.c_str(), copy.c_str(), FALSE)) {
        SetupLog(L"cleanup: copy failed " + copy);
        return false;
    }

    std::wstring args = kCleanupSwitch;
    args += L" \"";
    args += TrimSeparator(installDir);
    args += L"\" \"";
    args += selfPath;
    args += L"\" ";
    args += std::to_wstring(GetCurrentProcessId());
    if (!SpawnHidden(copy, args)) {
        SetupLog(L"cleanup: spawn failed " + copy);
        DeleteFileIfExists(copy);
        return false;
    }
    SetupLog(L"cleanup: worker started " + copy);
    return true;
}

int RunCleanupWorker(const std::wstring& installDir, const std::wstring& selfPath, unsigned long parentPid) {
    if (parentPid != 0) {
        // 父卸载器停在"卸载完成"页面上可能有一会儿，这里给足 10 分钟：副本没有任何界面，
        // 多等一会儿用户也感觉不到，但换来的是"关掉完成页就一定删干净"。
        HANDLE parent = OpenProcess(SYNCHRONIZE, FALSE, parentPid);
        if (parent != nullptr) {
            WaitForSingleObject(parent, 600000);
            CloseHandle(parent);
        }
    }

    // 原卸载器退出后它的镜像才解锁；给杀软/文件索引一点时间，重试 40 次 × 250ms。
    bool removed = false;
    for (int attempt = 0; attempt < 40 && !removed; ++attempt) {
        DeleteFileIfExists(selfPath);
        removed = !FileExists(selfPath);
        if (!removed) Sleep(250);
    }

    const std::wstring dir = TrimSeparator(installDir);
    if (removed && !dir.empty()) {
        // 只有目录已经空了才删得掉：用户自己放进去的文件不会被连带清掉。
        (void)RemoveDirectoryW(dir.c_str());
    }

    // 副本藏在 %TEMP%：先试着直接删（此刻自己还锁着，通常失败），删不掉就登记到重启时删除。
    const std::wstring selfCopy = GetModulePath();
    if (!selfCopy.empty()) {
        DeleteFileIfExists(selfCopy);
        if (FileExists(selfCopy)) (void)MoveFileExW(selfCopy.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
    }
    return removed ? 0 : 1;
}
bool AutostartEnabled() {
    std::wstring cmd = L"schtasks.exe /Query /TN \"";
    cmd += kTaskName;
    cmd += L"\"";
    DWORD exitCode = 1;
    if (RunHidden(cmd, kTaskTimeoutMs, &exitCode) && exitCode == 0) return true;
    // 兜底写在 HKCU\...\Run 里的同名值也算开启（升级时恢复界面开关用）
    std::wstring value;
    return RegGetString(HKEY_CURRENT_USER, kRunKey, kProductName, &value) && !value.empty();
}

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

bool HasDotnetRuntime8() {
    return HasRuntime8In(GetProgramFiles()) || HasRuntime8In(GetProgramFilesX86());
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

// 安装包里嵌着卸载器（setup.rc 的 RCDATA），安装时原样写进安装目录
#ifndef FC_IDR_UNINSTALL_EXE
#define FC_IDR_UNINSTALL_EXE 2001
#endif

bool WriteBundledUninstaller(const std::wstring& installDir) {
    HRSRC res = FindResourceW(nullptr, MAKEINTRESOURCEW(FC_IDR_UNINSTALL_EXE), RT_RCDATA);
    if (res == nullptr) return false;
    HGLOBAL block = LoadResource(nullptr, res);
    const DWORD size = SizeofResource(nullptr, res);
    const void* bytes = (block != nullptr) ? LockResource(block) : nullptr;
    if (bytes == nullptr || size == 0) return false;
    const std::wstring target = Join(installDir, kUninstallExe);
    DeleteFileIfExists(target);
    return WriteAllBytes(target, bytes, static_cast<size_t>(size));
}

std::wstring RunInstall(const InstallRequest& req, ProgressFn progress, void* user) {
    SetUiLanguage(req.chinese);
    const Progress report{progress, user};

    std::wstring installDir = NormalizeDir(req.installDir);
    if (installDir.empty()) installDir = DefaultInstallDir();
    const std::wstring customDir =
        req.dataMode == DataMode::Custom ? NormalizeDir(req.customDir) : std::wstring();
    const std::wstring dataRoot = ResolveDataRoot(req.dataMode, installDir, customDir);
    const std::wstring packagePath = req.exePath.empty() ? GetModulePath() : req.exePath;

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

    // 4b) 卸载器：控制面板「卸载」指向它
    if (!WriteBundledUninstaller(installDir)) return std::wstring(T(Str::ErrUninstaller));

    // 5) 配置（与主程序 ConfigManager 同布局）
    report(72, T(Str::StepConfig));
    const std::wstring configError = WriteInstallerConfig(req.dataMode, dataRoot, customDir, req.chinese);
    if (!configError.empty()) return configError;

    // 6) 快捷方式
    report(80, T(Str::StepShortcuts));
    if (!CreateInstallerShortcuts(installDir, req.desktopShortcut, req.startMenuShortcut)) {
        return std::wstring(T(Str::ErrShortcut));
    }

    // 7) 开机自启：计划任务是主路；组策略/权限把任务建不出来时退到 HKCU\...\Run，
    //    至少保证"登录后自动启动"这件事真的发生（Run 项不带提权，但主程序自己会再自提权）。
    report(86, T(Str::StepAutostart));
    if (req.autostart) {
        if (!CreateAutostartTask(installDir) && !CreateRunKeyAutostart(Join(installDir, kAppExe))) {
            return std::wstring(T(Str::ErrAutostart));
        }
    } else {
        DeleteAutostartTask();
    }

    // 8) 注册表
    report(92, T(Str::StepRegistry));
    if (!WriteInstallerRegistry(installDir, dataRoot, req.dataMode, req.chinese, *segment)) {
        return std::wstring(T(Str::ErrRegistry));
    }
    CleanLegacyInstall(installDir);

    // 9) 主程序由完成页的「完成」按钮启动（勾选了"安装完成后启动"时），引擎不抢先开。
    report(96, T(Str::StepFinalize));

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

    // 7) 收尾：Windows 删不掉正在运行的 exe，交给 %TEMP% 里的副本进程删自己、删空的安装目录。
    report(92, T(Str::StepFinalize));
    if (!StartSelfCleanup(installDir, selfPath)) {
        // 副本都起不来时兜底：至少把自身登记成重启删除，别留下永远删不掉的残留。
        (void)MoveFileExW(selfPath.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
    }
    return std::wstring();
}

}  // namespace fc
