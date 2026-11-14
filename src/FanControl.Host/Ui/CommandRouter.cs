using System.IO.Ports;
using System.Text.Encodings.Web;
using System.Text.Json;
using FanControl.Service.Communication;
using FanControl.Service.Host;
using FanControl.Service.Tray;
using FanControl.Shared.Contracts;
using FanControl.Shared.Models;
using Microsoft.Extensions.Logging;

namespace FanControl.Host.Ui;

/// <summary>
/// 界面命令路由。单一进程后没有管道了，界面把动作序列化成与 1.4.0 管道完全相同的
/// JSON 交过来（set / setCurve / setSystem / reconnect …），这里解析后照旧落配置。
/// 状态由 <see cref="PushLoopAsync"/> 每 400 ms 推一帧给界面。
/// </summary>
internal sealed class CommandRouter : IAsyncDisposable
{
    private const int ProtocolVersion = 1;
    private const int PushIntervalMilliseconds = 400;

    private static readonly JsonSerializerOptions ReplyOptions = new() { Encoder = JavaScriptEncoder.UnsafeRelaxedJsonEscaping };

    private readonly FanControlRuntime _runtime;
    private readonly SystemSettings _system;
    private readonly ILogger _logger;
    private readonly CancellationTokenSource _cts = new();

    private Task? _pushLoop;
    private volatile bool _autostart;

    public CommandRouter(FanControlRuntime runtime, SystemSettings system, ILogger logger)
    {
        _runtime = runtime;
        _system = system;
        _logger = logger;
    }

    public void Start()
    {
        _ = Task.Run(RefreshAutostartAsync);
        _pushLoop = Task.Run(() => PushLoopAsync(_cts.Token));
    }

    /// <summary>界面刚打开时先给一帧完整快照。</summary>
    public void PushSnapshot() => Send(StateJson.BuildState(_runtime, _autostart, _system.Current));

    private async Task PushLoopAsync(CancellationToken cancellationToken)
    {
        while (!cancellationToken.IsCancellationRequested)
        {
            try
            {
                await Task.Delay(PushIntervalMilliseconds, cancellationToken).ConfigureAwait(false);
            }
            catch (OperationCanceledException)
            {
                break;
            }

            // 界面没开就不生成 JSON：一次性进程里唯一消费者就是那个窗口。
            if (!UiBridge.IsRunning)
            {
                continue;
            }

            Send(StateJson.BuildState(_runtime, _autostart, _system.Current));
        }
    }

    public async Task HandleAsync(string json)
    {
        JsonDocument document;
        try
        {
            document = JsonDocument.Parse(json);
        }
        catch (JsonException)
        {
            Send(Error(ErrorInfo.Of("ipc.badJson", "请求不是合法 JSON。")));
            return;
        }

        using (document)
        {
            var root = document.RootElement;
            var command = root.TryGetProperty("cmd", out var cmdElement) && cmdElement.ValueKind == JsonValueKind.String
                ? cmdElement.GetString()
                : null;

            switch (command)
            {
                case "hello":
                    Send(Reply(new
                    {
                        @event = "hello",
                        protocol = ProtocolVersion,
                        version = typeof(CommandRouter).Assembly.GetName().Version?.ToString(3) ?? "0.0.0",
                    }));
                    break;

                case "getState":
                    Send(StateJson.BuildState(_runtime, _autostart, _system.Current));
                    break;

                case "set":
                    await HandleSetAsync(root).ConfigureAwait(false);
                    break;

                case "setCurve":
                    await HandleCurveAsync(root, rpm: false).ConfigureAwait(false);
                    break;

                case "setRpmCurve":
                    await HandleCurveAsync(root, rpm: true).ConfigureAwait(false);
                    break;

                case "reconnect":
                    _runtime.RequestReconnect();
                    Send(Reply(new { @event = "ok", cmd = "reconnect" }));
                    break;

                case "scanBle":
                    await HandleScanBleAsync().ConfigureAwait(false);
                    break;

                case "listPorts":
                    Send(JsonSerializer.Serialize(new
                    {
                        @event = "ports",
                        ports = SerialPort.GetPortNames().OrderBy(name => name, StringComparer.OrdinalIgnoreCase).ToArray(),
                    }, ReplyOptions));
                    break;

                case "listGpus":
                    var gpus = await _runtime.EnumerateGpuNamesAsync().ConfigureAwait(false);
                    Send(JsonSerializer.Serialize(new { @event = "gpus", gpus }, ReplyOptions));
                    break;

                case "setAutostart":
                    await HandleSetAutostartAsync(root).ConfigureAwait(false);
                    break;

                case "setSystem":
                    await HandleSetSystemAsync(root).ConfigureAwait(false);
                    break;

                default:
                    Send(Error(ErrorInfo.Of("ipc.unknownCommand", "未知命令：" + (command ?? "(空)"), command ?? "(空)")));
                    break;
            }
        }
    }

    private async Task HandleSetAsync(JsonElement root)
    {
        if (!root.TryGetProperty("key", out var keyElement) || keyElement.ValueKind != JsonValueKind.String)
        {
            Send(Error(ErrorInfo.Of("ipc.missingKey", "缺少 key。")));
            return;
        }

        if (!root.TryGetProperty("value", out var valueElement))
        {
            Send(Error(ErrorInfo.Of("ipc.missingValue", "缺少 value。")));
            return;
        }

        var key = keyElement.GetString() ?? string.Empty;
        if (!ConfigPatch.TryApply(_runtime.CurrentConfig, key, valueElement, out var updated, out var error))
        {
            Send(Error(error));
            return;
        }

        await ApplyAsync(updated).ConfigureAwait(false);
    }

