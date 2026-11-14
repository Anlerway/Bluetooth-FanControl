using System.Text.Json;
using FanControl.Shared.Contracts;
using FanControl.Shared.Enums;
using FanControl.Shared.Models;

namespace FanControl.Host.Ui;

/// <summary>
/// 界面下发单个配置项时在这里落地：界面不需要复制 AppConfig 的完整结构，
/// 只发 key/value，由守护进程负责类型转换、范围校验与持久化。
/// </summary>
internal static class ConfigPatch
{
    public static bool TryApply(AppConfig current, string key, JsonElement value, out AppConfig updated, out ErrorInfo? error)
    {
        switch (key.Trim().ToLowerInvariant())
        {
            case "theme":
                return TryEnum<ThemeType>(current, value, parsed => current with { Theme = parsed }, out updated, out error);
            case "language":
                return TryString(current, value, text => current with { Language = text }, out updated, out error);
            case "temperatureunit":
                return TryEnum<TemperatureUnit>(current, value, parsed => current with { TemperatureUnit = parsed }, out updated, out error);
            case "temperaturesource":
                return TryEnum<TemperatureSource>(current, value, parsed => current with { TemperatureSource = parsed }, out updated, out error);
            case "gputemperaturesource":
                return TryEnum<TemperatureSource>(current, value, parsed => current with { GpuTemperatureSource = parsed }, out updated, out error);
            case "gpusourceenabled":
                return TryBool(current, value, flag => current with { GpuTemperatureSource = flag ? TemperatureSource.NvidiaSmiAdl : null }, out updated, out error);
            case "gpuselection":
                return TryString(current, value, text => current with { GpuSelection = text }, out updated, out error);
            case "fanspeedsource":
                return TryEnum<FanSpeedSource>(current, value, parsed => current with { FanSpeedSource = parsed }, out updated, out error);
            case "fancontrolmode":
                return TryEnum<FanControlMode>(current, value, parsed => current with { FanControlMode = parsed }, out updated, out error);
            case "communicationtype":
                return TryEnum<CommunicationType>(current, value, parsed => current with { CommunicationType = parsed }, out updated, out error);
            case "comport":
                return TryString(current, value, text => current with { ComPort = text }, out updated, out error);
            case "combaudrate":
                return TryInt(current, value, 1200, 921600, number => current with { ComBaudRate = number }, out updated, out error);
            case "bledevicemac":
                return TryString(current, value, text => current with { BleDeviceMac = text }, out updated, out error);
            case "bledevicename":
                return TryString(current, value, text => current with { BleDeviceName = text }, out updated, out error);
            case "pollintervalmilliseconds":
                return TryInt(current, value, 100, 5000, number => current with { PollIntervalMilliseconds = number }, out updated, out error);
            case "blepollintervalseconds":
                return TryInt(current, value, 1, 30, number => current with { BlePollIntervalSeconds = number }, out updated, out error);
            case "blereconnectintervalseconds":
                return TryInt(current, value, 1, 60, number => current with { BleReconnectIntervalSeconds = number }, out updated, out error);
            case "autoreconnectble":
                return TryBool(current, value, flag => current with { AutoReconnectBle = flag }, out updated, out error);
            case "pwmsmoothingwindowseconds":
                return TryDouble(current, value, 0.5, 30, number => current with { PwmSmoothingWindowSeconds = number }, out updated, out error);
            case "notifyonbledisconnect":
                return TryBool(current, value, flag => current with { NotifyOnBleDisconnect = flag }, out updated, out error);
            case "notifyontemperatureerror":
                return TryBool(current, value, flag => current with { NotifyOnTemperatureError = flag }, out updated, out error);
            case "manualpwmpercent":
                return TryDouble(current, value, 0, 100, number => current with { ManualPwmPercent = number }, out updated, out error);
            case "smoothingenabled":
                return TryBool(current, value, flag => current with { SmoothingEnabled = flag }, out updated, out error);
            default:
                updated = current;
                error = ErrorInfo.Of("config.unknownKey", "未知配置项：" + key, key);
                return false;
        }
    }

