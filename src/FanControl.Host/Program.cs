using System.Diagnostics;
using System.Runtime.InteropServices;
using FanControl.Host.Ui;
using FanControl.Service.Config;
using FanControl.Service.Host;
using FanControl.Service.Tray;
using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Logging;
using Microsoft.Extensions.Logging.Abstractions;

namespace FanControl.Host;

/// <summary>
/// 单一进程的 FanControl：硬件采样 + 风扇控制 + 托盘 + 配置落盘都在这里，
/// 界面是进程内加载的 FanControl.UI.dll（C++/Direct2D），没有命名管道也没有第二个进程。
/// 关掉窗口只是销毁窗口，托盘与采样继续；托盘菜单「打开主界面」会把窗口重新建出来。
/// </summary>
internal static class Program
{
    private const string MutexName = @"Local\FanControl";
    private const string ShowWindowEventName = @"Local\FanControl.ShowWindow";

    // --page=N 让界面第一次打开时停在指定页：截图脚本与自动化测试用，日常启动不带这个参数。
    private static int _initialPage = -1;

    // 退出路径上出的异常不该冒充「启动失败」弹框：进程已经在收尾，弹出来只会挡住用户
    private static volatile bool _shuttingDown;
    // 退出阶段 DI 容器已经拆了，只能用提前留好的 logger 把异常写进日志
    private static Microsoft.Extensions.Logging.ILogger? _logger;

    [STAThread]
    private static int Main(string[] args)
    {
        // 与界面 DLL 中创建的窗口同一个 AppUserModelID：任务栏只有一个按钮。
        SetCurrentProcessExplicitAppUserModelID("FanControl");

        // 自动化测试（截图脚本）会把 FANCONTROL_INSTANCE 设成一个标记，让互斥体与事件各带后缀，
        // 这样在用户已经开着 FanControl 的机器上也能再起一个实例截图，互不干扰。
        var instanceTag = Environment.GetEnvironmentVariable("FANCONTROL_INSTANCE");
        var suffix = string.IsNullOrWhiteSpace(instanceTag) ? string.Empty : "." + instanceTag.Trim();

        using var mutex = new Mutex(true, MutexName + suffix, out var isFirstInstance);
        if (!isFirstInstance)
        {
            // 已经在跑：请现有实例把窗口叫到前台，然后退出。
            if (EventWaitHandle.TryOpenExisting(ShowWindowEventName + suffix, out var existing))
            {
                using (existing)
                {
                    existing.Set();
                }
            }

            return 0;
        }

        // 事件要在耗时初始化之前创建：第二个实例可能在界面还没就绪时就发来信号，
        // AutoReset 事件会把这次信号保留到界面线程开始等待。
        using var showEvent = new EventWaitHandle(false, EventResetMode.AutoReset, ShowWindowEventName + suffix);

        try
        {
            return RunAsync(args, showEvent).GetAwaiter().GetResult();
        }
        catch (Exception ex)
        {
            Trace.WriteLine(ex);
            if (_shuttingDown)
            {
                // 退出时才炸的（大多是释放阶段的对象已释放）：只记日志，不弹框
                try
                {
                    _logger?.LogWarning(ex, "退出时出错。");
                }
                catch (Exception)
                {
                }

                return 1;
            }

            MessageBoxW(IntPtr.Zero, "FanControl 启动失败：" + ex.Message, "FanControl", 0x00000010);
            return 1;
        }
    }

