// 安装器公共工具实现：字符串 / 路径 / 文件 / 进程 / 注册表 / 快捷方式 / 杂项。
// 仅依赖 Win32 与标准库；失败路径返回 false，不弹窗、不抛异常。
#include "sysutil.h"

#include <objbase.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <tlhelp32.h>

#include <algorithm>
#include <array>
#include <climits>
#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <string>
#include <vector>

#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "shlwapi.lib")

namespace fc {
namespace {

constexpr size_t kMaxPathLen  = 65536;      // 模块路径 / 临时路径的扩容上限
constexpr size_t kIoChunk     = 64 * 1024;  // 读写分块大小
constexpr int    kPollMs      = 50;         // 进程等待轮询间隔

bool IsSep(wchar_t c) { return c == L'\\' || c == L'/'; }

// 把未配对的代理项替换为 U+FFFD，供 WideCharToMultiByte 兜底使用。
std::wstring SanitizeSurrogates(const std::wstring& s) {
    std::wstring out;
    out.reserve(s.size() + 4);
    for (size_t i = 0; i < s.size(); ++i) {
        const wchar_t c = s[i];
        if (c >= 0xD800 && c <= 0xDBFF) {
            if (i + 1 < s.size() && s[i + 1] >= 0xDC00 && s[i + 1] <= 0xDFFF) {
                out.push_back(c);
                out.push_back(s[++i]);
            } else {
                out.push_back(static_cast<wchar_t>(0xFFFD));
            }
        } else if (c >= 0xDC00 && c <= 0xDFFF) {
            out.push_back(static_cast<wchar_t>(0xFFFD));
        } else {
            out.push_back(c);
        }
    }
    return out;
}

// 规范化成绝对路径（折叠 . 与 ..）；失败时原样返回。
std::wstring CanonicalizePath(const std::wstring& path) {
    if (path.empty()) return path;
    std::vector<wchar_t> buf(MAX_PATH, L'\0');
    for (;;) {
        const DWORD n = GetFullPathNameW(path.c_str(), static_cast<DWORD>(buf.size()), buf.data(), nullptr);
        if (n == 0) return path;
        if (static_cast<size_t>(n) < buf.size()) return std::wstring(buf.data(), static_cast<size_t>(n));
        if (buf.size() >= kMaxPathLen) return path;
        buf.resize(std::min<size_t>(buf.size() * 2, kMaxPathLen));
    }
}

// 去掉 \\?\ 前缀；\\?\UNC\server\share 还原为 \\server\share。
std::wstring StripLongPrefix(const std::wstring& path) {
    if (path.size() < 4) return path;
    if (path[0] != L'\\' || path[1] != L'\\' || path[2] != L'?' || path[3] != L'\\') return path;
    const std::wstring rest = path.substr(4);
    if (rest.size() >= 4 && (rest.compare(0, 4, L"UNC\\") == 0 || rest.compare(0, 4, L"unc\\") == 0)) {
        return L"\\\\" + rest.substr(4);
    }
    return rest;
}

// 去掉尾随分隔符，但保留 "C:\" 这类根。
std::wstring TrimTrailingSeps(const std::wstring& path) {
    size_t end = path.size();
    while (end > 1 && IsSep(path[end - 1])) {
        if (end == 3 && path[1] == L':') break;  // 保留 "C:\"
        --end;
    }
    return path.substr(0, end);
}

std::wstring FromEnv(const wchar_t* name) {
    if (name == nullptr || name[0] == L'\0') return std::wstring();
    const DWORD need = GetEnvironmentVariableW(name, nullptr, 0);
    if (need == 0) return std::wstring();
    std::vector<wchar_t> buf(static_cast<size_t>(need) + 1, L'\0');
    const DWORD got = GetEnvironmentVariableW(name, buf.data(), static_cast<DWORD>(buf.size()));
    if (got == 0 || static_cast<size_t>(got) >= buf.size()) return std::wstring();
    return std::wstring(buf.data(), static_cast<size_t>(got));
}

std::wstring FromCsidl(int csidl) {
    wchar_t buf[MAX_PATH] = {};
    if (SUCCEEDED(SHGetFolderPathW(nullptr, csidl, nullptr, SHGFP_TYPE_CURRENT, buf))) {
        return TrimTrailingSeps(std::wstring(buf));
    }
    return std::wstring();
}

std::wstring QueryTempDir() {
    std::vector<wchar_t> buf(MAX_PATH, L'\0');
    for (;;) {
        const DWORD n = GetTempPathW(static_cast<DWORD>(buf.size()), buf.data());
        if (n == 0) return std::wstring();
        if (static_cast<size_t>(n) < buf.size()) {
            return TrimTrailingSeps(std::wstring(buf.data(), static_cast<size_t>(n)));
        }
        if (buf.size() >= kMaxPathLen) {
            return TrimTrailingSeps(std::wstring(buf.data(), static_cast<size_t>(n)));
        }
        buf.resize(std::min<size_t>(buf.size() * 2, kMaxPathLen));
    }
}

bool RemoveDirRecursive(const std::wstring& dir);

// SHFileOperationW 快速删除；路径必须双 NUL 结尾。
bool DeleteTreeViaShell(const std::wstring& dir) {
    std::vector<wchar_t> from(dir.size() + 2, L'\0');
    if (!dir.empty()) std::memcpy(from.data(), dir.c_str(), dir.size() * sizeof(wchar_t));
    SHFILEOPSTRUCTW op = {};
    op.hwnd = nullptr;
    op.wFunc = FO_DELETE;
    op.pFrom = from.data();
    op.pTo = nullptr;
    op.fFlags = static_cast<FILEOP_FLAGS>(FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT | FOF_NOCONFIRMMKDIR);
    op.fAnyOperationsAborted = FALSE;
    const int rc = SHFileOperationW(&op);
    return rc == 0 && op.fAnyOperationsAborted == FALSE;
}

bool RemoveDirRecursive(const std::wstring& dir) {
    const std::wstring pattern = Join(dir, L"*");
    WIN32_FIND_DATAW fd = {};
    HANDLE find = FindFirstFileW(pattern.c_str(), &fd);
    if (find != INVALID_HANDLE_VALUE) {
        do {
            if (std::wcscmp(fd.cFileName, L".") == 0 || std::wcscmp(fd.cFileName, L"..") == 0) continue;
            const std::wstring child = Join(dir, fd.cFileName);
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
                if ((fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
                    RemoveDirectoryW(child.c_str());  // 不跟随 junction / 符号链接
                } else {
                    RemoveDirRecursive(child);
                }
            } else {
                SetFileAttributesW(child.c_str(), FILE_ATTRIBUTE_NORMAL);
                DeleteFileW(child.c_str());
            }
        } while (FindNextFileW(find, &fd) != FALSE);
        FindClose(find);
    }
    RemoveDirectoryW(dir.c_str());
    return DirExists(dir) == false;
}

const std::array<uint32_t, 256>& CrcTable() {
    static const std::array<uint32_t, 256> table = [] {
        std::array<uint32_t, 256> t = {};
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = ((c & 1u) != 0) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            t[i] = c;
        }
        return t;
    }();
    return table;
}

}  // namespace

