namespace FanControl.Service.Hardware.Lenovo;

/// <summary>
/// 联想专有 WMI 的只读访问面（LENOVO_GAMEZONE_DATA / LENOVO_FAN_METHOD / LENOVO_FAN_TABLE_DATA）。
/// 抽成接口是为了让纯逻辑（选传感器、档位换算）能在单测里用假实现覆盖，不必真的跑 WMI。
/// </summary>
public interface ILenovoWmiBackend
{
    /// <summary>当前机型是否为联想且 WMI 类可用（结果按分钟级缓存）。</summary>
    bool IsAvailable { get; }

    /// <summary>读取风扇表（Fan_Id / Sensor_ID 组合），结果会缓存数十秒。</summary>
    IReadOnlyList<LenovoFanTableRow> GetFanTable();

    /// <summary>Fan_GetCurrentFanSpeed：返回固件原始值（档位、百分比或 RPM，按机型而定）。</summary>
    int? GetCurrentFanSpeed(int fanId);

    /// <summary>Fan_GetCurrentSensorTemperature：返回该传感器当前温度（摄氏度）。</summary>
    int? GetCurrentSensorTemperature(int sensorId);

    /// <summary>GameZone WMI 的 GetCPUTemperature（部分机型没有该温度传感器表时的兜底）。</summary>
    int? GetCpuTemperature();

    /// <summary>GameZone WMI 的 GetGPUTemperature。</summary>
    int? GetGpuTemperature();
}
