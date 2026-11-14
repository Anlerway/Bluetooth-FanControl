namespace FanControl.Service.Hardware.Lenovo;

/// <summary>
/// LENOVO_FAN_TABLE_DATA 的一行：某个风扇（Fan_Id）与某个温度传感器（Sensor_ID）共用的一张曲线表。
/// 联想把「档位 → 转速」和「档位 → 温度阈值」放在两张平行数组里，数组下标就是档位（0..10）。
/// </summary>
/// <param name="Mode">固件模式编号，部分机型没有该字段。</param>
/// <param name="FanId">风扇编号，0 通常是 CPU 风扇，1 是 GPU 风扇。</param>
/// <param name="SensorId">温度传感器编号，用来调用 Fan_GetCurrentSensorTemperature。</param>
/// <param name="FanSpeeds">档位 → 转速（RPM）映射表。</param>
/// <param name="SensorTemps">档位 → 温度阈值（摄氏度），127 表示该档位无效。</param>
/// <param name="DefaultFanMaxSpeed">固件默认最大转速。</param>
/// <param name="CurrentFanMaxSpeed">当前电源模式下的最大转速。</param>
public sealed record LenovoFanTableRow(
    int? Mode,
    int FanId,
    int SensorId,
    IReadOnlyList<ushort> FanSpeeds,
    IReadOnlyList<ushort> SensorTemps,
    int? DefaultFanMaxSpeed,
    int? CurrentFanMaxSpeed)
{
    /// <summary>最大转速（RPM）：优先用固件给出的当前上限，退化到档位表里的最大值。</summary>
    public double MaxFanRpm
    {
        get
        {
            if (CurrentFanMaxSpeed is > 0)
            {
                return CurrentFanMaxSpeed.Value;
            }

            if (DefaultFanMaxSpeed is > 0)
            {
                return DefaultFanMaxSpeed.Value;
            }

            var max = 0;
            foreach (var speed in FanSpeeds)
            {
                if (speed > max)
                {
                    max = speed;
                }
            }

            return max;
        }
    }
}