// ---------------- 字符串 ----------------

std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return std::wstring();
    if (s.size() > static_cast<size_t>(INT_MAX)) return std::wstring();
    const int len = static_cast<int>(s.size());
    const int need = MultiByteToWideChar(CP_UTF8, 0, s.data(), len, nullptr, 0);
    if (need <= 0) return std::wstring();
    std::wstring out(static_cast<size_t>(need), L'\0');
    const int got = MultiByteToWideChar(CP_UTF8, 0, s.data(), len, out.data(), need);
    if (got <= 0) return std::wstring();
    out.resize(static_cast<size_t>(got));
    return out;
}

std::string WideToUtf8(const std::wstring& s) {
    if (s.empty()) return std::string();
    if (s.size() > static_cast<size_t>(INT_MAX)) return std::string();
    const wchar_t* src = s.data();
    int len = static_cast<int>(s.size());
    int need = WideCharToMultiByte(CP_UTF8, 0, src, len, nullptr, 0, nullptr, nullptr);
    std::wstring sanitized;  // 仅在含未配对代理项时用于兜底
    if (need <= 0) {
        sanitized = SanitizeSurrogates(s);
        src = sanitized.c_str();
        len = static_cast<int>(sanitized.size());
        need = WideCharToMultiByte(CP_UTF8, 0, src, len, nullptr, 0, nullptr, nullptr);
        if (need <= 0) return std::string();
    }
    std::string out(static_cast<size_t>(need), '\0');
    const int got = WideCharToMultiByte(CP_UTF8, 0, src, len, out.data(), need, nullptr, nullptr);
    if (got <= 0) return std::string();
    out.resize(static_cast<size_t>(got));
    return out;
}

