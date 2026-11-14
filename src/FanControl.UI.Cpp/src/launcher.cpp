// FanControl.exe —— 管理员启动器。
// 双击时先确保守护进程（FanControl.Host.exe）在跑，再请它把界面唤到前台；
// 带 --autostart 时只拉守护进程（开机自启任务的动作与 1.3.0 完全一致）。
// 启动器本身不做任何硬件/界面工作，进程在执行完动作后立即退出。

#include <windows.h>

#include <string>

namespace
{
const wchar_t* kHostMutexName = L"Local\\FanControl.Host";
const wchar_t* kShowWindowEventName = L"Local\\FanControl.ShowWindow";
const wchar_t* kHostFileName = L"FanControl.Host.exe";
const wchar_t* kUiFileName = L"FanControl.UI.exe";

std::wstring ModuleDirectory()
{
    wchar_t path[MAX_PATH * 2];
    const DWORD length = GetModuleFileNameW(nullptr, path, ARRAYSIZE(path));
    if (length == 0)
    {
        return std::wstring();
    }

    const std::wstring full(path, length);
    const size_t slash = full.find_last_of(L'\\');
    return slash == std::wstring::npos ? std::wstring() : full.substr(0, slash);
}

bool FileExists(const std::wstring& path)
{
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

bool StartProgram(const std::wstring& path)
{
    std::wstring command = L"\"" + path + L"\"";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(path.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process))
    {
        return false;
    }

    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return true;
}

bool HostIsRunning()
{
    HANDLE mutex = OpenMutexW(SYNCHRONIZE, FALSE, kHostMutexName);
    if (mutex == nullptr)
    {
        return false;
    }

    CloseHandle(mutex);
    return true;
}

// 守护进程启动要初始化硬件传感器（数秒），这里只等它把事件建出来，最多 30 秒。
bool RequestShowWindow()
{
    for (int attempt = 0; attempt < 300; ++attempt)
    {
        HANDLE showEvent = OpenEventW(EVENT_MODIFY_STATE, FALSE, kShowWindowEventName);
        if (showEvent != nullptr)
        {
            SetEvent(showEvent);
            CloseHandle(showEvent);
            return true;
        }

        Sleep(100);
    }

    return false;
}

int Fail(const wchar_t* text)
{
    MessageBoxW(nullptr, text, L"FanControl", MB_OK | MB_ICONERROR);
    return 2;
}
}  // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR commandLine, int)
{
    const bool autostart = commandLine != nullptr && wcsstr(commandLine, L"--autostart") != nullptr;
    const std::wstring directory = ModuleDirectory();
    if (directory.empty())
    {
        return Fail(L"无法确定程序所在目录。");
    }

    const std::wstring hostPath = directory + L"\\" + kHostFileName;
    if (!FileExists(hostPath))
    {
        return Fail(L"找不到 FanControl.Host.exe，请重新安装 FanControl。");
    }

    if (!HostIsRunning() && !StartProgram(hostPath))
    {
        return Fail(L"无法启动 FanControl.Host.exe。");
    }

    if (autostart)
    {
        // 开机自启只要守护进程（图标常驻托盘），不弹界面。
        return 0;
    }

    if (RequestShowWindow())
    {
        return 0;
    }

    // 守护进程没把事件建出来（例如初始化失败）：直接把界面拉起来，界面会自己重试连接。
    const std::wstring uiPath = directory + L"\\" + kUiFileName;
    if (!FileExists(uiPath))
    {
        return Fail(L"找不到 FanControl.UI.exe，请重新安装 FanControl。");
    }

    return StartProgram(uiPath) ? 0 : Fail(L"无法启动 FanControl.UI.exe。");
}
