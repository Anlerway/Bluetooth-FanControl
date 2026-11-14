namespace FanControl.Shared.Enums;

/// <summary>
/// 风扇转速数据源（独立于温度数据源，用户可自行选择）。
/// ATKACPI 使用 G-Helper 同款 DSTS 端点（CPU_Fan/GPU_Fan，值 ×100 = RPM）。
/// </summary>
public enum FanSpeedSource
{
    AtkAcpi = 0,
    Wmi = 1,
    LibreHardwareMonitor = 2,
    Aida64 = 3,
    // 联想专有 WMI：Fan_GetCurrentFanSpeed + 档位表换算（编号与温度枚举不同，故运行时不按数值比较）
    LenovoWmi = 4,
    Simulated = 99,
}