std::wstring Format(const wchar_t* fmt, ...) {
    if (fmt == nullptr) return std::wstring();
    va_list args;
    va_start(args, fmt);
    va_list args_copy;
    va_copy(args_copy, args);
    const int len = _vscwprintf(fmt, args);
    va_end(args);
    if (len < 0) {
        va_end(args_copy);
        return std::wstring();
    }
    std::vector<wchar_t> buf(static_cast<size_t>(len) + 1, L'\0');
    const int written = vswprintf_s(buf.data(), buf.size(), fmt, args_copy);
    va_end(args_copy);
    if (written <= 0) return std::wstring();
    return std::wstring(buf.data(), static_cast<size_t>(written));
}

// ---------------- 路径 ----------------

std::wstring GetModulePath() {
    std::vector<wchar_t> buf(MAX_PATH, L'\0');
    for (;;) {
        const DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
        if (n == 0) return std::wstring();
        if (static_cast<size_t>(n) < buf.size() || buf.size() >= kMaxPathLen) {
            return CanonicalizePath(StripLongPrefix(std::wstring(buf.data(), static_cast<size_t>(n))));
        }
        buf.resize(std::min<size_t>(buf.size() * 2, kMaxPathLen));
    }
}

std::wstring GetModuleDir() {
    return ParentDir(GetModulePath());
}

std::wstring GetProgramFiles() {
    std::wstring v = FromEnv(L"ProgramW6432");
    if (!v.empty()) return TrimTrailingSeps(v);
    v = FromEnv(L"ProgramFiles");
    if (!v.empty()) return TrimTrailingSeps(v);
    return FromCsidl(CSIDL_PROGRAM_FILES);
}

std::wstring GetProgramFilesX86() {
    std::wstring v = FromEnv(L"ProgramFiles(x86)");
    if (!v.empty()) return TrimTrailingSeps(v);
    v = FromEnv(L"ProgramFiles");
    if (!v.empty()) return TrimTrailingSeps(v);
    return FromCsidl(CSIDL_PROGRAM_FILESX86);
}

std::wstring GetAppDataRoaming() {
    std::wstring v = FromCsidl(CSIDL_APPDATA);
    if (!v.empty()) return v;
    return TrimTrailingSeps(FromEnv(L"APPDATA"));
}

std::wstring GetLocalAppData() {
    std::wstring v = FromCsidl(CSIDL_LOCAL_APPDATA);
    if (!v.empty()) return v;
    return TrimTrailingSeps(FromEnv(L"LOCALAPPDATA"));
}

std::wstring GetTempDir() {
    return QueryTempDir();
}

std::wstring GetDesktopDir() {
    return FromCsidl(CSIDL_DESKTOPDIRECTORY);
}

