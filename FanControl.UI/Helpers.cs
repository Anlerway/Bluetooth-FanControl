using System.IO;

namespace FanControl.UI;

internal static class CrashLog
{
    public static void Write(string tag, Exception? ex)
    {
        try
        {
            var dir = Path.Combine(
                Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData),
                "FanControl",
                "Logs");
            Directory.CreateDirectory(dir);
            var path = Path.Combine(dir, $"fancontrol-crash-{DateTime.Now:yyyyMMdd-HHmmss}.log");
            File.WriteAllText(path, $"[{DateTime.Now:HH:mm:ss.fff}] {tag}{Environment.NewLine}{ex}{Environment.NewLine}");
        }
        catch
        {
            // 崩溃日志写入失败时不再尝试
        }
    }
}

/// <summary>温度显示单位换算。</summary>
internal static class TemperatureUnitHelper
{
    public static double ToDisplay(double celsius, FanControl.Shared.Enums.TemperatureUnit unit)
    {
        return unit == FanControl.Shared.Enums.TemperatureUnit.Fahrenheit
            ? celsius * 9.0 / 5.0 + 32.0
            : celsius;
    }

    public static string Suffix(FanControl.Shared.Enums.TemperatureUnit unit) =>
        unit == FanControl.Shared.Enums.TemperatureUnit.Fahrenheit ? "°F" : "°C";
}
