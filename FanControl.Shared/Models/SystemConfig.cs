using FanControl.Shared.Enums;

namespace FanControl.Shared.Models;

/// <summary>
/// 系统级配置：用户数据存放位置（首次启动引导选择，结果保存于所选数据根目录的 Config\system.json）。
/// 数据根目录为 %AppData%\FanControl 或 exe 所在目录\Userdata，
/// 系统设置（system.json）、用户配置（appconfig.json）存放于根目录 Config\，日志存放于根目录 Logs\；
/// 切换位置时已有数据整体迁移到新位置。
/// </summary>
public sealed record SystemConfig
{
    /// <summary>用户数据存放位置（AppData / exe 目录 Userdata），决定 Config\ 与 Logs\ 的归属。</summary>
    public ConfigLocation UserDataLocation { get; init; } = ConfigLocation.AppData;

    // 日志开关
    public bool LogEnabled { get; init; } = true;

    // 日志文件最大保留数（超出自动清除最旧的，防止堆积）
    public int MaxLogFiles { get; init; } = 20;
}