std::wstring GetCommonDesktopDir() {
    return FromCsidl(CSIDL_COMMON_DESKTOPDIRECTORY);
}

std::wstring GetProgramsMenuDir() {
    return FromCsidl(CSIDL_PROGRAMS);
}

std::wstring ExpandEnv(const std::wstring& text) {
    if (text.empty()) return std::wstring();
    const DWORD need = ExpandEnvironmentStringsW(text.c_str(), nullptr, 0);
    if (need == 0) return text;
    std::vector<wchar_t> buf(static_cast<size_t>(need) + 1, L'\0');
    const DWORD got = ExpandEnvironmentStringsW(text.c_str(), buf.data(), static_cast<DWORD>(buf.size()));
    if (got == 0 || static_cast<size_t>(got) > buf.size()) return text;
    return std::wstring(buf.data(), static_cast<size_t>(got) - 1);  // got 含结尾 NUL
}

std::wstring Join(const std::wstring& a, const std::wstring& b) {
    if (a.empty()) return b;
    if (b.empty()) return a;
    std::wstring out = a;
    if (!IsSep(out.back())) out.push_back(L'\\');
    size_t i = 0;
    while (i < b.size() && IsSep(b[i])) ++i;  // 避免出现双分隔符
    out.append(b, i, std::wstring::npos);
    return out;
}

std::wstring ParentDir(const std::wstring& path) {
    if (path.empty()) return std::wstring();
    const std::wstring p = TrimTrailingSeps(path);
    if (p.empty()) return std::wstring();
    if (p.size() == 2 && p[1] == L':') return std::wstring();  // "C:" 无父目录
    if (p.size() == 3 && p[1] == L':' && IsSep(p[2])) return p;  // "C:\" 是其自身
    size_t i = p.size();
    while (i > 0 && !IsSep(p[i - 1])) --i;
    if (i == 0) return std::wstring();  // 无分隔符，无父目录
    size_t cut = i - 1;                 // p[cut] 为最后一个分隔符
    if (cut == 0) return std::wstring();
    if (cut == 2 && p[1] == L':') return p.substr(0, 3);  // "C:\foo" -> "C:\"
    while (cut > 2 && IsSep(p[cut - 1])) --cut;           // 折叠重复分隔符
    return p.substr(0, cut);
}

std::wstring FileNameOf(const std::wstring& path) {
    if (path.empty()) return std::wstring();
    const std::wstring p = TrimTrailingSeps(path);
    if (p.empty()) return std::wstring();
    if (p.size() == 3 && p[1] == L':' && IsSep(p[2])) return std::wstring();  // "C:\" 无文件名
    size_t i = p.size();
    while (i > 0 && !IsSep(p[i - 1])) --i;
    return p.substr(i);
}

std::wstring NormalizeDir(const std::wstring& path) {
    if (path.empty()) return std::wstring();
    std::wstring p = ExpandEnv(path);
    p = StripLongPrefix(p);
    return TrimTrailingSeps(p);
}

