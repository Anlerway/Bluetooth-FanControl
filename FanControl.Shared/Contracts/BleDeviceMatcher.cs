namespace FanControl.Shared.Contracts;

/// <summary>
/// BLE 设备定位辅助：按 MAC 地址定位设备（名称只用于显示）。
/// Windows 的设备 ID 形如 "BluetoothLE#BluetoothLE{本机MAC}-{设备MAC}"，
/// 末端即为设备自身的 MAC 地址，可直接解析；格式异常时由调用方回退到
/// BluetoothLEDevice.BluetoothAddress。
/// </summary>
public static class BleDeviceMatcher
{
    private const string HexDigits = "0123456789ABCDEF";

    /// <summary>把 6 字节蓝牙地址格式化为 "AA:BB:CC:DD:EE:FF"（大写）。</summary>
    public static string FormatMacAddress(ulong address)
    {
        var buffer = new char[17]; // 6 字节十六进制 + 5 个分隔符
        for (var index = 0; index < 6; index++)
        {
            var value = (int)((address >> ((5 - index) * 8)) & 0xFF);
            buffer[index * 3] = HexDigits[value >> 4];
            buffer[(index * 3) + 1] = HexDigits[value & 0x0F];
            if (index < 5)
            {
                buffer[(index * 3) + 2] = ':';
            }
        }

        return new string(buffer);
    }

    /// <summary>比较两个 MAC 是否相同（忽略大小写与分隔符差异，兼容含/不含冒号两种写法）。</summary>
    public static bool MacEquals(string? left, string? right)
    {
        var a = NormalizeMac(left);
        var b = NormalizeMac(right);
        return a.Length > 0 && string.Equals(a, b, StringComparison.OrdinalIgnoreCase);
    }

    /// <summary>去掉分隔符并转大写：兼容 "AA:BB:CC:DD:EE:FF" / "aabbccddeeff" / "AA-BB-CC-DD-EE-FF"。</summary>
    public static string NormalizeMac(string? mac)
    {
        if (string.IsNullOrWhiteSpace(mac))
        {
            return string.Empty;
        }

        var buffer = new char[12];
        var count = 0;
        foreach (var ch in mac)
        {
            if (Uri.IsHexDigit(ch))
            {
                if (count == buffer.Length)
                {
                    return string.Empty; // 超过 6 字节，视为非法值
                }

                buffer[count++] = char.ToUpperInvariant(ch);
            }
            else if (ch is ':' or '-' or '.')
            {
                continue;
            }
            else
            {
                return string.Empty;
            }
        }

        return count == buffer.Length ? new string(buffer) : string.Empty;
    }

    /// <summary>
    /// 从 Windows 设备 ID 中解析设备 MAC：ID 形如
    /// "BluetoothLE#BluetoothLE{本机MAC}-{设备MAC}"，取最后一个可解析为 6 字节的段。
    /// </summary>
    public static string ParseMacFromId(string? deviceId)
    {
        if (string.IsNullOrWhiteSpace(deviceId))
        {
            return string.Empty;
        }

        var mac = string.Empty;
        var start = 0;
        for (var i = 0; i <= deviceId.Length; i++)
        {
            if (i < deviceId.Length && deviceId[i] is not ('-' or '#'))
            {
                continue;
            }

            var candidate = NormalizeMac(deviceId[start..i]);
            if (candidate.Length > 0)
            {
                mac = candidate;
            }

            start = i + 1;
        }

        return mac;
    }
}
