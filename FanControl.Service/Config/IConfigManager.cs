using FanControl.Shared.Models;

namespace FanControl.Service.Config;

/// <summary>配置管理器：系统级配置（存放位置）与应用配置（业务参数）。</summary>
public interface IConfigManager
{
    Task<SystemConfig> LoadSystemConfigAsync(CancellationToken cancellationToken = default);

    Task SaveSystemConfigAsync(SystemConfig config, CancellationToken cancellationToken = default);

    /// <summary>程序安装目录。</summary>
    string InstallDirectory { get; }

    /// <summary>按系统配置解析日志目录（数据根目录下的 Logs）。</summary>
    string GetLogDirectory(SystemConfig systemConfig);

    Task<AppConfig> LoadAppConfigAsync(CancellationToken cancellationToken = default);

    Task SaveAppConfigAsync(AppConfig config, CancellationToken cancellationToken = default);
}
