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

    [STAThread]
    private static int Main(string[] args)
    {
        // 与界面 DLL 中创建的窗口同一个 AppUserModelID：任务栏只有一个按钮。
        SetCurrentProcessExplicitAppUserModelID("FanControl");

        using var mutex = new Mutex(true, MutexName, out var isFirstInstance);
        if (!isFirstInstance)
        {
            // 已经在跑：请现有实例把窗口叫到前台，然后退出。
            if (EventWaitHandle.TryOpenExisting(ShowWindowEventName, out var existing))
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
        using var showEvent = new EventWaitHandle(false, EventResetMode.AutoReset, ShowWindowEventName);

        try
        {
            return RunAsync(args, showEvent).GetAwaiter().GetResult();
        }
        catch (Exception ex)
        {
            Trace.WriteLine(ex);
            MessageBoxW(IntPtr.Zero, "FanControl 启动失败：" + ex.Message, "FanControl", 0x00000010);
            return 1;
        }
    }

    private static async Task<int> RunAsync(string[] args, EventWaitHandle showEvent)
    {
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

        if (UiBridge.EnsureStarted())
        {
            UiBridge.Activate();
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
