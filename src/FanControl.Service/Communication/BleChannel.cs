using System.Text;
using FanControl.Service.Config;
using FanControl.Shared.Contracts;
using FanControl.Shared.Enums;
using Microsoft.Extensions.Logging;

namespace FanControl.Service.Communication;

/// <summary>
/// BLE 通信通道（Nordic UART Service，与固件约定）：
/// - RX 特征（6e400002）写入文本行：PWM:x / TIME:... / STATUS? / CPUx / GPUx
/// - TX 特征（6e400003）订阅通知，接收设备状态
/// 实现改为 Win32 bluetoothleapis（见 <see cref="BluetoothLeInterop"/>），不再依赖 WinRT 投影。
/// </summary>
public sealed class BleChannel : ICommunicationChannel
{
    private const string NusServiceUuid = "6e400001-b5a3-f393-e0a9-e50e24dcca9e";
    private const string NusRxUuid = "6e400002-b5a3-f393-e0a9-e50e24dcca9e";
    private const string NusTxUuid = "6e400003-b5a3-f393-e0a9-e50e24dcca9e";

    private static readonly TimeSpan ConnectTimeout = TimeSpan.FromSeconds(20);
    private static readonly TimeSpan WriteTimeout = TimeSpan.FromSeconds(5);

    private static readonly Guid NusService = new(NusServiceUuid);
    private static readonly Guid NusRx = new(NusRxUuid);
    private static readonly Guid NusTx = new(NusTxUuid);

    private readonly IConfigManager _configManager;
    private readonly ILogger<BleChannel> _logger;
    private readonly object _sync = new();
    private readonly SemaphoreSlim _writeLock = new(1, 1);
    private BleGattSession? _session;
    private byte[]? _rxCharacteristic;
    private bool _subscribeWarned;

    public BleChannel(IConfigManager configManager, ILogger<BleChannel> logger)
    {
        _configManager = configManager;
        _logger = logger;
    }

    public CommunicationType Type => CommunicationType.Ble;

    public bool IsConnected { get; private set; }

    public async Task ConnectAsync(CancellationToken cancellationToken = default)
    {
        var config = await _configManager.LoadAppConfigAsync(cancellationToken);
        var targetMac = config.BleDeviceMac;
        var deviceName = config.BleDeviceName;
        if (string.IsNullOrWhiteSpace(targetMac) && string.IsNullOrWhiteSpace(deviceName))
        {
            throw new FanControlException(ErrorInfo.Of("ble.notConfigured", "未配置 BLE 设备，请在设置中选择设备。"));
        }

        var result = await WithTimeoutAsync(
            () => ConnectCore(targetMac, deviceName, cancellationToken),
            ConnectTimeout,
            "连接",
            cancellationToken);

        BleGattSession? previous;
        lock (_sync)
        {
            previous = _session;
            _session = result.Session;
            _rxCharacteristic = result.RxCharacteristic;
            IsConnected = true;
        }

        previous?.Dispose();
        // 每次重连都写一条会让日志页在连不上时刷屏：连接细节降到 Debug，
        // 「链路可用」的信息由 FanControlRuntime 在写入成功时记一次。
        _logger.LogDebug(
            "BLE 已连接：{Name}（{Mac}），GATT 模块 {Module}",
            result.DisplayName,
            string.IsNullOrWhiteSpace(targetMac) ? "未配置" : targetMac,
            BluetoothLeInterop.GattModuleName ?? "未知");
    }

    public async Task SendAsync(PwmCommand command, CancellationToken cancellationToken = default)
    {
        ArgumentNullException.ThrowIfNull(command);
        await SendLineAsync($"PWM:{command.PwmPercent:0.0}\r\n", cancellationToken);
    }

    public async Task SendTimeAsync(DateTime time, CancellationToken cancellationToken = default)
    {
        await SendLineAsync($"TIME:{time:yyyy/MM/dd HH:mm:ss}\r\n", cancellationToken);
    }

    public async Task SendTemperaturesAsync(double cpuCelsius, double? gpuCelsius, CancellationToken cancellationToken = default)
    {
        await SendLineAsync($"CPU{cpuCelsius:0.0}\r\n", cancellationToken);
        if (gpuCelsius is double gpu)
        {
            await SendLineAsync($"GPU{gpu:0.0}\r\n", cancellationToken);
        }
    }

    public Task DisconnectAsync(CancellationToken cancellationToken = default)
    {
        BleGattSession? session;
        lock (_sync)
        {
            session = _session;
            _session = null;
            _rxCharacteristic = null;
            IsConnected = false;
        }

        try
        {
            session?.Dispose();
        }
        catch (Exception)
        {
            // 释放失败不影响退出
        }

        return Task.CompletedTask;
    }

