using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Threading;
using FanControl.Host.Ipc;
using FanControl.Service.Config;
using FanControl.Service.Host;
using FanControl.Service.Tray;
using FanControl.Shared.Enums;
using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Logging;
using Microsoft.Extensions.Logging.Abstractions;

namespace FanControl.Host;

/// <summary>
/// 常驻守护进程：硬件采样 + 风扇控制 + 托盘 + 命名管道服务端。
/// 界面（FanControl.UI.exe，C++）按需启动，通过命名管道读状态、下发配置；
/// 托盘、开机自启、断线重连都属于这里 —— 关掉界面，风扇仍在受控。
/// </summary>
internal static class Program
{
    private const string MutexName = @"Local\FanControl.Host";
    private const string ShowWindowEventName = @"Local\FanControl.ShowWindow";

    [STAThread]
    private static int Main(string[] args)
    {
        using var mutex = new Mutex(true, MutexName, out var isFirstInstance);
        if (!isFirstInstance)
        {
            // 已有守护进程在跑：请求它把界面唤到前台，然后退出。
            if (EventWaitHandle.TryOpenExisting(ShowWindowEventName, out var existing))
            {
                using (existing)
                {
                    existing.Set();
                }
            }

            return 0;
        }

        // 事件要在耗时初始化之前就创建：启动器（FanControl.exe）可能在守护进程还没就绪时
        // 就发来“显示界面”的信号，AutoReset 事件会把这次信号保留到界面线程开始等待。
        using var showEvent = new EventWaitHandle(false, EventResetMode.AutoReset, ShowWindowEventName);

        try
        {
            return RunAsync(args, showEvent).GetAwaiter().GetResult();
        }
        catch (Exception ex)
        {
            Trace.WriteLine(ex);
            MessageBoxW(IntPtr.Zero, "FanControl 守护进程启动失败：" + ex.Message, "FanControl", 0x00000010);
            return 1;
        }
    }

    private static async Task<int> RunAsync(string[] args, EventWaitHandle showEvent)
    {
        // 日志目录取决于 system.json，所以先用一个最小 ConfigManager 把它读出来，
        // 再按它的结果装配完整的服务容器（与旧版 UI 启动顺序一致）。
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
        using var launcher = new UiLauncher(provider.GetRequiredService<ILogger<UiLauncher>>());
        using var tray = new TrayHost();

        await runtime.StartAsync().ConfigureAwait(false);

        tray.Start(
            showUi: launcher.Show,
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

        logger.LogInformation("守护进程已启动，日志目录 {LogDirectory}", logDirectory);

        var systemSettings = new SystemSettings(earlyConfigManager, systemConfig);
        await using var ipc = new IpcServer(runtime, systemSettings, launcher.Show, logger);
        ipc.Start();

        var showThread = new Thread(() =>
        {
            while (!shutdown.IsCancellationRequested)
            {
                if (showEvent.WaitOne(500))
                {
                    launcher.Show();
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

        logger.LogInformation("守护进程退出。");
        await runtime.DisposeAsync().ConfigureAwait(false);
        return 0;
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
}
