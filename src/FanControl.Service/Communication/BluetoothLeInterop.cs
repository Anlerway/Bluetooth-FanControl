using System.ComponentModel;
using System.Runtime.InteropServices;
using FanControl.Shared.Contracts;
using Microsoft.Win32;

namespace FanControl.Service.Communication;

/// <summary>枚举到的 BLE 设备：Path 可直接交给 CreateFile 取得 GATT 句柄。</summary>
internal sealed record BleDeviceInterface(string Path, string MacAddress, string Name);

/// <summary>
/// BLE 的 Win32 互操作层，替代 Windows.Devices.Bluetooth 的 WinRT 投影
/// （Microsoft.Windows.SDK.NET.dll + WinRT.Runtime.dll 单独占发布包约 24 MB）。
///
/// 实测（Windows 11，2026-11）：
/// - 设备级接口类为 {781aee18-…}，路径形如 \\?\bthle#dev_MAC#…#{781aee18-…}，CreateFile 只能开这一类；
///   每个 GATT 服务另有 {6e3bb679-…} 的接口，但打开会返回 ERROR_SHARING_VIOLATION(32)。
/// - BluetoothGATTGetServices 的服务结构体为 24 字节；BluetoothGATTGetCharacteristics 的
///   特征结构体实测为 36 字节（不是文档推算的 32），因此一律按 64 字节留余量、按原始字节回传。
///
/// 回退方式：git checkout 960d4ff -- FanControl.Service/Communication/BleChannel.cs BleDeviceScanner.cs
/// 并把 TargetFramework 改回 net8.0-windows10.0.19041.0。
/// </summary>
internal static class BluetoothLeInterop
{
    /// <summary>BLE 设备接口类 GUID（设备级，实测可 CreateFile）。</summary>
    public static readonly Guid DeviceInterfaceClass = new("781aee18-7733-4ce4-add0-91f41c67b592");

    private const uint DigcfPresent = 0x00000002;
    private const uint DigcfDeviceInterface = 0x00000010;
    private const uint GenericRead = 0x80000000;
    private const uint GenericWrite = 0x40000000;
    private const uint FileShareRead = 0x00000001;
    private const uint FileShareWrite = 0x00000002;
    private const uint OpenExisting = 3;

    private static readonly IntPtr InvalidHandleValue = new(-1);
    private static readonly int DetailHeaderSize = IntPtr.Size == 8 ? 8 : 6;
    private static readonly Lazy<BleGattApi?> Api = new(BleGattApi.Load);

    public static bool IsAvailable => Api.Value is not null;

    public static string? GattModuleName => Api.Value?.ModuleName;

