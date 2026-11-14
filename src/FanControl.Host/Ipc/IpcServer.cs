using System.Collections.Concurrent;
using System.IO.Pipes;
using System.IO.Ports;
using System.Text.Encodings.Web;
using System.Text.Json;
using FanControl.Service.Communication;
using FanControl.Service.Host;
using FanControl.Service.Tray;
using FanControl.Shared.Contracts;
using Microsoft.Extensions.Logging;

namespace FanControl.Host.Ipc;

/// <summary>
/// 命名管道服务端：一帧一个 JSON（4 字节小端长度前缀 + UTF-8 载荷，见 PipeFraming）。
/// 界面连上来后先收到一次 state，此后每 400 ms 收到一次 state 推送；
/// 配置变更时额外收到 config 事件。
/// </summary>
internal sealed class IpcServer : IAsyncDisposable
{
    public const string PipeName = "FanControl.Host.v1";
    private const int ProtocolVersion = 1;
    private const int PushIntervalMilliseconds = 400;

    private static readonly JsonSerializerOptions ReplyOptions = new() { Encoder = JavaScriptEncoder.UnsafeRelaxedJsonEscaping };

    private readonly FanControlRuntime _runtime;
    private readonly SystemSettings _system;
    private readonly Action _showWindow;
    private readonly ILogger _logger;
    private readonly CancellationTokenSource _cts = new();
    private readonly ConcurrentDictionary<string, ClientSession> _clients = new();

    private Task? _acceptLoop;
    private Task? _pushLoop;
    private volatile bool _autostart;

    public IpcServer(FanControlRuntime runtime, SystemSettings system, Action showWindow, ILogger logger)
    {
        _runtime = runtime;
        _system = system;
        _showWindow = showWindow;
        _logger = logger;
    }

    public void Start()
    {
        _ = Task.Run(RefreshAutostartAsync);
        _acceptLoop = Task.Run(() => AcceptLoopAsync(_cts.Token));
        _pushLoop = Task.Run(() => PushLoopAsync(_cts.Token));
        _logger.LogInformation("命名管道服务已启动：{Pipe}", PipeName);
    }

    private async Task AcceptLoopAsync(CancellationToken cancellationToken)
    {
        while (!cancellationToken.IsCancellationRequested)
        {
            NamedPipeServerStream pipe;
            try
            {
                pipe = new NamedPipeServerStream(
                    PipeName,
                    PipeDirection.InOut,
                    NamedPipeServerStream.MaxAllowedServerInstances,
                    PipeTransmissionMode.Byte,
                    PipeOptions.Asynchronous);
            }
            catch (Exception ex)
            {
                _logger.LogError(ex, "创建管道实例失败。");
                await Task.Delay(500, CancellationToken.None).ConfigureAwait(false);
                continue;
            }

            try
            {
                await pipe.WaitForConnectionAsync(cancellationToken).ConfigureAwait(false);
            }
            catch (OperationCanceledException)
            {
                await pipe.DisposeAsync().ConfigureAwait(false);
                break;
            }
            catch (Exception ex)
            {
                _logger.LogDebug(ex, "等待界面连接失败。");
                await pipe.DisposeAsync().ConfigureAwait(false);
                await Task.Delay(200, CancellationToken.None).ConfigureAwait(false);
                continue;
            }

            var session = new ClientSession(Guid.NewGuid().ToString("N"), pipe);
            _clients[session.Id] = session;
            _logger.LogInformation("界面已连接（{Count} 个会话）。", _clients.Count);
            _ = Task.Run(() => SessionLoopAsync(session, cancellationToken), CancellationToken.None);
        }
    }