bool PathExists(const std::wstring& path) {
    if (path.empty()) return false;
    return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

// ---------------- 文件 ----------------

bool FileExists(const std::wstring& path) {
    if (path.empty()) return false;
    const DWORD attr = GetFileAttributesW(path.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES) return false;
    return (attr & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

bool DirExists(const std::wstring& path) {
    if (path.empty()) return false;
    const DWORD attr = GetFileAttributesW(path.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES) return false;
    return (attr & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

bool EnsureDir(const std::wstring& path) {
    if (path.empty()) return false;
    const std::wstring p = NormalizeDir(path);
    if (p.empty()) return false;
    if (DirExists(p)) return true;

    std::wstring cur;
    size_t i = 0;
    if (p.size() >= 2 && IsSep(p[0]) && IsSep(p[1])) {
        // UNC：先把 \\server\share 作为起点
        size_t pos = 2;
        while (pos < p.size() && !IsSep(p[pos])) ++pos;
        if (pos < p.size()) {
            ++pos;
            while (pos < p.size() && !IsSep(p[pos])) ++pos;
        }
        cur = p.substr(0, pos);
        i = pos;
    } else if (p.size() >= 2 && p[1] == L':') {
        cur = p.substr(0, 2);
        i = 2;
        if (i < p.size() && IsSep(p[i])) {
            cur.push_back(L'\\');
            ++i;
        }
    }

    while (i < p.size()) {
        size_t j = i;
        while (j < p.size() && !IsSep(p[j])) ++j;
        if (j > i) {
            if (!cur.empty() && !IsSep(cur.back())) cur.push_back(L'\\');
            cur.append(p, i, j - i);
            if (!CreateDirectoryW(cur.c_str(), nullptr)) {
                const DWORD err = GetLastError();
                if (err != ERROR_ALREADY_EXISTS && !DirExists(cur)) return false;
            }
        }
        i = j + 1;
    }
    return DirExists(p);
}

bool EnsureDirForFile(const std::wstring& filePath) {
    if (filePath.empty()) return false;
    const std::wstring parent = ParentDir(filePath);
    if (parent.empty()) return true;  // 相对当前目录，无需创建
    return EnsureDir(parent);
}

bool ReadAllBytes(const std::wstring& path, std::vector<uint8_t>* out) {
    if (out == nullptr) return false;
    out->clear();
    if (path.empty()) return false;
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;

    bool ok = true;
    std::vector<uint8_t> chunk(kIoChunk);
    for (;;) {
        DWORD got = 0;
        if (!ReadFile(file, chunk.data(), static_cast<DWORD>(chunk.size()), &got, nullptr)) {
            ok = false;
            break;
        }
        if (got == 0) break;  // EOF
        out->insert(out->end(), chunk.begin(), chunk.begin() + static_cast<size_t>(got));
    }
    CloseHandle(file);
    if (!ok) out->clear();
    return ok;
}

bool WriteAllBytes(const std::wstring& path, const void* data, size_t size) {
    if (path.empty()) return false;
    if (data == nullptr && size != 0) return false;
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;

    const auto* bytes = static_cast<const uint8_t*>(data);
    size_t done = 0;
    bool ok = true;
    while (done < size) {
        const size_t remain = size - done;
        const DWORD want = static_cast<DWORD>(std::min<size_t>(remain, 1u << 20));
        DWORD wrote = 0;
        if (!WriteFile(file, bytes + done, want, &wrote, nullptr) || wrote == 0) {
            ok = false;
            break;
        }
        done += static_cast<size_t>(wrote);
    }
    CloseHandle(file);
    return ok;
}

bool ReadAllText(const std::wstring& path, std::string* out) {
    if (out == nullptr) return false;
    out->clear();
    std::vector<uint8_t> bytes;
    if (!ReadAllBytes(path, &bytes)) return false;
    size_t skip = 0;  // 容忍带 BOM 的输入，输出始终为无 BOM 文本
    if (bytes.size() >= 3 && bytes[0] == 0xEF && bytes[1] == 0xBB && bytes[2] == 0xBF) skip = 3;
    if (bytes.size() > skip) {
        out->assign(reinterpret_cast<const char*>(bytes.data()) + skip, bytes.size() - skip);
    }
    return true;
}

bool WriteAllText(const std::wstring& path, const std::string& text) {
    return WriteAllBytes(path, text.data(), text.size());
}

bool DeleteFileIfExists(const std::wstring& path) {
    if (path.empty()) return true;
    if (DeleteFileW(path.c_str())) return true;
    const DWORD err = GetLastError();
    return err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND;
}

bool DeleteTree(const std::wstring& path) {
    if (path.empty()) return true;
    const std::wstring p = NormalizeDir(path);
    if (p.empty()) return true;
    const DWORD attr = GetFileAttributesW(p.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES) return true;  // 已不存在
    if ((attr & FILE_ATTRIBUTE_DIRECTORY) == 0) {
        DeleteFileIfExists(p);
        return FileExists(p) == false;
    }
    DeleteTreeViaShell(p);
    if (DirExists(p)) RemoveDirRecursive(p);
    return DirExists(p) == false;
}

uint64_t FileSizeOf(const std::wstring& path) {
    if (path.empty()) return 0;
    WIN32_FILE_ATTRIBUTE_DATA data = {};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) return 0;
    return (static_cast<uint64_t>(data.nFileSizeHigh) << 32) | static_cast<uint64_t>(data.nFileSizeLow);
}

// ---------------- 进程 ----------------

DWORD FindProcessId(const wchar_t* exeName) {
    if (exeName == nullptr || exeName[0] == L'\0') return 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W entry = {};
    entry.dwSize = sizeof(entry);
    DWORD found = 0;
    if (Process32FirstW(snap, &entry)) {
        do {
            if (_wcsicmp(entry.szExeFile, exeName) == 0) {
                found = entry.th32ProcessID;
                break;
            }
        } while (Process32NextW(snap, &entry));
    }
    CloseHandle(snap);
    return found;
}

bool KillProcessByExeName(const wchar_t* exeName, int timeoutMs) {
    if (exeName == nullptr || exeName[0] == L'\0') return false;
    if (timeoutMs < 0) timeoutMs = 0;
    const ULONGLONG deadline = GetTickCount64() + static_cast<ULONGLONG>(timeoutMs);

    const DWORD pid = FindProcessId(exeName);
    if (pid == 0) return true;  // 本来就没在跑

    HANDLE proc = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (proc != nullptr) {
        TerminateProcess(proc, 1);
        const ULONGLONG now = GetTickCount64();
        const DWORD waitMs = now < deadline ? static_cast<DWORD>(deadline - now) : 0;
        WaitForSingleObject(proc, waitMs);
        CloseHandle(proc);
    }

    for (;;) {
        if (FindProcessId(exeName) == 0) return true;
        if (GetTickCount64() >= deadline) break;
        Sleep(static_cast<DWORD>(kPollMs));
    }

    // 兜底：taskkill /IM <name> /T /F
    const std::wstring cmd = L"taskkill.exe /IM \"" + std::wstring(exeName) + L"\" /T /F";
    DWORD code = 0;
    RunHidden(cmd, 15000, &code);
    for (int i = 0; i < 40; ++i) {
        if (FindProcessId(exeName) == 0) return true;
        Sleep(static_cast<DWORD>(kPollMs));
    }
    return FindProcessId(exeName) == 0;
}

bool RunHidden(const std::wstring& cmdLine, DWORD timeoutMs, DWORD* exitCode) {
    if (cmdLine.empty()) return false;
    if (exitCode != nullptr) *exitCode = static_cast<DWORD>(-1);

    std::vector<wchar_t> cmd(cmdLine.begin(), cmdLine.end());
    cmd.push_back(L'\0');

    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi = {};

    const DWORD flags = CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT;
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, flags, nullptr, nullptr, &si, &pi)) {
        return false;
    }

    bool ok = true;
    const DWORD wait = WaitForSingleObject(pi.hProcess, timeoutMs);
    if (wait == WAIT_OBJECT_0) {
        DWORD code = 0;
        if (GetExitCodeProcess(pi.hProcess, &code) && exitCode != nullptr) *exitCode = code;
    } else {
        TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, 5000);
        ok = false;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return ok;
}

bool ShellOpen(const std::wstring& pathOrUrl) {
    if (pathOrUrl.empty()) return false;
    const HINSTANCE rc = ShellExecuteW(nullptr, L"open", pathOrUrl.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    return reinterpret_cast<INT_PTR>(rc) > 32;
}

bool IsElevated() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    TOKEN_ELEVATION elevation = {};
    DWORD size = 0;
    bool elevated = false;
    if (GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size)) {
        elevated = elevation.TokenIsElevated != 0;
    }
    CloseHandle(token);
    return elevated;
}

bool RelaunchElevated(const std::wstring& args) {
    const std::wstring exe = GetModulePath();
    if (exe.empty()) return false;
    const std::wstring dir = ParentDir(exe);

    SHELLEXECUTEINFOW sei = {};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI;
    sei.lpVerb = L"runas";
    sei.lpFile = exe.c_str();
    sei.lpParameters = args.empty() ? nullptr : args.c_str();
    sei.lpDirectory = dir.empty() ? nullptr : dir.c_str();
    sei.nShow = SW_SHOWNORMAL;

    if (!ShellExecuteExW(&sei)) return false;  // 用户取消 UAC 时为 ERROR_CANCELLED
    if (sei.hProcess != nullptr) CloseHandle(sei.hProcess);
    return true;
}

// ---------------- 注册表 ----------------

bool RegSetString(HKEY root, const std::wstring& subKey, const std::wstring& name, const std::wstring& value) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(root, subKey.c_str(), 0, nullptr, REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr, &key,
                        nullptr) != ERROR_SUCCESS) {
        return false;
    }
    const DWORD bytes = static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t));
    const LONG rc = RegSetValueExW(key, name.empty() ? nullptr : name.c_str(), 0, REG_SZ,
                                   reinterpret_cast<const BYTE*>(value.c_str()), bytes);
    RegCloseKey(key);
    return rc == ERROR_SUCCESS;
}