    /// <summary>枚举已配对的 BLE 设备（SetupAPI 只列出配对/安装过的设备）。</summary>
    public static IReadOnlyList<BleDeviceInterface> EnumerateDevices()
    {
        var results = new List<BleDeviceInterface>();
        var classGuid = DeviceInterfaceClass;
        var deviceInfoSet = SetupDiGetClassDevsW(
            ref classGuid,
            IntPtr.Zero,
            IntPtr.Zero,
            DigcfPresent | DigcfDeviceInterface);

        if (deviceInfoSet == InvalidHandleValue)
        {
            return results;
        }

        try
        {
            for (uint index = 0; ; index++)
            {
                var interfaceData = new SpDeviceInterfaceData { Size = Marshal.SizeOf<SpDeviceInterfaceData>() };
                if (!SetupDiEnumDeviceInterfaces(deviceInfoSet, IntPtr.Zero, ref classGuid, index, ref interfaceData))
                {
                    break; // ERROR_NO_MORE_ITEMS
                }

                var deviceInfo = new SpDevInfoData { Size = Marshal.SizeOf<SpDevInfoData>() };

                // 取尺寸的调用必然返回 FALSE + ERROR_INSUFFICIENT_BUFFER(122)，只以 requiredSize 判定成败
                _ = SetupDiGetDeviceInterfaceDetailW(
                    deviceInfoSet, ref interfaceData, IntPtr.Zero, 0, out var requiredSize, ref deviceInfo);

                if (requiredSize == 0)
                {
                    continue;
                }

                var buffer = Marshal.AllocHGlobal((int)requiredSize);
                try
                {
                    // SP_DEVICE_INTERFACE_DETAIL_DATA_W.cbSize：x64 为 8（对齐后），字符串仍从偏移 4 开始
                    Marshal.WriteInt32(buffer, DetailHeaderSize);
                    if (!SetupDiGetDeviceInterfaceDetailW(
                            deviceInfoSet, ref interfaceData, buffer, requiredSize, out _, ref deviceInfo))
                    {
                        continue;
                    }

                    var path = Marshal.PtrToStringUni(IntPtr.Add(buffer, 4));
                    if (string.IsNullOrEmpty(path))
                    {
                        continue;
                    }

                    var instanceId = ReadInstanceId(deviceInfoSet, ref deviceInfo);
                    var mac = BleDeviceMatcher.ParseMacFromDevicePath(instanceId);
                    var name = mac.Length == 0 ? string.Empty : ReadDeviceName(mac);
                    results.Add(new BleDeviceInterface(path, mac, name));
                }
                finally
                {
                    Marshal.FreeHGlobal(buffer);
                }
            }
        }
        finally
        {
            SetupDiDestroyDeviceInfoList(deviceInfoSet);
        }

        return results;
    }

    /// <summary>打开设备级接口取得 GATT 句柄。</summary>
    public static BleGattSession Open(string devicePath)
    {
        var api = Api.Value
            ?? throw new FanControlException(ErrorInfo.Of("ble.gattUnavailable", "系统未提供 BLE GATT 接口（bluetoothleapis 导出缺失），无法使用蓝牙通信。"));

        var handle = CreateFileW(
            devicePath,
            GenericRead | GenericWrite,
            FileShareRead | FileShareWrite,
            IntPtr.Zero,
            OpenExisting,
            0,
            IntPtr.Zero);

        if (handle == InvalidHandleValue)
        {
            var error = Marshal.GetLastWin32Error();
            throw new FanControlException(
                ErrorInfo.Of("ble.openFailed", "打开 BLE 设备失败（Win32 " + error + "）：" + devicePath, error.ToString()),
                new Win32Exception(error));
        }

        return new BleGattSession(handle, api);
    }

    /// <summary>BTHPORT 注册表里的配对设备名（UTF-8 字节串，需管理员权限，取不到则返回空）。</summary>
    public static string ReadDeviceName(string mac)
    {
        var key = BleDeviceMatcher.NormalizeMac(mac);
        if (key.Length != 12)
        {
            return string.Empty;
        }

        try
        {
            using var devices = Registry.LocalMachine.OpenSubKey(@"SYSTEM\CurrentControlSet\Services\BTHPORT\Parameters\Devices");
            using var device = devices?.OpenSubKey(key);
            if (device?.GetValue("Name") is byte[] raw && raw.Length > 0)
            {
                return System.Text.Encoding.UTF8.GetString(raw).TrimEnd('\0').Trim();
            }
        }
        catch (Exception)
        {
            // 权限不足或键不存在：名称降级为空，界面回退显示 MAC
        }

        return string.Empty;
    }

