using System.Buffers;
using System.Text;
using System.Text.Encodings.Web;
using System.Text.Json;
using FanControl.Service.Host;
using FanControl.Shared.Enums;
using FanControl.Shared.Models;

namespace FanControl.Host.Ipc;

/// <summary>
/// 把运行时快照序列化成界面进程消费的 JSON。字段名固定为 camelCase，
/// C++ 客户端按名字取值，两边改动必须同步（协议见 docs/IPC-PROTOCOL.md）。
/// </summary>
internal static class StateJson
{
    // 中文提示/设备名按 UTF-8 原样输出，避免 \uXXXX 转义（C++ 客户端只做最简 JSON 解析）。
    private static readonly JsonWriterOptions WriterOptions = new() { Encoder = JavaScriptEncoder.UnsafeRelaxedJsonEscaping };

    public static string BuildState(FanControlRuntime runtime, bool autostart)
        => Build(runtime, includeConfig: true, eventName: "state", autostart: autostart);

    public static string BuildConfig(FanControlRuntime runtime, bool autostart)
        => Build(runtime, includeConfig: true, eventName: "config", autostart: autostart);

    private static string Build(FanControlRuntime runtime, bool includeConfig, string eventName, bool autostart)
    {
        var buffer = new ArrayBufferWriter<byte>(1024);
        using (var writer = new Utf8JsonWriter(buffer, WriterOptions))
        {
            writer.WriteStartObject();
            writer.WriteString("event", eventName);

            if (includeConfig)
            {
                writer.WritePropertyName("config");
                WriteConfig(writer, runtime.CurrentConfig);
            }

            writer.WritePropertyName("state");
            WriteRuntime(writer, runtime, autostart);

            writer.WriteEndObject();
        }

        return Encoding.UTF8.GetString(buffer.WrittenSpan);
    }

    private static void WriteConfig(Utf8JsonWriter writer, AppConfig config)
    {
        writer.WriteStartObject();
        writer.WriteString("temperatureSource", config.TemperatureSource.ToString());
        writer.WriteString("gpuTemperatureSource", config.GpuTemperatureSource?.ToString() ?? string.Empty);
        writer.WriteString("gpuSelection", config.GpuSelection);
        writer.WriteString("fanSpeedSource", config.FanSpeedSource.ToString());
        writer.WriteString("fanControlMode", config.FanControlMode.ToString());
        writer.WriteString("communicationType", config.CommunicationType.ToString());
        writer.WriteString("comPort", config.ComPort);
        writer.WriteNumber("comBaudRate", config.ComBaudRate);
        writer.WriteString("bleDeviceMac", config.BleDeviceMac);
        writer.WriteString("bleDeviceName", config.BleDeviceName);
        writer.WriteNumber("pollIntervalMilliseconds", config.PollIntervalMilliseconds);
        writer.WriteNumber("blePollIntervalSeconds", config.BlePollIntervalSeconds);
        writer.WriteNumber("bleReconnectIntervalSeconds", config.BleReconnectIntervalSeconds);
        writer.WriteBoolean("autoReconnectBle", config.AutoReconnectBle);
        writer.WriteNumber("pwmSmoothingWindowSeconds", config.PwmSmoothingWindowSeconds);
        writer.WriteBoolean("notifyOnBleDisconnect", config.NotifyOnBleDisconnect);
        writer.WriteBoolean("notifyOnTemperatureError", config.NotifyOnTemperatureError);
        writer.WriteString("language", config.Language);
        writer.WriteString("temperatureUnit", config.TemperatureUnit.ToString());
        writer.WriteString("theme", config.Theme.ToString());
        writer.WriteNumber("manualPwmPercent", config.ManualPwmPercent);
        writer.WriteBoolean("smoothingEnabled", config.SmoothingEnabled);

        writer.WriteStartArray("curve");
        foreach (var point in config.Curve)
        {
            writer.WriteStartArray();
            writer.WriteNumberValue(Math.Round(point.TemperatureCelsius, 1));
            writer.WriteNumberValue(Math.Round(point.PwmPercent, 1));
            writer.WriteEndArray();
        }

        writer.WriteEndArray();

        writer.WriteStartArray("rpmCurve");
        foreach (var point in config.RpmCurve)
        {
            writer.WriteStartArray();
            writer.WriteNumberValue(Math.Round(point.Rpm, 0));
            writer.WriteNumberValue(Math.Round(point.PwmPercent, 1));
            writer.WriteEndArray();
        }

        writer.WriteEndArray();

        writer.WriteEndObject();
    }

    private static void WriteRuntime(Utf8JsonWriter writer, FanControlRuntime runtime, bool autostart)
    {
        var packet = runtime.LatestPacket;
        var config = runtime.CurrentConfig;

        writer.WriteStartObject();
        writer.WriteNumber("cpu", Math.Round(packet?.CpuTemperatureCelsius ?? 0, 1));
        writer.WriteNumber("gpu", Math.Round(packet?.GpuTemperatureCelsius ?? 0, 1));
        writer.WriteNumber("rpm", Math.Round(packet?.FanRpm ?? 0));
        writer.WriteNumber("pwm", Math.Round(packet?.PwmPercent ?? 0, 1));
        writer.WriteNumber("rawPwm", Math.Round(packet?.RawTargetPwmPercent ?? 0, 1));
        writer.WriteString("mode", packet?.FanControlMode ?? config.FanControlMode.ToString());
        writer.WriteString("connectionState", runtime.ConnectionState.ToString());
        writer.WriteBoolean("connected", runtime.ConnectionState == CommunicationState.Connected);
        writer.WriteString("communicationType", config.CommunicationType.ToString());
        writer.WriteString("deviceName", DescribeDevice(config));
        writer.WriteString("lastError", runtime.LastError ?? string.Empty);
        var error = runtime.LastErrorInfo;
        writer.WriteString("errorCode", error?.Code ?? string.Empty);
        writer.WriteString("errorArg1", error?.Arg1 ?? string.Empty);
        writer.WriteString("errorArg2", error?.Arg2 ?? string.Empty);
        var commError = runtime.LastCommErrorInfo;
        writer.WriteString("commError", runtime.LastCommError ?? string.Empty);
        writer.WriteString("commErrorCode", commError?.Code ?? string.Empty);
        writer.WriteString("commErrorArg1", commError?.Arg1 ?? string.Empty);
        writer.WriteString("commErrorArg2", commError?.Arg2 ?? string.Empty);
        writer.WriteBoolean("autostart", autostart);
        writer.WriteNumber("timestamp", packet?.Timestamp.ToUnixTimeMilliseconds() ?? 0);
        writer.WriteEndObject();
    }

    private static string DescribeDevice(AppConfig config)
    {
        if (config.CommunicationType == CommunicationType.Ble)
        {
            return string.IsNullOrWhiteSpace(config.BleDeviceName) ? config.BleDeviceMac : config.BleDeviceName;
        }

        return config.ComPort;
    }
}
