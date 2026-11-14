using System.ComponentModel;
using System.Runtime.InteropServices;
using Microsoft.Extensions.Logging;

namespace FanControl.Host;

/// <summary>
/// 按需拉起 C++ 界面进程。界面是本进程的子进程，启动即继承进程组
/// （<see cref="ProcessGroup"/>，Local\FanControl.Group），守护进程退出时随组结束，
/// 不会残留孤儿窗口。
/// </summary>
internal sealed class UiLauncher : IDisposable
{
    private const string UiWindowClass = "FanControlUI";
    private const string UiFileName = "FanControl.UI.exe";

    private const uint CreateNoWindow = 0x08000000;
    private const int SwRestore = 9;

    private readonly ILogger<UiLauncher> _logger;
    private readonly object _gate = new();

    public UiLauncher(ILogger<UiLauncher> logger)
    {
        _logger = logger;
    }

    /// <summary>把界面带到前台；没有在跑的界面就新起一个。</summary>
    public void Show()
    {
        lock (_gate)
        {
            try
            {
                var existing = FindWindowW(UiWindowClass, null);
                if (existing != IntPtr.Zero)
                {
                    ShowWindow(existing, SwRestore);
                    SetForegroundWindow(existing);
                    return;
                }

                Launch();
            }
            catch (Exception ex)
            {
                _logger.LogError(ex, "启动界面进程失败。");
                MessageBoxW(IntPtr.Zero, "无法启动界面进程：" + ex.Message, "FanControl", 0x00000030);
            }
        }
    }

    private void Launch()
    {
        var path = ResolveUiPath();
        if (path is null)
        {
            MessageBoxW(IntPtr.Zero, "找不到 " + UiFileName + "，请重新安装 FanControl。", "FanControl", 0x00000030);
            return;
        }

        var startup = new StartupInfo { cb = Marshal.SizeOf<StartupInfo>() };
        var commandLine = "\"" + path + "\" --spawned";
        if (!CreateProcessW(path, commandLine, IntPtr.Zero, IntPtr.Zero, false, CreateNoWindow, IntPtr.Zero, Path.GetDirectoryName(path), ref startup, out var info))
        {
            var error = Marshal.GetLastWin32Error();
            throw new Win32Exception(error, "CreateProcess 失败。");
        }

        // 子进程自动继承进程组，这里只需要关掉句柄。
        CloseHandle(info.hThread);
        CloseHandle(info.hProcess);
    }

    private static string? ResolveUiPath()
    {
        var local = Path.Combine(AppContext.BaseDirectory, UiFileName);
        if (File.Exists(local))
        {
            return local;
        }

        var sibling = Path.GetFullPath(Path.Combine(AppContext.BaseDirectory, "..", "FanControl.UI", UiFileName));
        return File.Exists(sibling) ? sibling : null;
    }

    public void Dispose()
    {
    }

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    private struct StartupInfo
    {
        public int cb;
        public string? lpReserved;
        public string? lpDesktop;
        public string? lpTitle;
        public int dwX;
        public int dwY;
        public int dwXSize;
        public int dwYSize;
        public int dwXCountChars;
        public int dwYCountChars;
        public int dwFillAttribute;
        public int dwFlags;
        public short wShowWindow;
        public short cbReserved2;
        public IntPtr lpReserved2;
        public IntPtr hStdInput;
        public IntPtr hStdOutput;
        public IntPtr hStdError;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct ProcessInformation
    {
        public IntPtr hProcess;
        public IntPtr hThread;
        public int dwProcessId;
        public int dwThreadId;
    }

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern bool CreateProcessW(
        string lpApplicationName,
        string lpCommandLine,
        IntPtr lpProcessAttributes,
        IntPtr lpThreadAttributes,
        bool bInheritHandles,
        uint dwCreationFlags,
        IntPtr lpEnvironment,
        string? lpCurrentDirectory,
        ref StartupInfo lpStartupInfo,
        out ProcessInformation lpProcessInformation);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool CloseHandle(IntPtr hObject);

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    private static extern IntPtr FindWindowW(string? lpClassName, string? lpWindowName);

    [DllImport("user32.dll")]
    private static extern bool ShowWindow(IntPtr hWnd, int nCmdShow);

    [DllImport("user32.dll")]
    private static extern bool SetForegroundWindow(IntPtr hWnd);

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    private static extern int MessageBoxW(IntPtr hWnd, string lpText, string lpCaption, uint uType);
}