    private static async Task<int> RunAsync(string[] args, EventWaitHandle showEvent)
    {
        foreach (var argument in args)
        {
            if (argument.StartsWith("--page=", StringComparison.OrdinalIgnoreCase)
                && int.TryParse(argument.AsSpan(7), out var page)
                && page is >= 0 and <= 3)
            {
                _initialPage = page;
            }
        }

        // 日志目录取决于 system.json，所以先用一个最小 ConfigManager 把它读出来，
        // 再按它的结果装配完整的服务容器（与 1.3.0 的启动顺序一致）。
        var earlyConfigManager = new ConfigManager(NullLogger<ConfigManager>.Instance);
        var systemConfig = await earlyConfigManager.LoadSystemConfigAsync().ConfigureAwait(false);
        var logDirectory = earlyConfigManager.GetLogDirectory(systemConfig);

        await using var provider = FanControlServices.BuildFanControlProvider(
            logDirectory,
            systemConfig.LogEnabled,
            systemConfig.MaxLogFiles);

        var runtime = provider.GetRequiredService<FanControlRuntime>();
        var logger = provider.GetRequiredService<ILoggerFactory>().CreateLogger("FanControl.Host");
        _logger = logger;

        using var shutdown = new CancellationTokenSource();
        using var tray = new TrayHost();

        await runtime.StartAsync().ConfigureAwait(false);

        var systemSettings = new SystemSettings(earlyConfigManager, systemConfig);
        await using var router = new CommandRouter(runtime, systemSettings, logger);

        // 界面上的动作从这里进来（已经在线程池上），状态由 router 每 400 ms 推回去。
        UiBridge.Initialize(json => _ = Task.Run(() => router.HandleAsync(json)));

        tray.Start(
            showUi: ShowUi,
            exit: () => shutdown.Cancel(),
            reconnect: runtime.RequestReconnect);

        TrayStrings.SetLanguage(runtime.CurrentConfig.Language);
        UpdateTray(tray, runtime);

        // 界面或托盘切换语言都会落到配置里，配置变了就同步托盘文案。
        runtime.ConfigChanged += (_, _) =>
        {
            TrayStrings.SetLanguage(runtime.CurrentConfig.Language);
            UpdateTray(tray, runtime);
        };

        runtime.ConnectionStateChanged += (_, _) => UpdateTray(tray, runtime);
        runtime.BleConnectionChanged += (_, connected) =>
        {
            UpdateTray(tray, runtime);
            if (!connected && runtime.CurrentConfig.NotifyOnBleDisconnect)
            {
                tray.ShowNotification(TrayStrings.AppTitle, TrayStrings.Disconnected);
            }
        };
        runtime.TemperatureErrorOccurred += (_, _) =>
        {
            if (runtime.CurrentConfig.NotifyOnTemperatureError)
            {
                tray.ShowNotification(TrayStrings.TemperatureErrorTitle, runtime.LastError ?? TrayStrings.TemperatureErrorFallback);
            }
        };

        logger.LogInformation("FanControl 已启动（单进程），日志目录 {LogDirectory}", logDirectory);

        router.Start();

        // 诊断用：FANCONTROL_MEMLOG=1 每 30 秒记一次托管堆/私有内存/工作集，用来区分托管还是原生泄漏。
        if (Environment.GetEnvironmentVariable("FANCONTROL_MEMLOG") == "1")
        {
            _ = Task.Run(async () =>
            {
                while (true)
                {
                    await Task.Delay(TimeSpan.FromSeconds(10)).ConfigureAwait(false);
                    try
                    {
                        using var self = System.Diagnostics.Process.GetCurrentProcess();
                        System.IO.File.AppendAllText(
                            System.IO.Path.Combine(System.IO.Path.GetTempPath(), "fc-mem.log"),
                            string.Format(
                                System.Globalization.CultureInfo.InvariantCulture,
                                "managed={0:F1} heap={1:F1} priv={2:F1} ws={3:F1} alloc={4:F1} gc={5}/{6}/{7} threads={8}\n",
                                GC.GetTotalMemory(false) / 1048576.0,
                                GC.GetGCMemoryInfo().HeapSizeBytes / 1048576.0,
                                self.PrivateMemorySize64 / 1048576.0,
                                self.WorkingSet64 / 1048576.0,
                                GC.GetTotalAllocatedBytes() / 1048576.0,
                                GC.CollectionCount(0), GC.CollectionCount(1), GC.CollectionCount(2),
                                self.Threads.Count));
                    }
                    catch (Exception)
                    {
                    }
                }
            });
        }

        // 自动化测试用：FANCONTROL_EXIT_AFTER=秒 走一遍与托盘「退出」完全相同的收尾路径。
        var exitAfter = Environment.GetEnvironmentVariable("FANCONTROL_EXIT_AFTER");
        if (int.TryParse(exitAfter, out var exitSeconds) && exitSeconds > 0)
        {
            _ = Task.Run(async () =>
            {
                await Task.Delay(TimeSpan.FromSeconds(exitSeconds)).ConfigureAwait(false);
                shutdown.Cancel();
            });
        }

        // --autostart 是计划任务用的：只留托盘，不弹窗口。
        var autostart = args.Any(argument => string.Equals(argument, "--autostart", StringComparison.OrdinalIgnoreCase));
        if (!autostart)
        {
            ShowUi();
        }

        var showThread = new Thread(() =>
        {
            while (!shutdown.IsCancellationRequested)
            {
                if (showEvent.WaitOne(500))
                {
                    ShowUi();
                }
            }
        })
        {
            IsBackground = true,
            Name = "FanControl.ShowWindow",
        };
        showThread.Start();

        try
        {
            await Task.Delay(Timeout.Infinite, shutdown.Token).ConfigureAwait(false);
        }
        catch (OperationCanceledException)
        {
        }

        _shuttingDown = true;

        try
        {
            // 先关界面（界面线程在 DLL 里跑消息循环）。运行时由上面的 provider 负责释放：
            // 再手动 DisposeAsync 一次会让 CancellationTokenSource 被二次取消而抛
            // ObjectDisposedException，那条异常会被 Main 的 catch 当成「启动失败」弹框。
            UiBridge.Shutdown();
        }
        catch (Exception ex)
        {
            // 退出路径的异常不该冒充启动失败：界面线程没回收就让它随进程一起走。
            logger.LogWarning(ex, "关闭界面时出错。");
        }

        logger.LogInformation("FanControl 退出。");
        return 0;
    }

