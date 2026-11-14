using System.Runtime.InteropServices;

namespace FanControl.Host.Ui;

/// <summary>
/// 把进程工作集还给系统。界面窗口关掉后进程仍住托盘，这时 D2D 的位图和界面堆页
/// 都还能被换出，不主动 Trim 会让任务管理器一直显示几十 MB。
/// </summary>
internal static class MemoryTrim
{
    [DllImport("kernel32.dll")]
    private static extern IntPtr GetCurrentProcess();

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool SetProcessWorkingSetSize(IntPtr process, IntPtr min, IntPtr max);

    public static void Run()
    {
        // -1 会让系统把尽可能多的页换出，代价是下次显式分配要再换回来。
        SetProcessWorkingSetSize(GetCurrentProcess(), new IntPtr(-1), new IntPtr(-1));
    }
}