using FanControl.Shared.Contracts;
using Windows.Devices.Bluetooth;
using Windows.Devices.Enumeration;

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
/// </summary>
public static class BleDeviceScanner
{
    private static readonly TimeSpan EnumerationTimeout = TimeSpan.FromSeconds(5);
    private static readonly TimeSpan AddressTimeout = TimeSpan.FromSeconds(2);

    /// <summary>枚举全部 BLE 设备，解析各自的 MAC 地址（解析不到的 MAC 为空串）。</summary>
    public static async Task<IReadOnlyList<BleDeviceInfo>> EnumerateAsync(
        CancellationToken cancellationToken = default)
    {
        var results = new List<BleDeviceInfo>();

        DeviceInformationCollection devices;
        try
        {
            var enumeration = DeviceInformation
                .FindAllAsync(BluetoothLEDevice.GetDeviceSelector())
                .AsTask(cancellationToken);
            var finished = await Task.WhenAny(enumeration, Task.Delay(EnumerationTimeout, cancellationToken));
            if (finished != enumeration)
            {
                return results; // 枚举超时：返回空列表，保留调用方既有配置
            }

            devices = await enumeration;
        }
        catch
        {
            return results; // 蓝牙不可用（无适配器 / 权限不足）
        }

        foreach (var device in devices)
        {
            var mac = BleDeviceMatcher.ParseMacFromId(device.Id);
            if (mac.Length == 0)
            {
                mac = await TryReadMacAsync(device.Id, cancellationToken);
            }

            results.Add(new BleDeviceInfo(device.Name ?? string.Empty, mac));
        }

        return results
            .OrderBy(d => d.Name, StringComparer.OrdinalIgnoreCase)
            .ThenBy(d => d.MacAddress, StringComparer.OrdinalIgnoreCase)
            .ToList();
    }

    /// <summary>设备 ID 无法直接解析时，打开设备读取蓝牙地址作为兜底。</summary>
    private static async Task<string> TryReadMacAsync(string deviceId, CancellationToken cancellationToken)
    {
        try
        {
            var open = BluetoothLEDevice.FromIdAsync(deviceId).AsTask(cancellationToken);
            var finished = await Task.WhenAny(open, Task.Delay(AddressTimeout, cancellationToken));
            if (finished != open)
            {
                return string.Empty;
            }

            var device = await open;
            if (device is null)
            {
                return string.Empty;
            }

            var mac = BleDeviceMatcher.FormatMacAddress(device.BluetoothAddress);
            device.Dispose();
            return mac;
        }
        catch
        {
            return string.Empty;
        }
    }
}