    /// <summary>温度-输出 PWM 曲线：按温度升序、至少 2 个点，范围温度 0-120 / 输出 PWM 0-100。</summary>
    public static bool TrySetCurve(AppConfig current, IReadOnlyList<(double Temperature, double Pwm)> points, out AppConfig updated, out ErrorInfo? error)
    {
        if (points.Count < 2)
        {
            updated = current;
            error = ErrorInfo.Of("config.curveTooFew", "曲线至少需要 2 个点。");
            return false;
        }

        var normalized = points
            .Select(point => new CurvePoint(
                Math.Clamp(Math.Round(point.Temperature, 1), 0, 120),
                Math.Clamp(Math.Round(point.Pwm, 1), 0, 100)))
            .OrderBy(point => point.TemperatureCelsius)
            .ToList();

        if (Math.Abs(normalized[normalized.Count - 1].PwmPercent - normalized[0].PwmPercent) < 0.01 && normalized.Count == 2)
        {
            updated = current;
            error = ErrorInfo.Of("config.curveFlat", "两端输出 PWM 不能相同。");
            return false;
        }

        updated = current with { Curve = normalized };
        error = null;
        return true;
    }

    /// <summary>转速-PWM 曲线：按转速升序、至少 2 个点。</summary>
    public static bool TrySetRpmCurve(AppConfig current, IReadOnlyList<(double Rpm, double Pwm)> points, out AppConfig updated, out ErrorInfo? error)
    {
        if (points.Count < 2)
        {
            updated = current;
            error = ErrorInfo.Of("config.curveTooFew", "曲线至少需要 2 个点。");
            return false;
        }

        var normalized = points
            .Select(point => new RpmCurvePoint(
                Math.Clamp(Math.Round(point.Rpm, 0), 0, 20000),
                Math.Clamp(Math.Round(point.Pwm, 1), 0, 100)))
            .OrderBy(point => point.Rpm)
            .ToList();

        updated = current with { RpmCurve = normalized };
        error = null;
        return true;
    }

    private static bool TryEnum<T>(AppConfig current, JsonElement value, Func<T, AppConfig> apply, out AppConfig updated, out ErrorInfo? error)
        where T : struct, Enum
    {
        if (value.ValueKind == JsonValueKind.String && Enum.TryParse<T>(value.GetString(), ignoreCase: true, out var parsed))
        {
            updated = apply(parsed);
            error = null;
            return true;
        }

        if (value.ValueKind == JsonValueKind.Number && value.TryGetInt32(out var number) && Enum.IsDefined(typeof(T), number))
        {
            updated = apply((T)Enum.ToObject(typeof(T), number));
            error = null;
            return true;
        }

        updated = current;
        error = ErrorInfo.Of("config.enum", "取值无法解析为 " + typeof(T).Name + "。", typeof(T).Name);
        return false;
    }

    private static bool TryString(AppConfig current, JsonElement value, Func<string, AppConfig> apply, out AppConfig updated, out ErrorInfo? error)
    {
        if (value.ValueKind != JsonValueKind.String)
        {
            updated = current;
            error = ErrorInfo.Of("config.notString", "取值必须是字符串。");
            return false;
        }

        updated = apply(value.GetString() ?? string.Empty);
        error = null;
        return true;
    }

    private static bool TryBool(AppConfig current, JsonElement value, Func<bool, AppConfig> apply, out AppConfig updated, out ErrorInfo? error)
    {
        if (value.ValueKind is not (JsonValueKind.True or JsonValueKind.False))
        {
            updated = current;
            error = ErrorInfo.Of("config.notBool", "取值必须是布尔值。");
            return false;
        }

        updated = apply(value.GetBoolean());
        error = null;
        return true;
    }

    private static bool TryInt(AppConfig current, JsonElement value, int min, int max, Func<int, AppConfig> apply, out AppConfig updated, out ErrorInfo? error)
    {
        if (value.ValueKind != JsonValueKind.Number || !value.TryGetInt32(out var number) || number < min || number > max)
        {
            updated = current;
            error = ErrorInfo.Of("config.rangeInt", "取值必须是 " + min + "-" + max + " 之间的整数。", min.ToString(), max.ToString());
            return false;
        }

        updated = apply(number);
        error = null;
        return true;
    }

    private static bool TryDouble(AppConfig current, JsonElement value, double min, double max, Func<double, AppConfig> apply, out AppConfig updated, out ErrorInfo? error)
    {
        if (value.ValueKind != JsonValueKind.Number || !value.TryGetDouble(out var number) || number < min || number > max)
        {
            updated = current;
            error = ErrorInfo.Of("config.rangeNumber", "取值必须是 " + min + "-" + max + " 之间的数值。", min.ToString(), max.ToString());
            return false;
        }

        updated = apply(number);
        error = null;
        return true;
    }
}
