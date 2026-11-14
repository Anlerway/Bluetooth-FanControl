using FanControl.Shared.Enums;
using Microsoft.Extensions.Logging;

namespace FanControl.Service.Hardware.Lenovo;

/// <summary>
/// 联想 WMI 转速数据源：逐风扇调用 Fan_GetCurrentFanSpeed，按档位表折算 RPM 后取最大值。
/// </summary>
public sealed class LenovoWmiFanSpeedProvider : IFanSpeedProvider
{
    private readonly ILenovoWmiBackend _backend;
    private readonly ILogger<LenovoWmiFanSpeedProvider> _logger;
    private int _warned;

    public LenovoWmiFanSpeedProvider(
        ILenovoWmiBackend backend,
        ILogger<LenovoWmiFanSpeedProvider> logger)
    {
        _backend = backend;
        _logger = logger;
    }

    public FanSpeedSource Source => FanSpeedSource.LenovoWmi;

    public Task<double?> ReadAsync(CancellationToken cancellationToken = default)
    {
        return Task.Run(
            () =>
            {
                if (!_backend.IsAvailable)
                {
                    if (Interlocked.Exchange(ref _warned, 1) == 0)
                    {
                        _logger.LogWarning("当前机型不是联想，或联想 WMI 接口不可用，Lenovo WMI 转速源没有数据。");
                    }

                    return (double?)null;
                }

                return LenovoFanTableReader.ReadFanRpm(_backend, _backend.GetFanTable());
            },
            cancellationToken);
    }
}
