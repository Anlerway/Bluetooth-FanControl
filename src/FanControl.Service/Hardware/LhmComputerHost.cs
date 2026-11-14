using FanControl.Shared.Enums;
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
    private static bool _cpu;
    private static bool _gpu;
    private static bool _board;
    private static bool _rebuild;

    /// <summary>
    /// 按当前配置决定 LHM 要枚举哪些模块。实测（本机，单一进程）：只开 CPU 时工作集 76 MB，
    /// 加 GPU 模块后 114 MB（NVAPI/ADL 会跟着加载），再加主板/EC 后 85 MB 起步。
    /// 默认配置是 CPU 温度走 LHM、GPU 走 nvidia-smi、转速走 ATK ACPI，只需要 CPU 模块。
    /// </summary>
    public static void Configure(TemperatureSource temperature, TemperatureSource? gpu, FanSpeedSource fanSpeed)
    {
        var needCpu = temperature == TemperatureSource.LibreHardwareMonitor
                      || fanSpeed == FanSpeedSource.LibreHardwareMonitor
                      || gpu == TemperatureSource.LibreHardwareMonitor;
        var needGpu = gpu == TemperatureSource.LibreHardwareMonitor;
        var needBoard = fanSpeed == FanSpeedSource.LibreHardwareMonitor;

        lock (Sync)
        {
            if ((!needCpu || _cpu) && (!needGpu || _gpu) && (!needBoard || _board))
            {
                return;
            }

            _cpu |= needCpu;
            _gpu |= needGpu;
            _board |= needBoard;

            // 模块只加不减：切回不依赖 LHM 的数据源时保留句柄，免得反复 Open/Close 抖动。
            // 真正的重建留到下一次 Get()：配置可能从界面线程改，不能在别的线程正遍历硬件时 Close。
            _rebuild = _computer is not null;
        }
    }

    /// <summary>没有配置任何 LHM 模块时返回 null（这样也不会把 LHM 的原生依赖加载进来）。</summary>
    public static Computer? Get()
    {
        lock (Sync)
        {
            if (!_cpu && !_gpu && !_board)
            {
                return null;
            }

            if (_computer is not null && _rebuild)
            {
                _computer.Close();
                _computer = null;
                _rebuild = false;
            }

            if (_computer is null)
            {
                _computer = new Computer
                {
                    IsCpuEnabled = _cpu,
                    IsGpuEnabled = _gpu,
                    // 风扇转速通常挂在主板/嵌入式控制器上
                    IsMotherboardEnabled = _board,
                    IsControllerEnabled = _board,
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
            var computer = Get();
            if (computer is null)
            {
                return;
            }

            var now = Environment.TickCount64;
            if (_lastUpdateTimestamp != 0 && now - _lastUpdateTimestamp < UpdateIntervalMilliseconds)
            {
                return;
            }

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