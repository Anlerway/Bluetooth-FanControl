using System.ComponentModel;
using System.Runtime.InteropServices;
using Microsoft.Extensions.Logging;

namespace FanControl.Host;

/// <summary>
/// 按需拉起 C++ 界面进程，并把它挂进守护进程的 Job Object：
/// 守护进程退出（或被任务管理器结束）时，界面进程随之结束，不会残留孤儿窗口。
/// </summary>
internal sealed class UiLauncher : IDisposable
{
    private const string UiWindowClass = "FanControlUI";
    private const string UiFileName = "FanControl.UI.exe";

    private const uint JobObjectLimitKillOnJobClose = 0x2000;
    private const int JobObjectExtendedLimitInformation = 9;
    private const uint CreateNoWindow = 0x08000000;
    private const uint GenericRead = 0x80000000;
    private const int SwRestore = 9;

    private readonly ILogger<UiLauncher> _logger;
    private readonly object _gate = new();
    private IntPtr _job;

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

        try
        {
            var job = EnsureJob();
            if (job != IntPtr.Zero && !AssignProcessToJobObject(job, info.hProcess))
            {
                _logger.LogWarning("界面进程未能加入 Job Object：{Error}", Marshal.GetLastWin32Error());
            }
        }
        finally
        {
            CloseHandle(info.hThread);
            CloseHandle(info.hProcess);
        }
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

    private IntPtr EnsureJob()
    {
        if (_job != IntPtr.Zero)
        {
            return _job;
        }

        _job = CreateJobObjectW(IntPtr.Zero, null);
        if (_job == IntPtr.Zero)
        {
            _logger.LogWarning("创建 Job Object 失败：{Error}", Marshal.GetLastWin32Error());
            return IntPtr.Zero;
        }

        var info = new JobExtendedLimitInfo { BasicLimitInformation = { LimitFlags = JobObjectLimitKillOnJobClose } };
        var size = Marshal.SizeOf<JobExtendedLimitInfo>();
        var buffer = Marshal.AllocHGlobal(size);
        try
        {
            Marshal.StructureToPtr(info, buffer, false);
            if (!SetInformationJobObject(_job, JobObjectExtendedLimitInformation, buffer, (uint)size))
            {
                _logger.LogWarning("设置 Job Object 限制失败：{Error}", Marshal.GetLastWin32Error());
            }
        }
        finally
        {
            Marshal.FreeHGlobal(buffer);
        }

        return _job;
    }

    public void Dispose()
    {
        lock (_gate)
        {
            if (_job != IntPtr.Zero)
            {
                CloseHandle(_job);
                _job = IntPtr.Zero;
            }
        }
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

    [StructLayout(LayoutKind.Sequential)]
    private struct JobObjectBasicLimitInformation
    {
        public long PerProcessUserTimeLimit;
        public long PerJobUserTimeLimit;
        public uint LimitFlags;
        public UIntPtr MinimumWorkingSetSize;
        public UIntPtr MaximumWorkingSetSize;
        public uint ActiveProcessLimit;
        public UIntPtr Affinity;
        public uint PriorityClass;
        public uint SchedulingClass;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct IoCounters
    {
        public ulong ReadOperationCount;
        public ulong WriteOperationCount;
        public ulong OtherOperationCount;
        public ulong ReadTransferCount;
        public ulong WriteTransferCount;
        public ulong OtherTransferCount;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct JobExtendedLimitInfo
    {
        public JobObjectBasicLimitInformation BasicLimitInformation;
        public IoCounters IoInfo;
        public UIntPtr ProcessMemoryLimit;
        public UIntPtr JobMemoryLimit;
        public UIntPtr PeakProcessMemoryUsed;
        public UIntPtr PeakJobMemoryUsed;
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
    private static extern IntPtr CreateJobObjectW(IntPtr lpJobAttributes, string? lpName);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool SetInformationJobObject(IntPtr hJob, int infoClass, IntPtr lpInfo, uint cbInfoLength);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool AssignProcessToJobObject(IntPtr hJob, IntPtr hProcess);

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
