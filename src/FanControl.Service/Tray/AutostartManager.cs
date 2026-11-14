using System.Diagnostics;
using FanControl.Shared.Contracts;

namespace FanControl.Service.Tray;

/// <summary>
/// 开机自启（参考 G-Helper）：通过任务计划程序注册 ONLOGON 任务，托盘菜单可随时开关。
/// 使用 /RL HIGHEST（最高权限）：任务在登录时直接以管理员身份静默启动，
/// 不弹 UAC，保证 ATKACPI/串口等需要管理员权限的读取可用。
/// 注意：创建 HIGHEST 任务要求当前进程已提升（本程序手动启动时即为管理员）。
/// </summary>
public static class AutostartManager
{
    private const string TaskName = "FanControl";

    public static async Task<bool> IsEnabledAsync()
    {
        return await RunSchTasksAsync($"/Query /TN \"{TaskName}\"") == 0;
    }

    /// <summary>
    /// 自启注册的目标程序：优先用启动器 FanControl.exe（与 1.3.0 的任务动作完全一致，
    /// 老版本留下的任务在升级后依然有效），没有启动器时退回守护进程自身。
    /// </summary>
    private static string ResolveLaunchTarget()
    {
        var exe = Environment.ProcessPath
            ?? throw new FanControlException(ErrorInfo.Of("autostart.noProcessPath", "无法获取进程路径。"));

        var directory = Path.GetDirectoryName(exe);
        if (!string.IsNullOrEmpty(directory))
        {
            var launcher = Path.Combine(directory, "FanControl.exe");
            if (File.Exists(launcher))
            {
                return launcher;
            }
        }

        return exe;
    }

    public static async Task EnableAsync()
    {
        var exe = ResolveLaunchTarget();
        var arguments =
            $"/Create /TN \"{TaskName}\" " +
            $"/TR \"\\\"{exe}\\\" --autostart\" " +
            "/SC ONLOGON /RL HIGHEST /F";

        var exitCode = await RunSchTasksAsync(arguments);
        if (exitCode != 0)
        {
            throw new FanControlException(ErrorInfo.Of("autostart.taskFailed", $"创建自启任务失败（schtasks 退出码 {exitCode}）。", exitCode.ToString()));
        }
    }

    public static async Task DisableAsync()
    {
        await RunSchTasksAsync($"/Delete /TN \"{TaskName}\" /F");
    }

    private static async Task<int> RunSchTasksAsync(string arguments)
    {
        using var process = Process.Start(new ProcessStartInfo
        {
            FileName = "schtasks.exe",
            Arguments = arguments,
            UseShellExecute = false,
            CreateNoWindow = true,
            WindowStyle = ProcessWindowStyle.Hidden,
        });

        if (process is null)
        {
            return -1;
        }

        using var timeoutCts = new CancellationTokenSource(TimeSpan.FromSeconds(8));
        try
        {
            await process.WaitForExitAsync(timeoutCts.Token);
            return process.ExitCode;
        }
        catch (OperationCanceledException)
        {
            try
            {
                process.Kill(entireProcessTree: true);
            }
            catch
            {
                // 进程可能已退出
            }

            return -1;
        }
    }
}