    private async Task HandleCurveAsync(JsonElement root, bool rpm)
    {
        if (!root.TryGetProperty("points", out var pointsElement) || pointsElement.ValueKind != JsonValueKind.Array)
        {
            Send(Error(ErrorInfo.Of("ipc.missingPoints", "缺少 points 数组。")));
            return;
        }

        var points = new List<(double X, double Y)>();
        foreach (var point in pointsElement.EnumerateArray())
        {
            if (point.ValueKind != JsonValueKind.Array || point.GetArrayLength() < 2)
            {
                Send(Error(ErrorInfo.Of("ipc.badPoint", "曲线点必须是 [x, y] 数组。")));
                return;
            }

            points.Add((point[0].GetDouble(), point[1].GetDouble()));
        }

        var applied = rpm
            ? ConfigPatch.TrySetRpmCurve(_runtime.CurrentConfig, points, out var updated, out var error)
            : ConfigPatch.TrySetCurve(_runtime.CurrentConfig, points, out updated, out error);
        if (!applied)
        {
            Send(Error(error));
            return;
        }

        await ApplyAsync(updated).ConfigureAwait(false);
    }

    private async Task HandleScanBleAsync()
    {
        var devices = await BleDeviceScanner.EnumerateAsync(CancellationToken.None).ConfigureAwait(false);
        Send(JsonSerializer.Serialize(new
        {
            @event = "bleDevices",
            devices = devices.Select(device => new { name = device.Name, mac = device.MacAddress }).ToArray(),
        }, ReplyOptions));
    }

    private async Task HandleSetAutostartAsync(JsonElement root)
    {
        if (!root.TryGetProperty("value", out var value) || value.ValueKind is not (JsonValueKind.True or JsonValueKind.False))
        {
            Send(Error(ErrorInfo.Of("ipc.missingValue", "缺少 value。")));
            return;
        }

        var enabled = value.GetBoolean();
        try
        {
            if (enabled)
            {
                await AutostartManager.EnableAsync().ConfigureAwait(false);
            }
            else
            {
                await AutostartManager.DisableAsync().ConfigureAwait(false);
            }

            _autostart = await AutostartManager.IsEnabledAsync().ConfigureAwait(false);
        }
        catch (FanControlException ex)
        {
            Send(Error(ex.Error));
            return;
        }

        Send(StateJson.BuildState(_runtime, _autostart, _system.Current));
        Send(Reply(new { @event = "ok", cmd = "setAutostart", autostart = _autostart }));
    }

    private async Task HandleSetSystemAsync(JsonElement root)
    {
        if (!root.TryGetProperty("key", out var keyElement) || keyElement.ValueKind != JsonValueKind.String)
        {
            Send(Error(ErrorInfo.Of("ipc.missingKey", "缺少 key。")));
            return;
        }

        if (!root.TryGetProperty("value", out var value))
        {
            Send(Error(ErrorInfo.Of("ipc.missingValue", "缺少 value。")));
            return;
        }

        var key = keyElement.GetString() ?? string.Empty;
        var error = await _system.ApplyAsync(key, value).ConfigureAwait(false);
        if (error is not null)
        {
            Send(Error(error));
            return;
        }

        Send(StateJson.BuildState(_runtime, _autostart, _system.Current));
        Send(Reply(new { @event = "ok", cmd = "setSystem" }));
    }

    private async Task RefreshAutostartAsync()
    {
        try
        {
            _autostart = await AutostartManager.IsEnabledAsync().ConfigureAwait(false);
        }
        catch (Exception ex)
        {
            _logger.LogDebug(ex, "查询开机自启状态失败。");
        }
    }

    private async Task ApplyAsync(AppConfig updated)
    {
        try
        {
            await _runtime.ApplyConfigAsync(updated).ConfigureAwait(false);
        }
        catch (Exception ex)
        {
            _logger.LogError(ex, "应用配置失败。");
            Send(Error(ErrorInfo.Of("config.applyFailed", "应用配置失败：" + ex.Message, ex.Message)));
            return;
        }

        Send(StateJson.BuildConfig(_runtime, _autostart, _system.Current));
        Send(Reply(new { @event = "ok", cmd = "set" }));
    }

    private static void Send(string json) => UiBridge.Push(json);

    private static string Reply(object payload) => JsonSerializer.Serialize(payload, ReplyOptions);

    private static string Error(string message) => Error(ErrorInfo.Of("error.generic", message));

    /// <summary>带错误码的错误帧：code/arg1/arg2 供界面翻译，message 是中文兜底。</summary>
    private static string Error(ErrorInfo? info) => Reply(new
    {
        @event = "error",
        code = info?.Code ?? "error.generic",
        arg1 = info?.Arg1,
        arg2 = info?.Arg2,
        message = info?.Message ?? string.Empty,
    });

    public async ValueTask DisposeAsync()
    {
        _cts.Cancel();
        if (_pushLoop is not null)
        {
            try
            {
                await _pushLoop.WaitAsync(TimeSpan.FromSeconds(2)).ConfigureAwait(false);
            }
            catch (Exception)
            {
                // 关闭阶段忽略
            }
        }

        _cts.Dispose();
    }
}