bool RegSetDword(HKEY root, const std::wstring& subKey, const std::wstring& name, DWORD value) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(root, subKey.c_str(), 0, nullptr, REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr, &key,
                        nullptr) != ERROR_SUCCESS) {
        return false;
    }
    const LONG rc = RegSetValueExW(key, name.empty() ? nullptr : name.c_str(), 0, REG_DWORD,
                                   reinterpret_cast<const BYTE*>(&value), sizeof(value));
    RegCloseKey(key);
    return rc == ERROR_SUCCESS;
}

bool RegGetString(HKEY root, const std::wstring& subKey, const std::wstring& name, std::wstring* value) {
    if (value == nullptr) return false;
    value->clear();
    HKEY key = nullptr;
    if (RegOpenKeyExW(root, subKey.c_str(), 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) return false;

    bool ok = false;
    DWORD type = 0;
    DWORD bytes = 0;
    const wchar_t* valueName = name.empty() ? nullptr : name.c_str();
    LONG rc = RegQueryValueExW(key, valueName, nullptr, &type, nullptr, &bytes);
    if (rc == ERROR_SUCCESS && (type == REG_SZ || type == REG_EXPAND_SZ)) {
        std::vector<wchar_t> buf(bytes / sizeof(wchar_t) + 1, L'\0');
        DWORD got = bytes;
        rc = RegQueryValueExW(key, valueName, nullptr, &type, reinterpret_cast<BYTE*>(buf.data()), &got);
        if (rc == ERROR_SUCCESS) {
            size_t len = static_cast<size_t>(got) / sizeof(wchar_t);
            if (len > 0 && buf[len - 1] == L'\0') --len;
            value->assign(buf.data(), len);
            ok = true;
        }
    }
    RegCloseKey(key);
    return ok;
}

bool RegGetDword(HKEY root, const std::wstring& subKey, const std::wstring& name, DWORD* value) {
    if (value == nullptr) return false;
    HKEY key = nullptr;
    if (RegOpenKeyExW(root, subKey.c_str(), 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) return false;

    bool ok = false;
    DWORD type = 0;
    DWORD data = 0;
    DWORD bytes = static_cast<DWORD>(sizeof(data));
    const LONG rc = RegQueryValueExW(key, name.empty() ? nullptr : name.c_str(), nullptr, &type,
                                     reinterpret_cast<BYTE*>(&data), &bytes);
    if (rc == ERROR_SUCCESS && type == REG_DWORD && bytes == sizeof(DWORD)) {
        *value = data;
        ok = true;
    }
    RegCloseKey(key);
    return ok;
}

bool RegDeleteTree(HKEY root, const std::wstring& subKey) {
    if (subKey.empty()) return false;
    const LONG rc = RegDeleteTreeW(root, subKey.c_str());
    if (rc == ERROR_SUCCESS) return true;
    return rc == ERROR_FILE_NOT_FOUND || rc == ERROR_PATH_NOT_FOUND;
}

// ---------------- 快捷方式 ----------------

bool CreateShortcut(const std::wstring& lnkPath, const std::wstring& target, const std::wstring& args,
                    const std::wstring& workDir, const std::wstring& description) {
    if (lnkPath.empty() || target.empty()) return false;
    if (!EnsureDirForFile(lnkPath)) return false;

    bool initialized = false;
    const HRESULT hr_init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    if (hr_init == S_OK || hr_init == S_FALSE) {
        initialized = true;
    } else if (hr_init != RPC_E_CHANGED_MODE) {
        return false;  // 线程已是 MTA 之外的其他模式（RPC_E_CHANGED_MODE 可继续）
    }

    bool ok = false;
    IShellLinkW* link = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_IShellLinkW,
                                  reinterpret_cast<void**>(&link));
    if (SUCCEEDED(hr) && link != nullptr) {
        link->SetPath(target.c_str());
        if (!args.empty()) link->SetArguments(args.c_str());
        if (!workDir.empty()) link->SetWorkingDirectory(workDir.c_str());
        if (!description.empty()) link->SetDescription(description.c_str());

        IPersistFile* file = nullptr;
        hr = link->QueryInterface(IID_IPersistFile, reinterpret_cast<void**>(&file));
        if (SUCCEEDED(hr) && file != nullptr) {
            ok = SUCCEEDED(file->Save(lnkPath.c_str(), TRUE));
            file->Release();
        }
        link->Release();
    }
    if (initialized) CoUninitialize();
    return ok && FileExists(lnkPath);
}

// ---------------- 杂项 ----------------

uint32_t Crc32Update(uint32_t crc, const void* data, size_t size) {
    const std::array<uint32_t, 256>& table = CrcTable();
    uint32_t c = crc ^ 0xFFFFFFFFu;  // 传入的是已取反的中间结果
    const auto* p = static_cast<const uint8_t*>(data);
    if (p != nullptr) {
        for (size_t i = 0; i < size; ++i) c = table[(c ^ p[i]) & 0xFFu] ^ (c >> 8);
    }
    return c ^ 0xFFFFFFFFu;
}

uint32_t Crc32(const void* data, size_t size) {
    return Crc32Update(0, data, size);
}

bool IsSystemDarkMode() {
    const wchar_t* kPersonalize = L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize";
    DWORD apps = 1;
    if (RegGetDword(HKEY_CURRENT_USER, kPersonalize, L"AppsUseLightTheme", &apps)) return apps == 0;
    DWORD system = 1;
    if (RegGetDword(HKEY_CURRENT_USER, kPersonalize, L"SystemUsesLightTheme", &system)) return system == 0;
    return false;  // 与会话默认一致：读不到时按深色处理
}

std::wstring GetSystemLocaleName() {
    wchar_t buf[LOCALE_NAME_MAX_LENGTH] = {};
    const int n = GetUserDefaultLocaleName(buf, LOCALE_NAME_MAX_LENGTH);
    if (n <= 0) return std::wstring();
    return std::wstring(buf, static_cast<size_t>(n) - 1);  // n 含结尾 NUL
}

ULONGLONG NowMs() {
    return GetTickCount64();
}

}  // namespace fc
