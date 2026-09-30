namespace FanControl.Shared.Enums;

/// <summary>
/// 用户数据存放位置模式（首次启动引导选择，之后可在设置中修改）。
/// 配置（system.json / appconfig.json）与日志统一存放在所选数据根目录下，
/// 子目录固定为 Config\ 与 Logs\。
/// </summary>
public enum ConfigLocation
{
    /// <summary>exe 所在目录\Userdata（需管理员写入权限，本程序以管理员运行）。</summary>
    ExeDirectory = 0,

    /// <summary>%AppData%\FanControl（用户数据目录）。</summary>
    AppData = 1,
}
