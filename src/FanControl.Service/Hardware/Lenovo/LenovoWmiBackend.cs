using System.Globalization;
using System.Management;
using Microsoft.Extensions.Logging;

namespace FanControl.Service.Hardware.Lenovo;

/// <summary>
/// 联想专有 WMI 的只读实现，调用面参照 Lenovo Legion Toolkit：
/// - LENOVO_FAN_TABLE_DATA：只读表数据类，给出 Fan_Id / Sensor_ID 与两张档位表；
/// - LENOVO_FAN_METHOD：Fan_GetCurrentFanSpeed / Fan_GetCurrentSensorTemperature；
/// - LENOVO_GAMEZONE_DATA：部分机型没有传感器表时的温度兜底。
/// 所有调用都吞掉 ManagementException：非联想机型上这些类根本不存在。
/// </summary>
public sealed class LenovoWmiBackend : ILenovoWmiBackend
{
    private const string WmiScope = @"root\WMI";
    private const string CimScope = @"root\CIMV2";
    private const string FanTableQuery = "SELECT * FROM LENOVO_FAN_TABLE_DATA";
    private const string FanMethodQuery = "SELECT * FROM LENOVO_FAN_METHOD";
    private const string GameZoneQuery = "SELECT * FROM LENOVO_GAMEZONE_DATA";

    // 机型判断很便宜但没必要每次采样都做；风扇表可能随电源模式变化，缓存半分钟
    private static readonly TimeSpan DetectPeriod = TimeSpan.FromMinutes(5);
    private static readonly TimeSpan TablePeriod = TimeSpan.FromSeconds(30);

    private readonly object _sync = new();
    private readonly ILogger<LenovoWmiBackend> _logger;

    private bool? _available;
    private DateTime _detectedAt;
    private IReadOnlyList<LenovoFanTableRow>? _table;
    private DateTime _tableAt;
    private bool _invokeFailureLogged;

    public LenovoWmiBackend(ILogger<LenovoWmiBackend> logger)
    {
        _logger = logger;
    }

    public bool IsAvailable
    {
        get
        {
            lock (_sync)
            {
                var now = DateTime.UtcNow;
                if (_available is null || now - _detectedAt >= DetectPeriod)
                {
                    _detectedAt = now;
                    _available = Detect();
                }

                return _available.Value;
            }
        }
    }

    public IReadOnlyList<LenovoFanTableRow> GetFanTable()
    {
        lock (_sync)
        {
            var now = DateTime.UtcNow;
            if (_table is not null && now - _tableAt < TablePeriod)
            {
                return _table;
            }

            _tableAt = now;
            _table = ReadFanTableCore();
            return _table;
        }
    }

    public int? GetCurrentFanSpeed(int fanId)
        => InvokeInt(FanMethodQuery, "Fan_GetCurrentFanSpeed", "CurrentFanSpeed", ("FanID", fanId));

    public int? GetCurrentSensorTemperature(int sensorId)
        => InvokeInt(FanMethodQuery, "Fan_GetCurrentSensorTemperature", "CurrentSensorTemperature", ("SensorID", sensorId));

    public int? GetCpuTemperature() => InvokeInt(GameZoneQuery, "GetCPUTemperature", "Data");

    public int? GetGpuTemperature() => InvokeInt(GameZoneQuery, "GetGPUTemperature", "Data");

    private bool Detect()
    {
        try
        {
            using var searcher = new ManagementObjectSearcher(
                CimScope,
                "SELECT Manufacturer FROM Win32_ComputerSystem");
            using var results = searcher.Get();
            foreach (ManagementBaseObject item in results)
            {
                var manufacturer = ReadString(ToMap(item), "Manufacturer");
                if (manufacturer is not null
                    && manufacturer.Contains("LENOVO", StringComparison.OrdinalIgnoreCase))
                {
                    return true;
                }
            }
        }
        catch (Exception ex)
        {
            _logger.LogDebug(ex, "查询 Win32_ComputerSystem 判断联想机型失败。");
        }

        // 厂商字段被 OEM 工具改写时退一步：直接看 WMI 类在不在
        return ClassExists(FanMethodQuery) || ClassExists(GameZoneQuery);
    }

