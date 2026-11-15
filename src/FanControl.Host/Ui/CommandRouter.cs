using System.Diagnostics;
using System.IO.Ports;
using System.Net;
using System.Net.Http;
using System.Text.Encodings.Web;
using System.Text.Json;
using FanControl.Service.Communication;
using FanControl.Service.Host;
using FanControl.Service.Logging;
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
    // 值没变的帧没必要重发，但也得留个心跳，免得界面在长时间静止后拿到过期数据
    private const int HeartbeatMilliseconds = 1000;
    private const string ReleasesApi = "https://api.github.com/repos/anlerway/Bluetooth-FanControl/releases/latest";

    private static readonly JsonSerializerOptions ReplyOptions = new() { Encoder = JavaScriptEncoder.UnsafeRelaxedJsonEscaping };

    private readonly FanControlRuntime _runtime;
    private readonly SystemSettings _system;
    private readonly ILogger _logger;
    // 界面「运行日志」页：内存缓冲 + 当前日志目录（目录跟着数据位置走，用 _system.LogDirectory 现取）
    private readonly LogFeed _feed;
    private readonly CancellationTokenSource _cts = new();

    // 界面一条动作可能拆成几条命令（例如选蓝牙设备 = set mac + set name），
    // 它们各自读 CurrentConfig 再写盘，并发执行时后落盘的那条会把前一条的改动覆盖掉。
    // 命令本身都是毫秒级，串行化比给每条命令补版本号简单得多。
    private readonly SemaphoreSlim _commandLock = new(1, 1);

    // 复用同一个 HttpClient：每次 new 会各带一套连接池与定时器，是长期进程的隐形泄漏
    private static readonly HttpClient Http = CreateHttpClient();

    // 采样栈每秒只造几十 KB 垃圾，Gen0 预算却要攒到 15 MB 才回收一次——长期的托盘进程没必要
    // 把这段垃圾一直压在自己身上，攒够 60 秒就主动收一次代 0（后台、不阻塞，代价几毫秒）。
    private const int IdleCollectSeconds = 60;

    private Task? _pushLoop;
    private volatile bool _autostart;
    private string? _lastSignature;
    private long _lastPushTicks;
    private long _lastCollectTicks = Environment.TickCount64;
    // 第一次运行还没写 system.json：界面弹出欢迎向导，选完语言与数据目录后置 false
    private bool _firstRun;

    public CommandRouter(FanControlRuntime runtime, SystemSettings system, ILogger logger, LogFeed feed, bool firstRun)
    {
        _runtime = runtime;
        _system = system;
        _logger = logger;
        _feed = feed;
        _firstRun = firstRun;
    }

    public void Start()
    {
        // 托盘菜单也能切换自启：订阅广播，让设置页的开关立刻跟上
        AutostartManager.StateChanged += OnAutostartStateChanged;
        _ = Task.Run(RefreshAutostartAsync);
        // 先把 token 取出来再进 lambda：CTS 若在任务被调度前就释放，闭包里取 Token 会抛
        var token = _cts.Token;
        _pushLoop = Task.Run(() => PushLoopAsync(token));
    }

    private void OnAutostartStateChanged(bool enabled)
    {
        if (_autostart == enabled)
        {
            return;
        }

        _autostart = enabled;
        PushSnapshot();
    }

    /// <summary>界面刚打开时先给一帧完整快照。</summary>
    public void PushSnapshot() => Send(StateJson.BuildState(_runtime, _autostart, _system.Current, _firstRun));

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

            if (Environment.TickCount64 - _lastCollectTicks >= IdleCollectSeconds * 1000L)
            {
                _lastCollectTicks = Environment.TickCount64;
                GC.Collect(0, GCCollectionMode.Forced, blocking: false, compacting: false);
            }

            // 界面没开就不生成 JSON：一次性进程里唯一消费者就是那个窗口。
            if (!UiBridge.IsRunning)
            {
                continue;
            }

            var json = StateJson.BuildState(_runtime, _autostart, _system.Current, _firstRun);
            var signature = Signature(json);
            var now = Environment.TickCount64;
            if (signature == _lastSignature && now - _lastPushTicks < HeartbeatMilliseconds)
            {
                continue;
            }

            _lastSignature = signature;
            _lastPushTicks = now;
            Send(json);
        }
    }

    /// <summary>去掉每次都变的 timestamp，其余字段一致就算同一帧。</summary>
    private static string Signature(string json)
    {
        var index = json.LastIndexOf(",\"timestamp\":", StringComparison.Ordinal);
        return index > 0 ? json[..index] : json;
    }

    /// <summary>串行化入口：命令按到达顺序一条条落地，避免并发写盘互相覆盖。</summary>
    public async Task HandleAsync(string json)
    {
        await _commandLock.WaitAsync().ConfigureAwait(false);
        try
        {
            await HandleCoreAsync(json).ConfigureAwait(false);
        }
        finally
        {
            _commandLock.Release();
        }
    }

    private async Task HandleCoreAsync(string json)
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
                    Send(StateJson.BuildState(_runtime, _autostart, _system.Current, _firstRun));
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

                case "setDataLocation":
                    await HandleSetDataLocationAsync(root).ConfigureAwait(false);
                    break;

                case "completeSetup":
                    await HandleCompleteSetupAsync(root).ConfigureAwait(false);
                    break;

                case "checkUpdate":
                    // 网络请求不能占住界面线程的回调（回调本身已经在线程池上，这里再放走一层）
                    _ = CheckUpdateAsync();
                    break;

                case "getLogs":
                    SendLogs(root);
                    break;

                case "clearLogs":
                    // 只清内存缓冲：磁盘上的日志文件留给用户自己看
                    _feed.Clear();
                    Send(Reply(new { @event = "ok", cmd = "clearLogs" }));
                    break;

                case "openLogs":
                    OpenLogDirectory();
                    break;

                case "uiClosed":
                    // 界面窗口关了，进程还要住托盘：等界面线程把 D2D 资源放完再还工作集
                    _ = Task.Run(async () =>
                    {
                        await Task.Delay(600).ConfigureAwait(false);
                        // 先把托管垃圾还掉再 trim：顺序反了的话，回收出来的空闲页又会被算进工作集
                        GC.Collect(GC.MaxGeneration, GCCollectionMode.Forced, blocking: true, compacting: true);
                        MemoryTrim.Run();
                    });
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
            LogRejection(error);
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
            LogRejection(error);
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

        Send(StateJson.BuildState(_runtime, _autostart, _system.Current, _firstRun));
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
            LogRejection(error);
            Send(Error(error));
            return;
        }

        Send(StateJson.BuildState(_runtime, _autostart, _system.Current, _firstRun));
        Send(Reply(new { @event = "ok", cmd = "setSystem" }));
    }

    /// <summary>设置页「数据目录」：location 是 AppData / ExeDirectory / Custom，Custom 必须带绝对路径。</summary>
    private async Task HandleSetDataLocationAsync(JsonElement root)
    {
        var location = root.TryGetProperty("location", out var locationElement) && locationElement.ValueKind == JsonValueKind.String
            ? locationElement.GetString()
            : null;
        var path = root.TryGetProperty("path", out var pathElement) && pathElement.ValueKind == JsonValueKind.String
            ? pathElement.GetString()
            : null;

        if (string.IsNullOrWhiteSpace(location))
        {
            Send(Error(ErrorInfo.Of("ipc.missingValue", "缺少 location。")));
            return;
        }

        var error = await _system.SetDataLocationAsync(location, path).ConfigureAwait(false);
        if (error is not null)
        {
            LogRejection(error);
            Send(Error(error));
            return;
        }

        Send(StateJson.BuildState(_runtime, _autostart, _system.Current, _firstRun));
        Send(Reply(new
        {
            @event = "ok",
            cmd = "setDataLocation",
            state = _system.Current.UserDataLocation.ToString(),
            path = _system.Current.UserDataPath ?? string.Empty,
        }));
    }

    /// <summary>首次运行向导：一次把语言与数据目录定下来，然后关掉向导。</summary>
    private async Task HandleCompleteSetupAsync(JsonElement root)
    {
        var language = root.TryGetProperty("language", out var languageElement) && languageElement.ValueKind == JsonValueKind.String
            ? languageElement.GetString()
            : null;
        var location = root.TryGetProperty("location", out var locationElement) && locationElement.ValueKind == JsonValueKind.String
            ? locationElement.GetString()
            : null;
        var path = root.TryGetProperty("path", out var pathElement) && pathElement.ValueKind == JsonValueKind.String
            ? pathElement.GetString()
            : null;

        // 先落标记：SetDataLocationAsync 中途会推一次状态，若那时 firstRun 还是 true，界面会把向导又弹一遍
        var wasFirstRun = _firstRun;
        _firstRun = false;

        if (!string.IsNullOrWhiteSpace(location))
        {
            var error = await _system.SetDataLocationAsync(location, path).ConfigureAwait(false);
            if (error is not null)
            {
                _firstRun = wasFirstRun;
                LogRejection(error);
                Send(Error(error));
                return;
            }
        }

        if (!string.IsNullOrWhiteSpace(language))
        {
            // 语言落在 appconfig.json 里（数据目录刚换过去，这里写的就是新位置）
            await ApplyAsync(_runtime.CurrentConfig with { Language = language }).ConfigureAwait(false);
        }

        _firstRun = false;
        Send(StateJson.BuildState(_runtime, _autostart, _system.Current, _firstRun));
        Send(Reply(new { @event = "ok", cmd = "completeSetup" }));
    }

    private static HttpClient CreateHttpClient()
    {
        var client = new HttpClient { Timeout = TimeSpan.FromSeconds(10) };
        client.DefaultRequestHeaders.UserAgent.ParseAdd("FanControl");
        client.DefaultRequestHeaders.Accept.ParseAdd("application/vnd.github+json");
        return client;
    }

    /// <summary>查 GitHub 最新 release：结果以 update 事件回给关于页。</summary>
    private async Task CheckUpdateAsync()
    {
        Send(Reply(new { @event = "update", state = "checking" }));
        try
        {
            using var response = await Http.GetAsync(ReleasesApi).ConfigureAwait(false);
            if (response.StatusCode == HttpStatusCode.NotFound)
            {
                Send(Reply(new { @event = "update", state = "none" }));
                return;
            }

            response.EnsureSuccessStatusCode();
            var body = await response.Content.ReadAsStringAsync().ConfigureAwait(false);
            using var document = JsonDocument.Parse(body);
            var root = document.RootElement;
            var tag = root.TryGetProperty("tag_name", out var tagElement) ? tagElement.GetString() ?? string.Empty : string.Empty;
            var url = root.TryGetProperty("html_url", out var urlElement) ? urlElement.GetString() ?? string.Empty : string.Empty;

            // 程序集版本是 1.4.0.0、tag 是 1.4.0：直接比 Version 会把同一个版本判成更新
            var assembly = typeof(CommandRouter).Assembly.GetName().Version ?? new Version(1, 4, 0);
            var current = new Version(assembly.Major, assembly.Minor, Math.Max(0, assembly.Build));
            var latest = Version.TryParse(tag.TrimStart('v', 'V'), out var parsed) ? parsed : null;
            var newer = latest is not null && latest > current;
            Send(Reply(new { @event = "update", state = newer ? "newer" : "current", tag, url }));
        }
        catch (Exception ex)
        {
            _logger.LogDebug(ex, "检查更新失败。");
            Send(Reply(new { @event = "update", state = "failed", detail = ex.Message }));
        }
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
            // 落盘失败（目录被挪走/删掉、磁盘满、被杀软占用）只进日志：这类消息是 .NET 的原始异常文本，
            // 弹给用户也只是「Could not find a part of the path ...」，既看不懂也没法处理。
            // 界面这边把配置退回真实值，开关不会停在没生效的状态上。
            _logger.LogWarning(ex, "应用配置失败，已退回当前生效的配置。");
            Send(StateJson.BuildConfig(_runtime, _autostart, _system.Current, _firstRun));
            return;
        }

        Send(StateJson.BuildConfig(_runtime, _autostart, _system.Current, _firstRun));
        Send(Reply(new { @event = "ok", cmd = "set" }));
    }

    /// <summary>界面「运行日志」页：按 since 增量返回，lastSeq 是下次拉取的起点。</summary>
    private void SendLogs(JsonElement root)
    {
        long since = 0;
        if (root.TryGetProperty("since", out var sinceElement) && sinceElement.ValueKind == JsonValueKind.Number)
        {
            sinceElement.TryGetInt64(out since);
        }

        var entries = _feed.Since(since, 400);
        Send(JsonSerializer.Serialize(new
        {
            @event = "logs",
            entries = entries.Select(entry => new
            {
                seq = entry.Seq,
                time = entry.Time,
                level = entry.Level,
                tag = entry.Tag,
                category = entry.Category,
                message = entry.Message,
            }).ToArray(),
            lastSeq = _feed.LastSeq,
        }, ReplyOptions));
    }

    /// <summary>在资源管理器里打开真正的日志目录（数据目录可能是安装目录，不能写死 %APPDATA%）。</summary>
    private void OpenLogDirectory()
    {
        var logDirectory = _system.LogDirectory;
        try
        {
            Directory.CreateDirectory(logDirectory);
            Process.Start(new ProcessStartInfo("explorer.exe", $"\"{logDirectory}\"") { UseShellExecute = true });
            Send(Reply(new { @event = "ok", cmd = "openLogs" }));
        }
        catch (Exception ex)
        {
            var info = ErrorInfo.Of("system.openLogsFailed", "打开日志目录失败：" + ex.Message, ex.Message)
                .WithSeverity(ErrorSeverity.Error);
            ErrorLog.Write(_logger, info, ex);
            Send(Error(info));
        }
    }

    /// <summary>拒绝一条命令时记日志：验证错误写 [验证错误]，其余按错误码级别写。</summary>
    private void LogRejection(ErrorInfo? info)
    {
        if (info is not null)
        {
            ErrorLog.Write(_logger, info);
        }
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
        // 0 提示 / 1 异常 / 2 错误 / 3 验证错误，界面据此上色并加前缀
        severity = (int)(info?.Severity ?? ErrorSeverity.Error),
        message = info?.Message ?? string.Empty,
    });

    public async ValueTask DisposeAsync()
    {
        // 退出路径上的异常一律吞掉：这里抛出去会被 Main 的 catch 当成「启动失败」弹框
        try
        {
            await DisposeCoreAsync().ConfigureAwait(false);
        }
        catch (Exception)
        {
        }
    }

    private async ValueTask DisposeCoreAsync()
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
        _commandLock.Dispose();
    }
}
