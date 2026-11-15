using System.Text;
using FanControl.Service.Config;
using FanControl.Shared.Contracts;
using FanControl.Shared.Enums;
using Microsoft.Extensions.Logging;

namespace FanControl.Service.Communication;

/// <summary>
/// BLE 通信通道（Nordic UART Service，与固件约定）：
/// - RX 特征（6e400002）写入文本行：PWM:x / TIME:... / STATUS? / CPUx / GPUx
/// - TX 特征（6e400003）订阅通知，本实现不使用其内容，只作为链路可写的前置条件
/// GATT 调用全部在 FanControl.Ble.dll（C++/WinRT）里完成：C# 的 WinRT 投影要拖 24 MB 的
/// Microsoft.Windows.SDK.NET.dll，而 Win32 bluetoothleapis 在 Windows 11 上写 RX 恒返回
/// ERROR_INVALID_FUNCTION（用官方头文件写的原生探针同样失败）。
/// </summary>
public sealed class BleChannel : ICommunicationChannel
{
    private static readonly TimeSpan ConnectTimeout = TimeSpan.FromSeconds(20);
    private static readonly TimeSpan WriteTimeout = TimeSpan.FromSeconds(5);

    private readonly IConfigManager _configManager;
    private readonly ILogger<BleChannel> _logger;
    private readonly SemaphoreSlim _writeLock = new(1, 1);

    public BleChannel(IConfigManager configManager, ILogger<BleChannel> logger)
    {
        _configManager = configManager;
        _logger = logger;
    }

    public CommunicationType Type => CommunicationType.Ble;

    public bool IsConnected { get; private set; }

    public async Task ConnectAsync(CancellationToken cancellationToken = default)
    {
        var config = await _configManager.LoadAppConfigAsync(cancellationToken).ConfigureAwait(false);
        var targetMac = config.BleDeviceMac;
        var deviceName = config.BleDeviceName;
        if (string.IsNullOrWhiteSpace(targetMac) && string.IsNullOrWhiteSpace(deviceName))
        {
            throw new FanControlException(ErrorInfo.Of("ble.notConfigured", "未配置 BLE 设备，请在设置中选择设备。"));
        }

        var displayName = string.IsNullOrWhiteSpace(deviceName) ? targetMac : deviceName;
        var timeoutMs = (int)ConnectTimeout.TotalMilliseconds;
        var status = await Task.Run(() => BleNative.Connect(targetMac, deviceName, timeoutMs), cancellationToken)
            .ConfigureAwait(false);

        if (status != (int)BleNativeStatus.Ok)
        {
            throw CreateConnectException(status, targetMac, deviceName, displayName);
        }

        IsConnected = true;

        // 每次重连都写一条会让日志页在连不上时刷屏：连接细节降到 Debug，
        // 「链路可用」的信息由 FanControlRuntime 在写入成功时记一次。
        _logger.LogDebug(
            "BLE 已连接：{Name}（{Mac}），GATT 模块 {Module}",
            displayName,
            string.IsNullOrWhiteSpace(targetMac) ? "未配置" : targetMac,
            "WinRT (native)");
    }

    public async Task SendAsync(PwmCommand command, CancellationToken cancellationToken = default)
    {
        ArgumentNullException.ThrowIfNull(command);
        await SendLineAsync($"PWM:{command.PwmPercent:0.0}\r\n", cancellationToken).ConfigureAwait(false);
    }

    public async Task SendTimeAsync(DateTime time, CancellationToken cancellationToken = default)
    {
        await SendLineAsync($"TIME:{time:yyyy/MM/dd HH:mm:ss}\r\n", cancellationToken).ConfigureAwait(false);
    }

    public async Task SendTemperaturesAsync(double cpuCelsius, double? gpuCelsius, CancellationToken cancellationToken = default)
    {
        await SendLineAsync($"CPU{cpuCelsius:0.0}\r\n", cancellationToken).ConfigureAwait(false);
        if (gpuCelsius is double gpu)
        {
            await SendLineAsync($"GPU{gpu:0.0}\r\n", cancellationToken).ConfigureAwait(false);
        }
    }

