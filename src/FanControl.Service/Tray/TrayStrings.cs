namespace FanControl.Service.Tray;

/// <summary>
/// 托盘（菜单/通知/提示）文案：跟随 <c>AppConfig.Language</c>，由守护进程在启动时与配置变更时写入。
/// 托盘属于后台进程，界面切语言后 <c>FanControlRuntime.ConfigChanged</c> 会把它同步到这边。
/// </summary>
public static class TrayStrings
{
    private static volatile bool _english;

    public static void SetLanguage(string? language) =>
        _english = !string.IsNullOrWhiteSpace(language)
                   && language.StartsWith("en", StringComparison.OrdinalIgnoreCase);

    public static bool IsEnglish => _english;

    public static string OpenUi => _english ? "Open FanControl" : "打开主界面";
    public static string Reconnect => _english ? "Reconnect" : "重建连接";
    public static string Autostart => _english ? "Run at startup" : "开机自启";
    public static string Exit => _english ? "Exit" : "退出";
    public static string AppTitle => _english ? "Fan control" : "风扇控制";
    public static string Disconnected =>
        _english ? "Lost the link to the fan controller, reconnecting." : "与风扇控制器的连接已断开，正在尝试重连。";
    public static string TemperatureErrorTitle => _english ? "Temperature read failed" : "温度读取异常";
    public static string TemperatureErrorFallback =>
        _english ? "The temperature source is temporarily unavailable." : "温度数据源暂时不可用。";
    public static string AutostartFailed(string reason) =>
        _english ? "Failed to set run-at-startup: " + reason : "设置开机自启失败：" + reason;

    public static string State(Shared.Enums.CommunicationState state) => state switch
    {
        Shared.Enums.CommunicationState.Connected => _english ? "Connected" : "已连接",
        Shared.Enums.CommunicationState.Reconnecting => _english ? "Reconnecting" : "正在重连",
        _ => _english ? "Waiting" : "等待连接",
    };
}