    /// <summary>打开界面：窗口没建过就建，已经开着就唤到前台。托盘与第二个实例都走这里。</summary>
    private static void ShowUi()
    {
        if (!UiBridge.IsAvailable)
        {
            MessageBoxW(IntPtr.Zero, "找不到 FanControl.UI.dll，请重新安装。", "FanControl", 0x00000010);
            return;
        }

        // 初始页只在第一次建窗口时生效，之后由用户在窗口里切换。
        if (_initialPage >= 0)
        {
            UiBridge.SetInitialPage(_initialPage);
            _initialPage = -1;
        }

        if (UiBridge.EnsureStarted())
        {
            UiBridge.Activate();

            // 界面刚起来的十几秒里，DWrite/D2D 与采样栈会把托管堆顶到十几 MB 才触发第一次回收，
            // 任务管理器里看到的私有内存就一路往上爬。等界面热起来整一次代 2 回收，
            // 垃圾当场还给系统，后面每次只回收新垃圾，数字就稳住不动了。
            _ = Task.Run(async () =>
            {
                await Task.Delay(TimeSpan.FromSeconds(15)).ConfigureAwait(false);
                // Optimized 会把这次回收当成「可能不划算」直接跳过，这里要的是必做
                GC.Collect(GC.MaxGeneration, GCCollectionMode.Forced, blocking: true, compacting: true);
            });
        }
    }

    private static void UpdateTray(TrayHost tray, FanControlRuntime runtime)
    {
        var state = TrayStrings.State(runtime.ConnectionState);

        var packet = runtime.LatestPacket;
        var suffix = packet is null
            ? string.Empty
            : $" · {packet.CpuTemperatureCelsius:F0}°C / {packet.PwmPercent:F0}%";

        tray.SetStatus("FanControl · " + state + suffix);
    }

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    private static extern int MessageBoxW(IntPtr hWnd, string lpText, string lpCaption, uint uType);

    [DllImport("shell32.dll", CharSet = CharSet.Unicode, PreserveSig = false)]
    private static extern void SetCurrentProcessExplicitAppUserModelID(string appId);
}
