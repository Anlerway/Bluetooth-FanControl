using FanControl.Shared.Contracts;

namespace FanControl.Service.Communication;

/// <summary>枚举到的 BLE 设备：MAC 为定位主键，名称为显示名称。</summary>
public sealed record BleDeviceInfo(string Name, string MacAddress)
{
    /// <summary>界面用显示文本："设备名 (AA:BB:CC:DD:EE:FF)"，名称缺失时退化为 MAC 或占位符。</summary>
    public string Display =>
        string.IsNullOrWhiteSpace(Name)
            ? (string.IsNullOrWhiteSpace(MacAddress) ? "(未知设备)" : MacAddress)
            : (string.IsNullOrWhiteSpace(MacAddress) ? Name : $"{Name} ({MacAddress})");

    public override string ToString() => Display;
}

/// <summary>
/// BLE 设备枚举（UI 下拉列表与后台通道共用）：
/// 以 MAC 地址作为设备定位主键，名称仅用于显示。
/// 枚举本身走 FanControl.Ble.dll（C++/WinRT），此处只解析 "名称\tMAC" 行文本。
/// </summary>
public static class BleDeviceScanner
{
    /// <summary>枚举全部已配对的 BLE 设备；无蓝牙适配器或权限不足时返回空列表。</summary>
    public static Task<IReadOnlyList<BleDeviceInfo>> EnumerateAsync(CancellationToken cancellationToken = default)
    {
        return Task.Run(() => Parse(BleNative.Scan()), cancellationToken);
    }

    private static IReadOnlyList<BleDeviceInfo> Parse(string text)
    {
        var results = new List<BleDeviceInfo>();
        if (string.IsNullOrWhiteSpace(text))
        {
            return results;
        }

        var seen = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        foreach (var rawLine in text.Split('\n'))
        {
            var line = rawLine.TrimEnd('\r');
            if (line.Length == 0)
            {
                continue;
            }

            var separator = line.IndexOf('\t');
            var name = separator < 0 ? line : line[..separator];
            var mac = separator < 0 ? string.Empty : line[(separator + 1)..];
            name = name.Trim();
            mac = mac.Trim();

            if (name.Length == 0 && mac.Length == 0)
            {
                continue;
            }

            if (mac.Length > 0 && !seen.Add(mac))
            {
                continue;
            }

            results.Add(new BleDeviceInfo(name, mac));
        }

        return results
            .OrderBy(d => d.Name, StringComparer.OrdinalIgnoreCase)
            .ThenBy(d => d.MacAddress, StringComparer.OrdinalIgnoreCase)
            .ToList();
    }
}
