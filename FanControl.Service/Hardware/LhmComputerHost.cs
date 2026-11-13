using LibreHardwareMonitor.Hardware;

namespace FanControl.Service.Hardware;

/// <summary>
/// LibreHardwareMonitor 单例宿主：温度/转速两个 Provider 共用同一个 Computer 实例，
/// 避免重复枚举硬件（LHM 同一实例重复 Open 会冲突，多实例成本也高）。
/// </summary>
internal static class LhmComputerHost
{
    private const int UpdateIntervalMilliseconds = 800;
    private static readonly object Sync = new();
    private static readonly object UpdateSync = new();
    private static Computer? _computer;
    private static long _lastUpdateTimestamp;

    public static Computer Get()
    {
        lock (Sync)
        {
            if (_computer is null)
            {
                _computer = new Computer
                {
                    IsCpuEnabled = true,
                    IsGpuEnabled = true,
                    // 风扇转速通常挂在主板/嵌入式控制器上，需要开启
                    IsMotherboardEnabled = true,
                    IsControllerEnabled = true,
                    IsStorageEnabled = false,
                    IsNetworkEnabled = false,
                };
                _computer.Open();
            }

            return _computer;
        }
    }

    /// <summary>
    /// 统一刷新全部硬件（含子硬件），最小间隔 800 ms：
    /// 温度与转速两个 Provider 在同一采样周期共享一次 Update，避免重复走 EC/SMBus
    /// （LHM 的 Update 是最重的一次硬件 IO）。
    /// </summary>
    public static void EnsureFresh()
    {
        lock (UpdateSync)
        {
            var now = Environment.TickCount64;
            if (_lastUpdateTimestamp != 0 && now - _lastUpdateTimestamp < UpdateIntervalMilliseconds)
            {
                return;
            }

            var computer = Get();
            foreach (var hardware in computer.Hardware)
            {
                hardware.Update();
                foreach (var subHardware in hardware.SubHardware)
                {
                    subHardware.Update();
                }
            }

            _lastUpdateTimestamp = now;
        }
    }

    public static void Close()
    {
        lock (Sync)
        {
            _computer?.Close();
            _computer = null;
            _lastUpdateTimestamp = 0;
        }
    }
}
