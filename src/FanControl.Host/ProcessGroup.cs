using System.Runtime.InteropServices;

namespace FanControl.Host;

/// <summary>
/// 把守护进程自己放进命名 Job Object Local\FanControl.Group。界面进程由它拉起后自动继承，
/// 之后再拉起的子进程也都在同一组里：守护进程一退出（正常退出、崩溃或被结束），
/// Job 的最后一个句柄关闭，组内所有进程（主要是界面）随之结束，不会留下孤儿窗口。
/// <para>
/// 句柄只在守护进程生命周期内保留，不能提前关闭 —— 提前关闭等于立刻杀死组内进程。
/// 界面侧自己 OpenJobObjectW 加入后必须马上 CloseHandle，否则守护进程退出时那个句柄
/// 会让 Job 继续活着，KILL_ON_JOB_CLOSE 就不会触发。
/// </para>
/// </summary>
internal static class ProcessGroup
{
    /// <summary>组名；界面进程与外部工具用它找到同一个组。</summary>
    public const string Name = @"Local\FanControl.Group";

    private const uint JobObjectLimitKillOnJobClose = 0x2000;
    private const int JobObjectExtendedLimitInformation = 9;

    private static IntPtr _job;

    /// <summary>把自己加入组并持有句柄直到进程退出；失败只记一笔，不影响启动。</summary>
    public static bool Join(Action<string>? logWarning = null)
    {
        if (_job != IntPtr.Zero)
        {
            return true;
        }

        var job = CreateJobObjectW(IntPtr.Zero, Name);
        if (job == IntPtr.Zero)
        {
            logWarning?.Invoke("创建进程组失败：" + Marshal.GetLastWin32Error());
            return false;
        }

        var info = new JobExtendedLimitInfo { BasicLimitInformation = { LimitFlags = JobObjectLimitKillOnJobClose } };
        var size = Marshal.SizeOf<JobExtendedLimitInfo>();
        var buffer = Marshal.AllocHGlobal(size);
        try
        {
            Marshal.StructureToPtr(info, buffer, false);
            if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, buffer, (uint)size))
            {
                logWarning?.Invoke("设置进程组限制失败：" + Marshal.GetLastWin32Error());
            }
        }
        finally
        {
            Marshal.FreeHGlobal(buffer);
        }

        if (!AssignProcessToJobObject(job, GetCurrentProcess()))
        {
            logWarning?.Invoke("加入进程组失败：" + Marshal.GetLastWin32Error());
            CloseHandle(job);
            return false;
        }

        _job = job;
        return true;
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
    private static extern IntPtr CreateJobObjectW(IntPtr lpJobAttributes, string? lpName);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool SetInformationJobObject(IntPtr hJob, int infoClass, IntPtr lpInfo, uint cbInfoLength);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool AssignProcessToJobObject(IntPtr hJob, IntPtr hProcess);

    [DllImport("kernel32.dll")]
    private static extern IntPtr GetCurrentProcess();

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool CloseHandle(IntPtr hObject);
}