    private bool ClassExists(string query)
    {
        try
        {
            using var searcher = new ManagementObjectSearcher(WmiScope, query);
            using var results = searcher.Get();

            // ManagementObjectCollection 是惰性的：只有真正枚举时才会执行 WQL，
            // 类不存在也只在枚举时抛 ManagementException，所以必须走一次 foreach。
            foreach (ManagementBaseObject _ in results)
            {
                break;
            }

            return true;
        }
        catch (Exception)
        {
            return false;
        }
    }

    private IReadOnlyList<LenovoFanTableRow> ReadFanTableCore()
    {
        var rows = new List<LenovoFanTableRow>();
        try
        {
            using var searcher = new ManagementObjectSearcher(WmiScope, FanTableQuery);
            using var results = searcher.Get();
            foreach (ManagementBaseObject item in results)
            {
                var map = ToMap(item);
                rows.Add(new LenovoFanTableRow(
                    Mode: ReadInt(map, "Mode"),
                    FanId: ReadInt(map, "Fan_Id") ?? 0,
                    SensorId: ReadInt(map, "Sensor_ID") ?? 0,
                    FanSpeeds: ReadUInt16List(map, "FanTable_Data"),
                    SensorTemps: ReadUInt16List(map, "SensorTable_Data"),
                    DefaultFanMaxSpeed: ReadInt(map, "DefaultFanMaxSpeed"),
                    CurrentFanMaxSpeed: ReadInt(map, "CurrentFanMaxSpeed")));
            }
        }
        catch (Exception ex)
        {
            LogInvokeFailure(ex, FanTableQuery);
        }

        return rows;
    }

    private int? InvokeInt(
        string query,
        string methodName,
        string resultProperty,
        params (string Name, object Value)[] arguments)
    {
        try
        {
            using var searcher = new ManagementObjectSearcher(WmiScope, query);
            using var results = searcher.Get();
            foreach (ManagementBaseObject item in results)
            {
                if (item is not ManagementObject instance)
                {
                    continue;
                }

                using var inParams = instance.GetMethodParameters(methodName);
                foreach (var (name, value) in arguments)
                {
                    inParams[name] = value;
                }

                using var outParams = instance.InvokeMethod(methodName, inParams, new InvokeMethodOptions());
                return outParams is null ? null : ReadInt(ToMap(outParams), resultProperty);
            }

            return null;
        }
        catch (Exception ex)
        {
            LogInvokeFailure(ex, methodName);
            return null;
        }
    }

    private void LogInvokeFailure(Exception ex, string operation)
    {
        if (_invokeFailureLogged)
        {
            return;
        }

        _invokeFailureLogged = true;
        _logger.LogDebug(ex, "联想 WMI 调用失败（{Operation}），当前机型可能不支持该接口。", operation);
    }

    private static Dictionary<string, object?> ToMap(ManagementBaseObject item)
    {
        var map = new Dictionary<string, object?>(StringComparer.OrdinalIgnoreCase);
        foreach (PropertyData property in item.Properties)
        {
            map[property.Name] = property.Value;
        }

        return map;
    }

    private static int? ReadInt(IReadOnlyDictionary<string, object?> map, string name)
    {
        if (!map.TryGetValue(name, out var value) || value is null)
        {
            return null;
        }

        try
        {
            return Convert.ToInt32(value, CultureInfo.InvariantCulture);
        }
        catch (Exception)
        {
            return null;
        }
    }

    private static string? ReadString(IReadOnlyDictionary<string, object?> map, string name)
    {
        if (!map.TryGetValue(name, out var value) || value is null)
        {
            return null;
        }

        try
        {
            return Convert.ToString(value, CultureInfo.InvariantCulture);
        }
        catch (Exception)
        {
            return null;
        }
    }

    private static IReadOnlyList<ushort> ReadUInt16List(IReadOnlyDictionary<string, object?> map, string name)
    {
        if (!map.TryGetValue(name, out var value) || value is not Array array)
        {
            return Array.Empty<ushort>();
        }

        var list = new List<ushort>(array.Length);
        foreach (var element in array)
        {
            try
            {
                list.Add(Convert.ToUInt16(element, CultureInfo.InvariantCulture));
            }
            catch (Exception)
            {
                list.Add(0);
            }
        }

        return list;
    }
}
