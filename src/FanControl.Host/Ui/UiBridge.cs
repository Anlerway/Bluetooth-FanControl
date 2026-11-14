using System.IO;
using System.Runtime.InteropServices;

namespace FanControl.Host.Ui;

/// <summary>
/// 进程内的 C++/Direct2D 界面（FanControl.UI.dll）。单一进程：没有命名管道，
/// 界面线程由这里启动，快照用 JSON 推进去，界面上的操作通过回调再交回来。
/// </summary>
internal sealed class UiBridge
{
    private const string DllName = "FanControl.UI.dll";

    [UnmanagedFunctionPointer(CallingConvention.StdCall)]
    private delegate void CommandSink(IntPtr utf8Json);

    // 回调必须是静态字段：交给原生的委托一旦被回收，界面下一次点击就会崩。
    private static readonly CommandSink SinkCallback = OnCommandFromUi;

    private static readonly object Gate = new();
    private static Action<string>? _onCommand;
    private static Thread? _thread;

    /// <summary>界面 DLL 是否跟随程序一起发布。</summary>
    public static bool IsAvailable => File.Exists(Path.Combine(AppContext.BaseDirectory, DllName));

    public static bool IsRunning
    {
        get
        {
            lock (Gate)
            {
                return _thread is { IsAlive: true };
            }
        }
    }

    /// <summary>界面发来的命令（JSON 字符串，与 1.4.0 管道载荷同形）。</summary>
    public static void Initialize(Action<string> onCommand) => _onCommand = onCommand;

    /// <summary>界面窗口没开就开一个；已经是开着的直接返回 true。</summary>
    public static bool EnsureStarted()
    {
        lock (Gate)
        {
            if (_thread is { IsAlive: true })
            {
                return true;
            }

            if (!IsAvailable)
            {
                return false;
            }

            FcUiSetCommandSink(SinkCallback);
            _thread = new Thread(RunUiThread)
            {
                IsBackground = true,
                Name = "FanControl.UI",
            };
            _thread.SetApartmentState(ApartmentState.STA);
            _thread.Start();
            return true;
        }
    }

    /// <summary>把已经存在的窗口叫到前台。</summary>
    public static void Activate()
    {
        if (IsRunning)
        {
            FcUiActivate();
        }
    }

    /// <summary>推一帧状态/配置 JSON。界面没开时直接丢弃。</summary>
    public static void Push(string json)
    {
        if (json.Length == 0 || !IsRunning)
        {
            return;
        }

        var pointer = Marshal.StringToCoTaskMemUTF8(json);
        try
        {
            FcUiPush(pointer);
        }
        finally
        {
            Marshal.FreeCoTaskMem(pointer);
        }
    }

    /// <summary>关闭界面窗口（进程退出前调用）。</summary>
    public static void Shutdown()
    {
        Thread? thread;
        lock (Gate)
        {
            thread = _thread;
        }

        if (thread is null)
        {
            return;
        }

        FcUiQuit();
        thread.Join(TimeSpan.FromSeconds(3));
    }

    private static void RunUiThread()
    {
        try
        {
            FcUiRun();
        }
        catch (Exception ex)
        {
            System.Diagnostics.Trace.WriteLine("界面线程异常退出：" + ex);
        }
    }

    private static void OnCommandFromUi(IntPtr utf8Json)
    {
        if (utf8Json == IntPtr.Zero)
        {
            return;
        }

        var json = Marshal.PtrToStringUTF8(utf8Json);
        if (string.IsNullOrEmpty(json))
        {
            return;
        }

        // 回调跑在界面线程：立刻转到线程池，别让配置落盘/扫描设备堵住绘制。
        var handler = _onCommand;
        if (handler is null)
        {
            return;
        }

        _ = Task.Run(() => handler(json));
    }

    [DllImport(DllName, CallingConvention = CallingConvention.StdCall)]
    private static extern int FcUiRun();

    [DllImport(DllName, CallingConvention = CallingConvention.StdCall)]
    private static extern void FcUiActivate();

    [DllImport(DllName, CallingConvention = CallingConvention.StdCall)]
    private static extern void FcUiPush(IntPtr utf8Json);

    [DllImport(DllName, CallingConvention = CallingConvention.StdCall)]
    private static extern void FcUiQuit();

    [DllImport(DllName, CallingConvention = CallingConvention.StdCall)]
    private static extern void FcUiSetCommandSink(CommandSink sink);
}
