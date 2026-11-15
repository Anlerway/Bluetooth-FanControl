using FanControl.Shared.Enums;

namespace FanControl.Shared.Models;

/// <summary>应用业务配置：数据源、控制模式、通信参数、曲线、主题。</summary>
public sealed record AppConfig
{
    public TemperatureSource TemperatureSource { get; init; } = TemperatureSource.LibreHardwareMonitor;

    // GPU 温度获取方式（默认 NVIDIA-SMI / ADL）：NvidiaSmiAdl / LibreHardwareMonitor / AtkAcpi / Aida64
    // null 兼容旧配置，运行时代视为 NvidiaSmiAdl
    public TemperatureSource? GpuTemperatureSource { get; init; } = TemperatureSource.NvidiaSmiAdl;

    // GPU 选择：Auto=自动（多卡取最高温）；或指定厂商/名称关键词（NVIDIA / AMD / Intel / 如 "RTX 4060"）
    public string GpuSelection { get; init; } = "Auto";

    // 风扇转速数据源（独立于温度数据源；华硕机型建议 ATKACPI，G-Helper 同款端点）
    public FanSpeedSource FanSpeedSource { get; init; } = FanSpeedSource.AtkAcpi;

    public FanControlMode FanControlMode { get; init; } = FanControlMode.CpuTemp;

    public CommunicationType CommunicationType { get; init; } = CommunicationType.Ble;

    public string ComPort { get; init; } = "COM3";

    public int ComBaudRate { get; init; } = 115200;

    // BLE 设备定位主键：蓝牙 MAC 地址（AA:BB:CC:DD:EE:FF，统一大写冒号分隔）
    // 名称仅用于界面显示，不作为定位依据（同名设备会选错）
    public string BleDeviceMac { get; init; } = string.Empty;

    // BLE 设备名（仅显示用；旧配置只有名称时仍作为回退定位依据）
    public string BleDeviceName { get; init; } = string.Empty;

    // 温度采样轮询：0.1-5 秒
    public int PollIntervalMilliseconds { get; init; } = 1000;

    // 已连接时的发送轮询：1-30 秒
    public int BlePollIntervalSeconds { get; init; } = 2;

    // 断开后的重连轮询：1-60 秒
    public int BleReconnectIntervalSeconds { get; init; } = 5;

    // 自动重连开关（COM 与 BLE 共用）
    public bool AutoReconnectBle { get; init; } = true;

    // 风扇平滑窗口（秒）：记录最近这段时间内的目标 PWM 并取时间加权平均，
    // 抑制温度剧烈波动导致的转速抖动；窗口越大越平缓、跟随越慢（0 = 不平滑）
    public double PwmSmoothingWindowSeconds { get; init; } = 3.0;

    // 通知开关
    public bool NotifyOnBleDisconnect { get; init; } = true;

    public bool NotifyOnTemperatureError { get; init; } = true;

    // 界面语言（zh-CN / en-US）
    public string Language { get; init; } = "zh-CN";

    // 温度显示单位
    public TemperatureUnit TemperatureUnit { get; init; } = TemperatureUnit.Celsius;

    public ThemeType Theme { get; init; } = ThemeType.System;

    public double ManualPwmPercent { get; init; } = 50;

    // 风扇转速平滑开关：关闭时（含手动模式）PWM 直接取目标值，不做平滑
    public bool SmoothingEnabled { get; init; } = true;

    // 默认曲线取 1.4.0 现场调试出来的那条（温度 °C → PWM %，9 个点）
    public IReadOnlyList<CurvePoint> Curve { get; init; } =
        new List<CurvePoint>
        {
            new(0.3, 21.2),
            new(16.9, 21.2),
            new(35.3, 24.3),
            new(46.5, 33.6),
            new(53.5, 51.3),
            new(64.4, 65.5),
            new(78.7, 71.3),
            new(97.7, 73.0),
            new(119.7, 75.4),
        };

    // 转速-PWM 曲线（X = RPM 0-10000，Y = PWM%）
    // 默认转速曲线同样取现场调试值（RPM → PWM %）
    public IReadOnlyList<RpmCurvePoint> RpmCurve { get; init; } =
        new List<RpmCurvePoint>
        {
            new(87, 25.5),
            new(1753, 26.1),
            new(3247, 31.0),
            new(4177, 40.0),
            new(4719, 57.4),
            new(5390, 69.0),
            new(6407, 75.1),
            new(8247, 76.5),
            new(9957, 77.1),
        };
}
