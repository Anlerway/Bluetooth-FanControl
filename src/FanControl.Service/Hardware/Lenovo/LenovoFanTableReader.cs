namespace FanControl.Service.Hardware.Lenovo;

/// <summary>
/// 联想风扇表的纯逻辑：挑传感器、挑风扇、把固件原始读数换算成 RPM。
/// 这里不碰 WMI，全部是可在单测里直接验证的静态方法。
/// </summary>
public static class LenovoFanTableReader
{
    /// <summary>风扇编号列表（升序去重）；表为空时退回 0 号风扇。</summary>
    public static IReadOnlyList<int> FanIds(IEnumerable<LenovoFanTableRow> rows)
    {
        var ids = rows.Select(row => row.FanId).Distinct().OrderBy(id => id).ToList();
        return ids.Count > 0 ? ids : new[] { 0 };
    }

    /// <summary>温度传感器编号列表（升序去重）。</summary>
    public static IReadOnlyList<int> SensorIds(IEnumerable<LenovoFanTableRow> rows)
        => rows.Select(row => row.SensorId).Distinct().OrderBy(id => id).ToList();

    /// <summary>
    /// CPU 温度传感器：取编号最小的传感器。联想固件的编号顺序是 CPU、GPU、第二 GPU、PCH，
    /// 所以 0 号是 CPU，1 号是 GPU（不同机型可能从 1 开始）。
    /// </summary>
    public static int? CpuSensorId(IEnumerable<LenovoFanTableRow> rows)
    {
        var ids = SensorIds(rows);
        return ids.Count > 0 ? ids[0] : null;
    }

    /// <summary>GPU 温度传感器：CPU 之后的那个编号，没有就返回 null。</summary>
    public static int? GpuSensorId(IEnumerable<LenovoFanTableRow> rows)
    {
        var ids = SensorIds(rows);
        return ids.Count > 1 ? ids[1] : null;
    }

    /// <summary>温度读数是否可信：0 与 ≥127 都是固件“无此数据”的占位值。</summary>
    public static bool IsPlausibleTemperature(int raw) => raw is > 0 and < 127;

    /// <summary>
    /// 固件原始风扇读数 → RPM。
    /// 联想各代固件单位不同：老机型返回档位（0..10，用 FanTable_Data 查表），
    /// 新机型返回百分比（0..100），个别机型直接返回 RPM。
    /// </summary>
    public static double? FanRpm(int raw, LenovoFanTableRow? row)
    {
        if (raw < 0)
        {
            return null;
        }

        // 档位：有档位表且下标在范围内
        if (raw <= 10 && row is not null && raw < row.FanSpeeds.Count)
        {
            return row.FanSpeeds[raw];
        }

        // 百分比：按当前电源模式下的最大转速折算
        if (raw <= 100)
        {
            var maxRpm = row?.MaxFanRpm ?? 0;
            return maxRpm > 0 ? Math.Round(maxRpm * raw / 100.0) : null;
        }

        // 已经是 RPM（合理上限 10000）
        return raw <= 10000 ? raw : null;
    }

    /// <summary>读取所有风扇的转速并取最大值（多风扇机型界面只显示一个转速）。</summary>
    public static double? ReadFanRpm(ILenovoWmiBackend backend, IReadOnlyList<LenovoFanTableRow> rows)
    {
        double? best = null;
        foreach (var fanId in FanIds(rows))
        {
            var raw = backend.GetCurrentFanSpeed(fanId);
            if (raw is null)
            {
                continue;
            }

            var row = rows.FirstOrDefault(item => item.FanId == fanId);
            var rpm = FanRpm(raw.Value, row);
            if (rpm is null)
            {
                continue;
            }

            best = best is null ? rpm : Math.Max(best.Value, rpm.Value);
        }

        return best;
    }
}