    private async Task SessionLoopAsync(ClientSession session, CancellationToken cancellationToken)
    {
        try
        {
            // 连上就先推一份完整快照，界面不必额外请求；开机自启状态每次连接时重新查询，
            // 这样从托盘菜单改过的开关也能同步到界面。
            await RefreshAutostartAsync().ConfigureAwait(false);
            await session.SendAsync(StateJson.BuildState(_runtime, _autostart, _system.Current), cancellationToken).ConfigureAwait(false);

            while (!cancellationToken.IsCancellationRequested)
            {
                var frame = await PipeFraming.ReadFrameAsync(session.Stream, cancellationToken).ConfigureAwait(false);
                if (frame is null)
                {
                    break;
                }

                if (frame.Length == 0)
                {
                    continue;
                }

                await HandleAsync(session, frame, cancellationToken).ConfigureAwait(false);
            }
        }
        catch (OperationCanceledException)
        {
        }
        catch (Exception ex)
        {
            _logger.LogDebug(ex, "界面会话异常结束。");
        }
        finally
        {
            _clients.TryRemove(session.Id, out _);
            await session.DisposeAsync().ConfigureAwait(false);
            _logger.LogInformation("界面已断开（剩余 {Count} 个会话）。", _clients.Count);
        }
    }

    private async Task HandleAsync(ClientSession session, string json, CancellationToken cancellationToken)
    {
        JsonDocument document;
        try
        {
            document = JsonDocument.Parse(json);
        }
        catch (JsonException)
        {
            await session.SendAsync(Error(ErrorInfo.Of("ipc.badJson", "请求不是合法 JSON。")), cancellationToken).ConfigureAwait(false);
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
                    await session.SendAsync(Reply(new
                    {
                        @event = "hello",
                        protocol = ProtocolVersion,
                        version = typeof(IpcServer).Assembly.GetName().Version?.ToString(3) ?? "0.0.0",
                    }), cancellationToken).ConfigureAwait(false);
                    break;

                case "getState":
                    await session.SendAsync(StateJson.BuildState(_runtime, _autostart, _system.Current), cancellationToken).ConfigureAwait(false);
                    break;

                case "set":
                    await HandleSetAsync(session, root, cancellationToken).ConfigureAwait(false);
                    break;

                case "setCurve":
                    await HandleCurveAsync(session, root, cancellationToken, rpm: false).ConfigureAwait(false);
                    break;

                case "setRpmCurve":
                    await HandleCurveAsync(session, root, cancellationToken, rpm: true).ConfigureAwait(false);
                    break;

                case "reconnect":
                    _runtime.RequestReconnect();
                    await session.SendAsync(Reply(new { @event = "ok", cmd = "reconnect" }), cancellationToken).ConfigureAwait(false);
                    break;

                case "scanBle":
                    await HandleScanBleAsync(session, cancellationToken).ConfigureAwait(false);
                    break;

                case "listPorts":
                    await session.SendAsync(JsonSerializer.Serialize(new
                    {
                        @event = "ports",
                        ports = SerialPort.GetPortNames().OrderBy(name => name, StringComparer.OrdinalIgnoreCase).ToArray(),
                    }, ReplyOptions), cancellationToken).ConfigureAwait(false);
                    break;

                case "listGpus":
                    var gpus = await _runtime.EnumerateGpuNamesAsync().ConfigureAwait(false);
                    await session.SendAsync(JsonSerializer.Serialize(new { @event = "gpus", gpus }, ReplyOptions), cancellationToken).ConfigureAwait(false);
                    break;

                case "setAutostart":
                    await HandleSetAutostartAsync(session, root, cancellationToken).ConfigureAwait(false);
                    break;

                case "setSystem":
                    await HandleSetSystemAsync(session, root, cancellationToken).ConfigureAwait(false);
                    break;

                case "showWindow":
                    _showWindow();
                    break;

                default:
                    await session.SendAsync(Error(ErrorInfo.Of("ipc.unknownCommand", "未知命令：" + (command ?? "(空)"), command ?? "(空)")), cancellationToken).ConfigureAwait(false);
                    break;
            }
        }
    }

    private async Task HandleSetAsync(ClientSession session, JsonElement root, CancellationToken cancellationToken)
    {
        if (!root.TryGetProperty("key", out var keyElement) || keyElement.ValueKind != JsonValueKind.String)
        {
            await session.SendAsync(Error(ErrorInfo.Of("ipc.missingKey", "缺少 key。")), cancellationToken).ConfigureAwait(false);
            return;
        }

        if (!root.TryGetProperty("value", out var valueElement))
        {
            await session.SendAsync(Error(ErrorInfo.Of("ipc.missingValue", "缺少 value。")), cancellationToken).ConfigureAwait(false);
            return;
        }

        var key = keyElement.GetString() ?? string.Empty;
        if (!ConfigPatch.TryApply(_runtime.CurrentConfig, key, valueElement, out var updated, out var error))
        {
            await session.SendAsync(Error(error), cancellationToken).ConfigureAwait(false);
            return;
        }

        await ApplyAsync(session, updated, cancellationToken).ConfigureAwait(false);
    }

    private async Task HandleCurveAsync(ClientSession session, JsonElement root, CancellationToken cancellationToken, bool rpm)
    {
        if (!root.TryGetProperty("points", out var pointsElement) || pointsElement.ValueKind != JsonValueKind.Array)
        {
            await session.SendAsync(Error(ErrorInfo.Of("ipc.missingPoints", "缺少 points 数组。")), cancellationToken).ConfigureAwait(false);
            return;
        }

        var points = new List<(double X, double Y)>();
        foreach (var point in pointsElement.EnumerateArray())
        {
            if (point.ValueKind != JsonValueKind.Array || point.GetArrayLength() < 2)
            {
                await session.SendAsync(Error(ErrorInfo.Of("ipc.badPoint", "曲线点必须是 [x, y] 数组。")), cancellationToken).ConfigureAwait(false);
                return;
            }

            points.Add((point[0].GetDouble(), point[1].GetDouble()));
        }

        var applied = rpm
            ? ConfigPatch.TrySetRpmCurve(_runtime.CurrentConfig, points, out var updated, out var error)
            : ConfigPatch.TrySetCurve(_runtime.CurrentConfig, points, out updated, out error);
        if (!applied)
        {
            await session.SendAsync(Error(error), cancellationToken).ConfigureAwait(false);
            return;
        }

        if (rpm)
        {
            ConfigPatch.TrySetRpmCurve(_runtime.CurrentConfig, points, out updated, out _);
        }

        await ApplyAsync(session, updated, cancellationToken).ConfigureAwait(false);
    }

    private async Task HandleScanBleAsync(ClientSession session, CancellationToken cancellationToken)
    {
        // 扫描要读 SetupAPI + BTHPORT 注册表，放在线程池里跑，别堵住管道读循环
        var devices = await BleDeviceScanner.EnumerateAsync(cancellationToken).ConfigureAwait(false);
        await session.SendAsync(JsonSerializer.Serialize(new
        {
            @event = "bleDevices",
            devices = devices.Select(device => new { name = device.Name, mac = device.MacAddress }).ToArray(),
        }, ReplyOptions), cancellationToken).ConfigureAwait(false);
    }

    private async Task HandleSetAutostartAsync(ClientSession session, JsonElement root, CancellationToken cancellationToken)
    {
        if (!root.TryGetProperty("value", out var value) || value.ValueKind is not (JsonValueKind.True or JsonValueKind.False))
        {
            await session.SendAsync(Error(ErrorInfo.Of("ipc.missingValue", "缺少 value。")), cancellationToken).ConfigureAwait(false);
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
            await session.SendAsync(Error(ex.Error), cancellationToken).ConfigureAwait(false);
            return;
        }

        await BroadcastAsync(StateJson.BuildState(_runtime, _autostart, _system.Current), cancellationToken).ConfigureAwait(false);
        await session.SendAsync(Reply(new { @event = "ok", cmd = "setAutostart", autostart = _autostart }), cancellationToken).ConfigureAwait(false);
    }

    private async Task HandleSetSystemAsync(ClientSession session, JsonElement root, CancellationToken cancellationToken)
    {
        if (!root.TryGetProperty("key", out var keyElement) || keyElement.ValueKind != JsonValueKind.String)
        {
            await session.SendAsync(Error(ErrorInfo.Of("ipc.missingKey", "缺少 key。")), cancellationToken).ConfigureAwait(false);
            return;
        }

        if (!root.TryGetProperty("value", out var value))
        {
            await session.SendAsync(Error(ErrorInfo.Of("ipc.missingValue", "缺少 value。")), cancellationToken).ConfigureAwait(false);
            return;
        }

        var key = keyElement.GetString() ?? string.Empty;
        var error = await _system.ApplyAsync(key, value).ConfigureAwait(false);
        if (error is not null)
        {
            await session.SendAsync(Error(error), cancellationToken).ConfigureAwait(false);
            return;
        }

        await BroadcastAsync(StateJson.BuildState(_runtime, _autostart, _system.Current), cancellationToken).ConfigureAwait(false);
        await session.SendAsync(Reply(new { @event = "ok", cmd = "setSystem" }), cancellationToken).ConfigureAwait(false);
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

    private async Task ApplyAsync(ClientSession session, FanControl.Shared.Models.AppConfig updated, CancellationToken cancellationToken)
    {
        try
        {
            await _runtime.ApplyConfigAsync(updated).ConfigureAwait(false);
        }
        catch (Exception ex)
        {
            _logger.LogError(ex, "应用配置失败。");
            await session.SendAsync(Error(ErrorInfo.Of("config.applyFailed", "应用配置失败：" + ex.Message, ex.Message)), cancellationToken).ConfigureAwait(false);
            return;
        }

        await BroadcastAsync(StateJson.BuildConfig(_runtime, _autostart, _system.Current), cancellationToken).ConfigureAwait(false);
        await session.SendAsync(Reply(new { @event = "ok", cmd = "set" }), cancellationToken).ConfigureAwait(false);
    }

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

            if (_clients.IsEmpty)
            {
                continue;
            }

            await BroadcastAsync(StateJson.BuildState(_runtime, _autostart, _system.Current), cancellationToken).ConfigureAwait(false);
        }
    }

    private async Task BroadcastAsync(string json, CancellationToken cancellationToken)
    {
        foreach (var session in _clients.Values)
        {
            try
            {
                await session.SendAsync(json, cancellationToken).ConfigureAwait(false);
            }
            catch (Exception)
            {
                _clients.TryRemove(session.Id, out _);
            }
        }
    }

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
        foreach (var session in _clients.Values)
        {
            await session.DisposeAsync().ConfigureAwait(false);
        }

        _clients.Clear();

        foreach (var task in new[] { _acceptLoop, _pushLoop })
        {
            if (task is null)
            {
                continue;
            }

            try
            {
                await task.WaitAsync(TimeSpan.FromSeconds(2)).ConfigureAwait(false);
            }
            catch (Exception)
            {
                // 关闭阶段忽略
            }
        }

        _cts.Dispose();
    }

    private sealed class ClientSession : IAsyncDisposable
    {
        private readonly SemaphoreSlim _writeLock = new(1, 1);

        public ClientSession(string id, NamedPipeServerStream stream)
        {
            Id = id;
            Stream = stream;
        }

        public string Id { get; }

        public NamedPipeServerStream Stream { get; }

        public async Task SendAsync(string json, CancellationToken cancellationToken)
        {
            await _writeLock.WaitAsync(cancellationToken).ConfigureAwait(false);
            try
            {
                await PipeFraming.WriteFrameAsync(Stream, json, cancellationToken).ConfigureAwait(false);
            }
            finally
            {
                _writeLock.Release();
            }
        }

        public ValueTask DisposeAsync()
        {
            _writeLock.Dispose();
            return Stream.DisposeAsync();
        }
    }
}