    public Task DisconnectAsync(CancellationToken cancellationToken = default)
    {
        IsConnected = false;
        return Task.Run(BleNative.Disconnect, cancellationToken);
    }

    private FanControlException CreateConnectException(int status, string targetMac, string deviceName, string displayName)
    {
        var nativeError = BleNative.LastError();
        var suffix = string.IsNullOrWhiteSpace(nativeError) ? string.Empty : $"（{nativeError}）";
        return (BleNativeStatus)status switch
        {
            BleNativeStatus.NotConfigured => new FanControlException(ErrorInfo.Of("ble.notConfigured", "未配置 BLE 设备，请在设置中选择设备。")),
            BleNativeStatus.DeviceNotFound => new FanControlException(string.IsNullOrWhiteSpace(targetMac)
                ? ErrorInfo.Of("ble.deviceNotFound", $"未找到已配对的蓝牙设备：{deviceName}", deviceName)
                : ErrorInfo.Of("ble.deviceNotFoundByMac", $"未找到已配对的蓝牙设备（MAC {targetMac}），请在设置中刷新后重新选择。", targetMac)),
            BleNativeStatus.ServiceMissing => new FanControlException(ErrorInfo.Of(
                "ble.nusServiceMissingFor",
                $"{displayName} 未提供 NUS 服务，请确认固件已启用 BLE 模式。",
                displayName)),
            BleNativeStatus.CharacteristicMissing => new FanControlException(
                ErrorInfo.Of("ble.nusCharacteristicMissing", "设备 NUS 特征不完整，请确认固件版本。")),
            BleNativeStatus.Timeout => new FanControlException(ErrorInfo.Of(
                "ble.timeout",
                $"BLE 连接超时（{(int)ConnectTimeout.TotalSeconds}s）。",
                ((int)ConnectTimeout.TotalSeconds).ToString())),
            BleNativeStatus.BridgeMissing => new FanControlException(ErrorInfo.Of(
                "ble.bridgeMissing",
                "蓝牙组件缺失（FanControl.Ble.dll），请重新安装 FanControl。")),
            _ => new FanControlException(ErrorInfo.Of(
                "ble.connectFailed",
                $"BLE 连接失败（{status}）{suffix}。")),
        };
    }

    private async Task SendLineAsync(string line, CancellationToken cancellationToken)
    {
        if (!IsConnected)
        {
            throw new FanControlException(ErrorInfo.Of("ble.notConnected", "BLE 未连接。"));
        }

        var payload = Encoding.UTF8.GetBytes(line);
        var timeoutMs = (int)WriteTimeout.TotalMilliseconds;

        await _writeLock.WaitAsync(cancellationToken).ConfigureAwait(false);
        try
        {
            var status = await Task.Run(() => BleNative.Write(payload, timeoutMs), cancellationToken)
                .ConfigureAwait(false);

            switch ((BleNativeStatus)status)
            {
                case BleNativeStatus.Ok:
                    return;
                case BleNativeStatus.NotConnected:
                    IsConnected = false;
                    throw new FanControlException(ErrorInfo.Of("ble.notConnected", "BLE 未连接。"));
                case BleNativeStatus.Timeout:
                    throw new FanControlException(ErrorInfo.Of(
                        "ble.timeout",
                        $"BLE 写入超时（{(int)WriteTimeout.TotalSeconds}s）。",
                        ((int)WriteTimeout.TotalSeconds).ToString()));
                case BleNativeStatus.BridgeMissing:
                    throw new FanControlException(ErrorInfo.Of(
                        "ble.bridgeMissing",
                        "蓝牙组件缺失（FanControl.Ble.dll），请重新安装 FanControl。"));
                default:
                    var nativeError = BleNative.LastError();
                    throw new FanControlException(ErrorInfo.Of(
                        "ble.writeFailed",
                        $"BLE 写入失败（{status}）{(string.IsNullOrWhiteSpace(nativeError) ? string.Empty : $"：{nativeError}")}。",
                        status.ToString()));
            }
        }
        finally
        {
            _writeLock.Release();
        }
    }
}
