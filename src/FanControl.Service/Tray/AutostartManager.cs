using System.Diagnostics;
using System.Security.Principal;
using System.Text;
using FanControl.Shared.Contracts;
using Microsoft.Win32;

namespace FanControl.Service.Tray;

/// <summary>
/// 开机自启（参考 G-Helper）：通过任务计划程序注册 ONLOGON 任务，托盘菜单可随时开关。
/// 使用 /RL HIGHEST（最高权限）：任务在登录时直接以管理员身份静默启动，
/// 不弹 UAC，保证 ATKACPI/串口等需要管理员权限的读取可用。
/// 注意：创建 HIGHEST 任务要求当前进程已提升（本程序手动启动时即为管理员）。
///
/// 为什么不是一行 schtasks /Create /TR：那条命令建出来的任务一律带
/// &lt;DisallowStartIfOnBatteries&gt;true&lt;/DisallowStartIfOnBatteries&gt;，
/// 笔记本用电池时登录根本不会触发，用户看到的就是"开了自启但没自启"。
/// 所以这里写完整任务 XML（关掉电池条件、带上工作目录与运行身份），与安装器创建的任务完全一致。
/// </summary>
public static class AutostartManager
{
    private const string TaskName = "FanControl";
    private const string RunKeyPath = @"Software\Microsoft\Windows\CurrentVersion\Run";
    private const string RunValueName = "FanControl";

    /// <summary>自启状态变化：托盘菜单、设置页、安装器三处共用同一份真值。</summary>
    public static event Action<bool>? StateChanged;

    public static async Task<bool> IsEnabledAsync()
    {
        if (await RunSchTasksAsync($"/Query /TN \"{TaskName}\"") == 0)
        {
            return true;
        }

        // 安装器在任务建不出来时会退到 HKCU\...\Run，这里一并认作已启用
        return RunValueExists();
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
        await EnableCoreAsync().ConfigureAwait(false);
        StateChanged?.Invoke(true);
    }

    private static async Task EnableCoreAsync()
    {
        var exe = ResolveLaunchTarget();

        if (await TryCreateTaskAsync(exe))
        {
            return;
        }

        // 兜底：/TR 写法。能建出任务，但 schtasks 会给它加上"电池供电时不启动"，笔记本上可能不触发。
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
        await RunSchTasksAsync($"/Delete /TN \"{TaskName}\" /F").ConfigureAwait(false);
        DeleteRunValue();
        StateChanged?.Invoke(false);
    }

    private static async Task<bool> TryCreateTaskAsync(string exe)
    {
        var directory = Path.GetDirectoryName(exe) ?? string.Empty;
        string sid;
        try
        {
            sid = WindowsIdentity.GetCurrent().User?.Value ?? string.Empty;
        }
        catch (Exception)
        {
            sid = string.Empty;
        }

        var xmlPath = Path.Combine(Path.GetTempPath(), $"FanControl-task-{Environment.ProcessId}.xml");
        try
        {
            File.WriteAllText(xmlPath, BuildTaskXml(exe, directory, sid), new UnicodeEncoding(false, true));
            return await RunSchTasksAsync($"/Create /TN \"{TaskName}\" /XML \"{xmlPath}\" /F") == 0;
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException or NotSupportedException)
        {
            return false;
        }
        finally
        {
            try
            {
                File.Delete(xmlPath);
            }
            catch (Exception)
            {
                // 临时文件删不掉不影响结果
            }
        }
    }

    private static string BuildTaskXml(string exe, string workingDirectory, string sid)
    {
        var principal = sid.Length == 0 ? string.Empty : $"      <UserId>{sid}</UserId>";
        return $"""
<?xml version="1.0" encoding="UTF-16"?>
<Task version="1.2" xmlns="http://schemas.microsoft.com/windows/2004/02/mit/task">
  <RegistrationInfo>
    <Author>FanControl</Author>
    <Description>FanControl autostart</Description>
  </RegistrationInfo>
  <Triggers>
    <LogonTrigger>
      <Enabled>true</Enabled>
    </LogonTrigger>
  </Triggers>
  <Principals>
    <Principal id="Author">
{principal}
      <LogonType>InteractiveToken</LogonType>
      <RunLevel>HighestAvailable</RunLevel>
    </Principal>
  </Principals>
  <Settings>
    <MultipleInstancesPolicy>IgnoreNew</MultipleInstancesPolicy>
    <DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries>
    <StopIfGoingOnBatteries>false</StopIfGoingOnBatteries>
    <AllowHardTerminate>true</AllowHardTerminate>
    <StartWhenAvailable>true</StartWhenAvailable>
    <RunOnlyIfNetworkAvailable>false</RunOnlyIfNetworkAvailable>
    <IdleSettings>
      <StopOnIdleEnd>false</StopOnIdleEnd>
      <RestartOnIdle>false</RestartOnIdle>
    </IdleSettings>
    <AllowStartOnDemand>true</AllowStartOnDemand>
    <Enabled>true</Enabled>
    <Hidden>false</Hidden>
    <RunOnlyIfIdle>false</RunOnlyIfIdle>
    <WakeToRun>false</WakeToRun>
    <ExecutionTimeLimit>PT0S</ExecutionTimeLimit>
    <Priority>7</Priority>
  </Settings>
  <Actions Context="Author">
    <Exec>
      <Command>{System.Security.SecurityElement.Escape(exe)}</Command>
      <Arguments>--autostart</Arguments>
      <WorkingDirectory>{System.Security.SecurityElement.Escape(workingDirectory)}</WorkingDirectory>
    </Exec>
  </Actions>
</Task>
""";
    }

    private static bool RunValueExists()
    {
        try
        {
            using var key = Registry.CurrentUser.OpenSubKey(RunKeyPath);
            return key?.GetValue(RunValueName) is string value && value.Length > 0;
        }
        catch (Exception)
        {
            return false;
        }
    }

    private static void DeleteRunValue()
    {
        try
        {
            using var key = Registry.CurrentUser.OpenSubKey(RunKeyPath, writable: true);
            key?.DeleteValue(RunValueName, throwOnMissingValue: false);
        }
        catch (Exception)
        {
            // 没有该值或没有权限时忽略
        }
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