    private (BleGattSession Session, byte[] RxCharacteristic, string DisplayName) ConnectCore(
        string targetMac,
        string deviceName,
        CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();

        var devices = BluetoothLeInterop.EnumerateDevices();
        var target = ResolveDevice(devices, targetMac, deviceName);

        if (target is null)
        {
            throw new FanControlException(string.IsNullOrWhiteSpace(targetMac)
                ? ErrorInfo.Of("ble.deviceNotFound", $"未找到已配对的蓝牙设备：{deviceName}", deviceName)
                : ErrorInfo.Of("ble.deviceNotFoundByMac", $"未找到已配对的蓝牙设备（MAC {targetMac}），请在设置中刷新后重新选择。", targetMac));
        }

        var displayName = !string.IsNullOrWhiteSpace(deviceName)
            ? deviceName
            : (string.IsNullOrWhiteSpace(target.Name) ? target.MacAddress : target.Name);

        var session = BluetoothLeInterop.Open(target.Path);
        try
        {
            if (session.FindService(NusService) is null)
            {
                throw new FanControlException(ErrorInfo.Of("ble.nusServiceMissingFor", $"{displayName} 未提供 NUS 服务，请确认固件已启用 BLE 模式。", displayName));
            }

            var rx = session.FindCharacteristic(NusRx)
                ?? throw new FanControlException(ErrorInfo.Of("ble.nusCharacteristicMissing", "设备 NUS 特征不完整，请确认固件版本。"));
            var tx = session.FindCharacteristic(NusTx)
                ?? throw new FanControlException(ErrorInfo.Of("ble.nusCharacteristicMissing", "设备 NUS 特征不完整，请确认固件版本。"));

            // 通知内容本实现不使用；订阅失败只记警告，不因此断开（写入通道仍可用）
            if (!session.Subscribe(tx))
            {
                // 每次重连都会走到这里，只在首次提示，后续降到 Debug
                if (!_subscribeWarned)
                {
                    _subscribeWarned = true;
                    _logger.LogWarning("{Name} 订阅 TX 通知失败，继续使用写入通道。", displayName);
                }
                else
                {
                    _logger.LogDebug("{Name} 订阅 TX 通知失败（已提示过）。", displayName);
                }
            }

            return (session, rx, displayName);
        }
        catch
        {
            session.Dispose();
            throw;
        }
    }

    private async Task SendLineAsync(string line, CancellationToken cancellationToken)
    {
        byte[]? rx;
        BleGattSession? session;
        lock (_sync)
        {
            rx = _rxCharacteristic;
            session = _session;
            if (!IsConnected || rx is null || session is null)
            {
                throw new FanControlException(ErrorInfo.Of("ble.notConnected", "BLE 未连接。"));
            }
        }

        var payload = Encoding.UTF8.GetBytes(line);

        await _writeLock.WaitAsync(cancellationToken);
        try
        {
            var target = session;
            var characteristic = rx;
            await WithTimeoutAsync(
                () =>
                {
                    target.Write(characteristic, payload);
                    return true;
                },
                WriteTimeout,
                "写入",
                cancellationToken);
        }
        finally
        {
            _writeLock.Release();
        }
    }

    /// <summary>
    /// 设备定位：优先按 MAC 地址匹配（唯一）；未配置 MAC 时回退按名称匹配（兼容旧配置）。
    /// </summary>
    private static BleDeviceInterface? ResolveDevice(
        IReadOnlyList<BleDeviceInterface> devices,
        string targetMac,
        string deviceName)
    {
        if (!string.IsNullOrWhiteSpace(targetMac))
        {
            foreach (var device in devices)
            {
                if (BleDeviceMatcher.MacEquals(device.MacAddress, targetMac))
                {
                    return device;
                }
            }

            return null;
        }

        foreach (var device in devices)
        {
            if (!string.IsNullOrEmpty(device.Name)
                && string.Equals(device.Name, deviceName, StringComparison.OrdinalIgnoreCase))
            {
                return device;
            }
        }

        return null;
    }

    private static async Task<T> WithTimeoutAsync<T>(
        Func<T> work,
        TimeSpan timeout,
        string operation,
        CancellationToken cancellationToken)
    {
        // GATT 调用是同步阻塞的（服务发现可达数十秒），放到线程池并加超时，避免拖住采样循环
        var task = Task.Run(work, cancellationToken);
        var completed = await Task.WhenAny(task, Task.Delay(timeout, cancellationToken));
        if (completed != task)
        {
            throw new FanControlException(ErrorInfo.Of("ble.timeout", $"BLE {operation}超时（{(int)timeout.TotalSeconds}s）。", ((int)timeout.TotalSeconds).ToString(), operation));
        }

        return await task;
    }
}
