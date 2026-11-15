using System.Runtime.InteropServices;
using System.Text;

namespace FanControl.Service.Communication;

/// <summary>FanControl.Ble.dll 的返回码，与 bleapi.h 的 FcBleStatus 一一对应。</summary>
internal enum BleNativeStatus
{
    Ok = 0,
    Unknown = 1,
    NotConfigured = 2,
    DeviceNotFound = 3,
    ServiceMissing = 4,
    CharacteristicMissing = 5,
    WriteFailed = 6,
    NotConnected = 7,
    Timeout = 8,
    BufferTooSmall = 9,

    /// <summary>FanControl.Ble.dll 缺失或版本不匹配（只在托管侧出现）。</summary>
    BridgeMissing = 100,
}

/// <summary>
/// C++/WinRT 蓝牙桥（见 src/FanControl.Ble.Native）的 P/Invoke 封装。
/// 用原生桥而不是 C# 的 WinRT 投影，是因为投影程序集有 24 MB；
/// 而 Win32 bluetoothleapis 在 Windows 11 上写 GATT 恒返回 ERROR_INVALID_FUNCTION。
/// </summary>
internal static class BleNative
{
    private const string Library = "FanControl.Ble.dll";

    internal static int Connect(string? macAddress, string? deviceName, int timeoutMs)
    {
        try
        {
            return ConnectNative(macAddress, deviceName, timeoutMs);
        }
        catch (DllNotFoundException)
        {
            return (int)BleNativeStatus.BridgeMissing;
        }
        catch (EntryPointNotFoundException)
        {
            return (int)BleNativeStatus.BridgeMissing;
        }
    }

    internal static int Disconnect()
    {
        try
        {
            return DisconnectNative();
        }
        catch (DllNotFoundException)
        {
            return (int)BleNativeStatus.BridgeMissing;
        }
        catch (EntryPointNotFoundException)
        {
            return (int)BleNativeStatus.BridgeMissing;
        }
    }

    internal static int Write(byte[] payload, int timeoutMs)
    {
        try
        {
            return WriteNative(payload, payload.Length, timeoutMs);
        }
        catch (DllNotFoundException)
        {
            return (int)BleNativeStatus.BridgeMissing;
        }
        catch (EntryPointNotFoundException)
        {
            return (int)BleNativeStatus.BridgeMissing;
        }
    }

    /// <summary>枚举已配对设备，返回 "名称\tMAC" 行文本；失败返回空串。</summary>
    internal static string Scan()
    {
        try
        {
            var needed = ScanNative(null, 0);
            if (needed <= 0)
            {
                return string.Empty;
            }

            var builder = new StringBuilder(needed);
            var written = ScanNative(builder, needed);
            return written > 0 ? builder.ToString() : string.Empty;
        }
        catch (DllNotFoundException)
        {
            return string.Empty;
        }
        catch (EntryPointNotFoundException)
        {
            return string.Empty;
        }
    }

    internal static string LastError()
    {
        try
        {
            var needed = LastErrorNative(null, 0);
            if (needed <= 0)
            {
                return string.Empty;
            }

            var builder = new StringBuilder(needed);
            LastErrorNative(builder, needed);
            return builder.ToString();
        }
        catch (DllNotFoundException)
        {
            return string.Empty;
        }
        catch (EntryPointNotFoundException)
        {
            return string.Empty;
        }
    }

    [DllImport(Library, EntryPoint = "fcble_scan", CallingConvention = CallingConvention.StdCall, CharSet = CharSet.Unicode)]
    private static extern int ScanNative(StringBuilder? buffer, int capChars);

    [DllImport(Library, EntryPoint = "fcble_connect", CallingConvention = CallingConvention.StdCall, CharSet = CharSet.Unicode)]
    private static extern int ConnectNative(string? macAddress, string? deviceName, int timeoutMs);

    [DllImport(Library, EntryPoint = "fcble_disconnect", CallingConvention = CallingConvention.StdCall)]
    private static extern int DisconnectNative();

    [DllImport(Library, EntryPoint = "fcble_write", CallingConvention = CallingConvention.StdCall)]
    private static extern int WriteNative(byte[] payload, int length, int timeoutMs);

    [DllImport(Library, EntryPoint = "fcble_last_error", CallingConvention = CallingConvention.StdCall, CharSet = CharSet.Unicode)]
    private static extern int LastErrorNative(StringBuilder? buffer, int capChars);
}