    private static string ReadInstanceId(IntPtr deviceInfoSet, ref SpDevInfoData deviceInfo)
    {
        var buffer = new char[512];
        if (!SetupDiGetDeviceInstanceIdW(deviceInfoSet, ref deviceInfo, buffer, (uint)buffer.Length, out var required))
        {
            return string.Empty;
        }

        var length = (int)required;
        if (length <= 1 || length > buffer.Length)
        {
            return string.Empty;
        }

        return new string(buffer, 0, length - 1);
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct SpDeviceInterfaceData
    {
        public int Size;
        public Guid InterfaceClassGuid;
        public int Flags;
        public IntPtr Reserved;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct SpDevInfoData
    {
        public int Size;
        public Guid ClassGuid;
        public int DevInst;
        public IntPtr Reserved;
    }

    [DllImport("setupapi.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern IntPtr SetupDiGetClassDevsW(ref Guid classGuid, IntPtr enumerator, IntPtr parent, uint flags);

    [DllImport("setupapi.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern bool SetupDiEnumDeviceInterfaces(
        IntPtr deviceInfoSet,
        IntPtr deviceInfo,
        ref Guid interfaceClassGuid,
        uint memberIndex,
        ref SpDeviceInterfaceData deviceInterfaceData);

    [DllImport("setupapi.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern bool SetupDiGetDeviceInterfaceDetailW(
        IntPtr deviceInfoSet,
        ref SpDeviceInterfaceData deviceInterfaceData,
        IntPtr deviceInterfaceDetailData,
        uint deviceInterfaceDetailDataSize,
        out uint requiredSize,
        ref SpDevInfoData deviceInfoData);

    [DllImport("setupapi.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern bool SetupDiGetDeviceInstanceIdW(
        IntPtr deviceInfoSet,
        ref SpDevInfoData deviceInfoData,
        [Out] char[] deviceInstanceId,
        uint deviceInstanceIdSize,
        out uint requiredSize);

    [DllImport("setupapi.dll", SetLastError = true)]
    private static extern bool SetupDiDestroyDeviceInfoList(IntPtr deviceInfoSet);

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern IntPtr CreateFileW(
        string fileName,
        uint desiredAccess,
        uint shareMode,
        IntPtr securityAttributes,
        uint creationDisposition,
        uint flagsAndAttributes,
        IntPtr templateFile);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool CloseHandle(IntPtr handle);

    internal static void CloseDeviceHandle(IntPtr handle) => CloseHandle(handle);
}

/// <summary>
/// bluetoothleapis 的导出绑定：GATT 函数并不固定在某个模块里（历史上分散在 btdrt.dll / BluetoothApis.dll），
/// 因此按候选模块逐个探测导出；本机实测命中 BluetoothApis.dll。
/// </summary>
internal sealed class BleGattApi
{
    internal delegate int GetServicesDelegate(IntPtr device, ushort count, IntPtr buffer, out ushort actual, uint flags);

    internal delegate int GetCharacteristicsDelegate(IntPtr device, IntPtr service, ushort count, IntPtr buffer, out ushort actual, uint flags);

    internal delegate int SetCharacteristicValueDelegate(IntPtr device, IntPtr characteristic, IntPtr value, IntPtr reliableWriteContext, uint flags);

    internal delegate int RegisterEventDelegate(
        IntPtr device,
        int eventType,
        IntPtr eventParameter,
        IntPtr callback,
        IntPtr context,
        out IntPtr eventHandle,
        uint flags);

    internal delegate int UnregisterEventDelegate(IntPtr eventHandle, uint flags);

    private static readonly string[] ModuleCandidates = { "btdrt.dll", "BluetoothApis.dll", "bthprops.cpl" };

    private BleGattApi(
        string moduleName,
        GetServicesDelegate getServices,
        GetCharacteristicsDelegate getCharacteristics,
        SetCharacteristicValueDelegate setCharacteristicValue,
        RegisterEventDelegate registerEvent,
        UnregisterEventDelegate unregisterEvent)
    {
        ModuleName = moduleName;
        GetServices = getServices;
        GetCharacteristics = getCharacteristics;
        SetCharacteristicValue = setCharacteristicValue;
        RegisterEvent = registerEvent;
        UnregisterEvent = unregisterEvent;
    }

    public string ModuleName { get; }

    public GetServicesDelegate GetServices { get; }

    public GetCharacteristicsDelegate GetCharacteristics { get; }

    public SetCharacteristicValueDelegate SetCharacteristicValue { get; }

    public RegisterEventDelegate RegisterEvent { get; }

    public UnregisterEventDelegate UnregisterEvent { get; }

    internal static BleGattApi? Load()
    {
        foreach (var moduleName in ModuleCandidates)
        {
            if (!NativeLibrary.TryLoad(moduleName, out var module))
            {
                continue;
            }

            if (TryExport<GetServicesDelegate>(module, "BluetoothGATTGetServices", out var getServices)
                && TryExport<GetCharacteristicsDelegate>(module, "BluetoothGATTGetCharacteristics", out var getCharacteristics)
                && TryExport<SetCharacteristicValueDelegate>(module, "BluetoothGATTSetCharacteristicValue", out var setValue)
                && TryExport<RegisterEventDelegate>(module, "BluetoothGATTRegisterEvent", out var registerEvent)
                && TryExport<UnregisterEventDelegate>(module, "BluetoothGATTUnregisterEvent", out var unregisterEvent))
            {
                return new BleGattApi(moduleName, getServices!, getCharacteristics!, setValue!, registerEvent!, unregisterEvent!);
            }

            NativeLibrary.Free(module);
        }

        return null;
    }

    private static bool TryExport<T>(IntPtr module, string name, out T? export)
        where T : Delegate
    {
        if (NativeLibrary.TryGetExport(module, name, out var address))
        {
            export = Marshal.GetDelegateForFunctionPointer<T>(address);
            return true;
        }

        export = null;
        return false;
    }
}

/// <summary>
/// 一个 BLE 设备上的 GATT 会话。Win32 结构体布局与文档推算有出入（服务 24 字节、特征实测 36 字节），
/// 因此这里只保存 API 返回的原始字节块，定位条目靠 UUID 扫描，调用时把整块原样传回。
/// </summary>
internal sealed class BleGattSession : IDisposable
{
    /// <summary>BTH_LE_GATT_SERVICE 实测 24 字节：BTH_LE_UUID(20) + AttributeHandle(2)。</summary>
    private const int ServiceStride = 24;

    /// <summary>BTH_LE_GATT_CHARACTERISTIC 实测 36 字节，按 64 留余量分配，避免 API 越界写堆。</summary>
    private const int CharacteristicBufferSize = 64;

    private const int CharacteristicValueChangedEvent = 0;

    [UnmanagedFunctionPointer(CallingConvention.Winapi)]
    private delegate void GattEventCallback(IntPtr eventHandle, int eventType, IntPtr eventParameter, IntPtr context);

    private readonly IntPtr _handle;
    private readonly BleGattApi _api;
    private readonly GattEventCallback _callback;
    private byte[]? _serviceRaw;
    private ushort _serviceHandle;
    private IntPtr _eventHandle;
    private bool _disposed;

    internal BleGattSession(IntPtr handle, BleGattApi api)
    {
        _handle = handle;
        _api = api;
        _callback = OnNotification;
    }

    /// <summary>GATT 服务条目：Raw 为该结构体的原始字节，可原样回传给 Win32 API。</summary>
    internal readonly record struct GattServiceEntry(ushort AttributeHandle, Guid Uuid, byte[]? Raw);

    /// <summary>GATT 特征条目：Raw 为 64 字节窗口（含完整的 36 字节结构体）。</summary>
    internal readonly record struct GattCharacteristicEntry(Guid Uuid, ushort AttributeHandle, byte[]? Raw);

    /// <summary>查找服务并记住其原始结构体（后续取特征需要该指针）。</summary>
    internal ushort? FindService(Guid uuid)
    {
        foreach (var service in EnumerateServices())
        {
            if (service.Uuid != uuid || service.Raw is null)
            {
                continue;
            }

            _serviceRaw = service.Raw;
            _serviceHandle = service.AttributeHandle;
            return _serviceHandle;
        }

        return null;
    }

    /// <summary>在已选服务下查找特征，返回可直接回传的原始字节窗口。</summary>
    internal byte[]? FindCharacteristic(Guid uuid)
    {
        if (_serviceRaw is null)
        {
            throw new FanControlException(ErrorInfo.Of("ble.nusServiceMissing", "尚未定位 NUS 服务。"));
        }

        foreach (var characteristic in EnumerateCharacteristics(new GattServiceEntry(_serviceHandle, Guid.Empty, _serviceRaw)))
        {
            if (characteristic.Uuid == uuid)
            {
                return characteristic.Raw;
            }
        }

        return null;
    }

    /// <summary>写入特征值（带响应写）。</summary>
    internal void Write(byte[] characteristic, byte[] payload)
    {
        ArgumentNullException.ThrowIfNull(characteristic);
        ArgumentNullException.ThrowIfNull(payload);

        var characteristicPtr = Marshal.AllocHGlobal(characteristic.Length);
        var valuePtr = Marshal.AllocHGlobal(4 + Math.Max(payload.Length, 1));
        try
        {
            Marshal.Copy(characteristic, 0, characteristicPtr, characteristic.Length);

            // BTH_LE_GATT_CHARACTERISTIC_VALUE { ULONG DataSize; UCHAR Data[]; }
            Marshal.WriteInt32(valuePtr, payload.Length);
            if (payload.Length > 0)
            {
                Marshal.Copy(payload, 0, IntPtr.Add(valuePtr, 4), payload.Length);
            }

            var hr = _api.SetCharacteristicValue(_handle, characteristicPtr, valuePtr, IntPtr.Zero, 0);
            if (hr < 0)
            {
                throw new FanControlException(ErrorInfo.Of("ble.writeFailed", "BLE 写入失败（HRESULT 0x" + hr.ToString("X8") + "）。", "0x" + hr.ToString("X8")));
            }
        }
        finally
        {
            Marshal.FreeHGlobal(characteristicPtr);
            Marshal.FreeHGlobal(valuePtr);
        }
    }

    /// <summary>订阅特征通知（注册事件本身即会打开 CCCD 通知位）。</summary>
    internal bool Subscribe(byte[] characteristic)
    {
        ArgumentNullException.ThrowIfNull(characteristic);

        var characteristicPtr = Marshal.AllocHGlobal(characteristic.Length);
        try
        {
            Marshal.Copy(characteristic, 0, characteristicPtr, characteristic.Length);
            var callbackPtr = Marshal.GetFunctionPointerForDelegate(_callback);
            var hr = _api.RegisterEvent(
                _handle,
                CharacteristicValueChangedEvent,
                characteristicPtr,
                callbackPtr,
                IntPtr.Zero,
                out var eventHandle,
                0);

            if (hr < 0)
            {
                return false;
            }

            _eventHandle = eventHandle;
            return true;
        }
        finally
        {
            Marshal.FreeHGlobal(characteristicPtr);
        }
    }

    /// <summary>列出全部服务（诊断/自检用）。</summary>
    internal IReadOnlyList<GattServiceEntry> EnumerateServices()
    {
        var results = new List<GattServiceEntry>();
        var count = QueryServiceCount();
        if (count == 0)
        {
            return results;
        }

        var size = count * ServiceStride;
        var buffer = Marshal.AllocHGlobal(size);
        try
        {
            Clear(buffer, size);
            var hr = _api.GetServices(_handle, count, buffer, out var actual, 0);
            if (hr < 0 || actual == 0)
            {
                return results;
            }

            var raw = new byte[size];
            Marshal.Copy(buffer, raw, 0, size);
            for (var index = 0; index < actual; index++)
            {
                var offset = index * ServiceStride;
                var entryRaw = new byte[ServiceStride];
                Array.Copy(raw, offset, entryRaw, 0, ServiceStride);
                results.Add(new GattServiceEntry(
                    ReadUInt16(raw, offset + 20),
                    ReadUuid(raw, offset + 4),
                    entryRaw));
            }
        }
        finally
        {
            Marshal.FreeHGlobal(buffer);
        }

        return results;
    }

    /// <summary>列出某个服务下的全部特征（诊断/自检用）。</summary>
    internal IReadOnlyList<GattCharacteristicEntry> EnumerateCharacteristics(GattServiceEntry service)
    {
        var results = new List<GattCharacteristicEntry>();
        if (service.Raw is null)
        {
            return results;
        }

        var count = QueryCharacteristicCount(service.Raw);
        if (count == 0)
        {
            return results;
        }

        var size = count * CharacteristicBufferSize;
        var servicePtr = Marshal.AllocHGlobal(service.Raw.Length);
        var buffer = Marshal.AllocHGlobal(size);
        try
        {
            Marshal.Copy(service.Raw, 0, servicePtr, service.Raw.Length);
            Clear(buffer, size);
            var hr = _api.GetCharacteristics(_handle, servicePtr, count, buffer, out var actual, 0);
            if (hr < 0 || actual == 0)
            {
                return results;
            }

            var raw = new byte[size];
            Marshal.Copy(buffer, raw, 0, size);

            // 条目起点 = ServiceHandle 命中且 IsShortUuid 合法
            var starts = new List<int>();
            for (var offset = 0; offset + 36 <= raw.Length; offset++)
            {
                if (ReadUInt16(raw, offset) != service.AttributeHandle || raw[offset + 4] > 1)
                {
                    continue;
                }

                starts.Add(offset);
            }

            for (var index = 0; index < starts.Count; index++)
            {
                var start = starts[index];
                var window = new byte[CharacteristicBufferSize];
                Array.Copy(raw, start, window, 0, Math.Min(CharacteristicBufferSize, raw.Length - start));
                results.Add(new GattCharacteristicEntry(
                    ReadUuid(raw, start + 8),
                    ReadUInt16(raw, start + 24),
                    window));
            }
        }
        finally
        {
            Marshal.FreeHGlobal(buffer);
            Marshal.FreeHGlobal(servicePtr);
        }

        return results;
    }

    public void Dispose()
    {
        if (_disposed)
        {
            return;
        }

        _disposed = true;

        if (_eventHandle != IntPtr.Zero)
        {
            try
            {
                _api.UnregisterEvent(_eventHandle, 0);
            }
            catch (Exception)
            {
                // 注销失败不阻塞释放
            }

            _eventHandle = IntPtr.Zero;
        }

        BluetoothLeInterop.CloseDeviceHandle(_handle);
    }

    private ushort QueryServiceCount()
    {
        // 缓冲区为 0 时返回 ERROR_MORE_DATA，但 actualCount 已给出总数
        _ = _api.GetServices(_handle, 0, IntPtr.Zero, out var count, 0);
        return count;
    }

    private ushort QueryCharacteristicCount(byte[] serviceRaw)
    {
        var servicePtr = Marshal.AllocHGlobal(serviceRaw.Length);
        try
        {
            Marshal.Copy(serviceRaw, 0, servicePtr, serviceRaw.Length);
            _ = _api.GetCharacteristics(_handle, servicePtr, 0, IntPtr.Zero, out var count, 0);
            return count;
        }
        finally
        {
            Marshal.FreeHGlobal(servicePtr);
        }
    }

    private void OnNotification(IntPtr eventHandle, int eventType, IntPtr eventParameter, IntPtr context)
    {
        // 协议只下发、不依赖回调内容（设备状态由采样周期自行查询），保持空实现避免额外解析。
    }

    private static void Clear(IntPtr buffer, int size)
    {
        for (var offset = 0; offset < size; offset++)
        {
            Marshal.WriteByte(buffer, offset, 0);
        }
    }

    private static ushort ReadUInt16(byte[] buffer, int offset)
        => (ushort)(buffer[offset] | (buffer[offset + 1] << 8));

    private static Guid ReadUuid(byte[] raw, int uuidOffset)
    {
        if (raw[uuidOffset - 4] != 0)
        {
            return new Guid("0000" + ReadUInt16(raw, uuidOffset).ToString("x4") + "-0000-1000-8000-00805f9b34fb");
        }

        return new Guid(raw.AsSpan(uuidOffset, 16));
    }
}
