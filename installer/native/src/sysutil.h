#pragma once
// 安装器公共工具：字符串/路径/文件/进程/注册表/快捷方式。
// 只依赖 Win32 与标准库；失败路径返回 false，不弹窗、不抛异常。
#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

namespace fc {

// ---------------- 字符串 ----------------
std::wstring Utf8ToWide(const std::string& s);
std::string  WideToUtf8(const std::wstring& s);
std::wstring Format(const wchar_t* fmt, ...);

// ---------------- 路径 ----------------
std::wstring GetModulePath();          // 当前 exe 完整路径（已去掉 \\?\ 前缀）
std::wstring GetModuleDir();           // 当前 exe 所在目录，无尾随分隔符
std::wstring GetProgramFiles();        // C:\Program Files（优先 ProgramW6432）
std::wstring GetProgramFilesX86();
std::wstring GetAppDataRoaming();      // %APPDATA%
std::wstring GetLocalAppData();        // %LOCALAPPDATA%
std::wstring GetTempDir();
std::wstring GetDesktopDir();          // 当前用户桌面
std::wstring GetCommonDesktopDir();    // 公共桌面
std::wstring GetProgramsMenuDir();     // 开始菜单「程序」
std::wstring ExpandEnv(const std::wstring& text);
std::wstring Join(const std::wstring& a, const std::wstring& b);
std::wstring ParentDir(const std::wstring& path);
std::wstring FileNameOf(const std::wstring& path);
std::wstring NormalizeDir(const std::wstring& path);   // 展开变量、去尾随分隔符
bool         PathExists(const std::wstring& path);

// ---------------- 文件 ----------------
bool     FileExists(const std::wstring& path);
bool     DirExists(const std::wstring& path);
bool     EnsureDir(const std::wstring& path);
bool     EnsureDirForFile(const std::wstring& filePath);
bool     ReadAllBytes(const std::wstring& path, std::vector<uint8_t>* out);
bool     WriteAllBytes(const std::wstring& path, const void* data, size_t size);
bool     ReadAllText(const std::wstring& path, std::string* out);
bool     WriteAllText(const std::wstring& path, const std::string& text);
bool     DeleteFileIfExists(const std::wstring& path);
bool     DeleteTree(const std::wstring& path);        // 尽力删除；返回目录是否已不存在
uint64_t FileSizeOf(const std::wstring& path);

// ---------------- 进程 ----------------
DWORD FindProcessId(const wchar_t* exeName);
bool  KillProcessByExeName(const wchar_t* exeName, int timeoutMs);
bool  RunHidden(const std::wstring& cmdLine, DWORD timeoutMs, DWORD* exitCode);
bool  ShellOpen(const std::wstring& pathOrUrl);
bool  IsElevated();
bool  RelaunchElevated(const std::wstring& args);

// ---------------- 注册表 ----------------
bool RegSetString(HKEY root, const std::wstring& subKey, const std::wstring& name, const std::wstring& value);
bool RegSetDword(HKEY root, const std::wstring& subKey, const std::wstring& name, DWORD value);
bool RegGetString(HKEY root, const std::wstring& subKey, const std::wstring& name, std::wstring* value);
bool RegGetDword(HKEY root, const std::wstring& subKey, const std::wstring& name, DWORD* value);
bool RegDeleteTree(HKEY root, const std::wstring& subKey);

// ---------------- 快捷方式 ----------------
bool CreateShortcut(const std::wstring& lnkPath, const std::wstring& target, const std::wstring& args,
                    const std::wstring& workDir, const std::wstring& description);

// ---------------- 杂项 ----------------
uint32_t    Crc32Update(uint32_t crc, const void* data, size_t size);
uint32_t    Crc32(const void* data, size_t size);
bool        IsSystemDarkMode();
std::wstring GetSystemLocaleName();
ULONGLONG   NowMs();

}  // namespace fc
