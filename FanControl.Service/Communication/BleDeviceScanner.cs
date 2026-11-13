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
/// 实现改为 SetupAPI + BTHPORT 注册表（见 <see cref="BluetoothLeInterop"/>），不再依赖 WinRT 投影。
/// </summary>
public static class BleDeviceScanner
{
    /// <summary>枚举全部已配对的 BLE 设备；无蓝牙适配器或权限不足时返回空列表。</summary>
    public static async Task<IReadOnlyList<BleDeviceInfo>> EnumerateAsync(
        CancellationToken cancellationToken = default)
    {
        var results = new List<BleDeviceInfo>();

        IReadOnlyList<BleDeviceInterface> interfaces;
        try
        {
            interfaces = await Task.Run(BluetoothLeInterop.EnumerateDevices, cancellationToken);
        }
        catch (OperationCanceledException)
        {
            throw;
        }
        catch
        {
            return results; // 蓝牙不可用（无适配器 / 权限不足）
        }

        var seen = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        foreach (var item in interfaces)
        {
            if (string.IsNullOrWhiteSpace(item.MacAddress) && string.IsNullOrWhiteSpace(item.Name))
            {
                continue; // 既无 MAC 又无名称：无法在界面上定位
            }

            if (!string.IsNullOrWhiteSpace(item.MacAddress) && !seen.Add(item.MacAddress))
            {
                continue; // 同一个设备可能暴露多个接口（例如不同实例）
            }

            results.Add(new BleDeviceInfo(item.Name ?? string.Empty, item.MacAddress));
        }

        return results
            .OrderBy(d => d.Name, StringComparer.OrdinalIgnoreCase)
            .ThenBy(d => d.MacAddress, StringComparer.OrdinalIgnoreCase)
            .ToList();
    }
}
