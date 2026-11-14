using FanControl.Shared.Enums;
using Microsoft.Extensions.Logging;

namespace FanControl.Service.Hardware.Lenovo;

/// <summary>
/// 联想 WMI 温度/转速数据源：传感器编号取自 LENOVO_FAN_TABLE_DATA，
/// 温度用 Fan_GetCurrentSensorTemperature 读，转速用 Fan_GetCurrentFanSpeed 配合档位表换算。
/// 没有传感器表的机型退回 GameZone 的 GetCPUTemperature / GetGPUTemperature。
/// </summary>
public sealed class LenovoWmiTemperatureProvider : ITemperatureProvider
{
    private readonly ILenovoWmiBackend _backend;
    private readonly ILogger<LenovoWmiTemperatureProvider> _logger;
    private int _warned;

    public LenovoWmiTemperatureProvider(
        ILenovoWmiBackend backend,
        ILogger<LenovoWmiTemperatureProvider> logger)
    {
        _backend = backend;
        _logger = logger;
    }

    public TemperatureSource Source => TemperatureSource.LenovoWmi;

    public Task<TemperatureSnapshot> ReadAsync(
        CancellationToken cancellationToken = default,
        string? gpuSelection = null)
    {
        return Task.Run(() => Read(gpuSelection), cancellationToken);
    }

    private TemperatureSnapshot Read(string? gpuSelection)
    {
        if (!_backend.IsAvailable)
        {
            WarnOnce("当前机型不是联想，或联想 WMI 接口不可用，Lenovo WMI 数据源没有数据。");
            return new TemperatureSnapshot(null, null, null);
        }

        var rows = _backend.GetFanTable();
        if (rows.Count == 0)
        {
            WarnOnce("联想 WMI 未返回风扇表（LENOVO_FAN_TABLE_DATA 为空），改用 GameZone 温度兜底。");
        }

        double? cpu = ReadSensor(LenovoFanTableReader.CpuSensorId(rows))
            ?? Sanitize(_backend.GetCpuTemperature());
        double? gpu = ReadSensor(LenovoFanTableReader.GpuSensorId(rows))
            ?? Sanitize(_backend.GetGpuTemperature());

        // 联想 WMI 读不到 GPU 时，回落到 nvidia-smi / ADL（与 WMI 数据源的处理一致）
        gpu ??= GpuTemperatureReader.Read(gpuSelection);

        var fanRpm = LenovoFanTableReader.ReadFanRpm(_backend, rows);
        return new TemperatureSnapshot(cpu, gpu, fanRpm);
    }

    private double? ReadSensor(int? sensorId)
    {
        if (sensorId is null)
        {
            return null;
        }

        return Sanitize(_backend.GetCurrentSensorTemperature(sensorId.Value));
    }

    private static double? Sanitize(int? raw)
        => raw is int value && LenovoFanTableReader.IsPlausibleTemperature(value) ? value : null;

    private void WarnOnce(string message)
    {
        if (Interlocked.Exchange(ref _warned, 1) != 0)
        {
            return;
        }

        _logger.LogWarning(message);
    }
}
