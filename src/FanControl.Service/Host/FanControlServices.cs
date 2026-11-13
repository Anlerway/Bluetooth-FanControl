using FanControl.Service.Communication;
using FanControl.Service.Config;
using FanControl.Service.Fan;
using FanControl.Service.Hardware;
using FanControl.Service.Logging;
using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Logging;

namespace FanControl.Service.Host;

/// <summary>
/// 公共组合根：硬件采样 + 通信 + 配置 + 日志的唯一装配入口。
/// 任何 UI 相关类型都不得出现在这里 —— 这是"UI 可替换"的结构前提。
/// </summary>
public static class FanControlServices
{
    /// <summary>把运行时所需的全部服务注册进容器（不构建容器，便于宿主追加自己的服务）。</summary>
    public static IServiceCollection AddFanControlServices(
        this IServiceCollection services,
        string logDirectory,
        bool logEnabled,
        int maxLogFiles)
    {
        var fileLogger = new FileLoggerProvider(logDirectory);
        fileLogger.Configure(logDirectory, logEnabled, Math.Max(1, maxLogFiles));
        services.AddSingleton(fileLogger);
        services.AddLogging(builder => builder.AddProvider(fileLogger));

        services.AddSingleton<IConfigManager, ConfigManager>();
        services.AddSingleton<AppState>();
        services.AddSingleton<FanController>();

        services.AddSingleton<TemperatureProviderFactory>();
        services.AddSingleton<FanSpeedProviderFactory>();

        services.AddSingleton<AtkAcpiTemperatureProvider>();
        services.AddSingleton<WmiTemperatureProvider>();
        services.AddSingleton<LibreHardwareMonitorTemperatureProvider>();
        services.AddSingleton<Aida64TemperatureProvider>();
        services.AddSingleton<SimulatedTemperatureProvider>();

        services.AddSingleton<AtkAcpiFanSpeedProvider>();
        services.AddSingleton<WmiFanSpeedProvider>();
        services.AddSingleton<LibreHardwareMonitorFanSpeedProvider>();
        services.AddSingleton<Aida64FanSpeedProvider>();
        services.AddSingleton<SimulatedFanSpeedProvider>();

        services.AddSingleton<CommunicationChannelFactory>();
        services.AddSingleton<ComChannel>();
        services.AddSingleton<BleChannel>();

        services.AddSingleton<FanControlRuntime>();
        return services;
    }

    /// <summary>便捷方法：直接构建装配好 FanControlRuntime 的容器。</summary>
    public static ServiceProvider BuildFanControlProvider(
        string logDirectory,
        bool logEnabled,
        int maxLogFiles)
        => new ServiceCollection()
            .AddFanControlServices(logDirectory, logEnabled, maxLogFiles)
            .BuildServiceProvider();
}
