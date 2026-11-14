using FanControl.Service.Communication;
using FanControl.Service.Config;
using FanControl.Service.Fan;
using FanControl.Service.Hardware;
using FanControl.Service.Logging;
using FanControl.Shared.Contracts;
using FanControl.Shared.Enums;
using FanControl.Shared.Models;
using System.Diagnostics;
using System.Management;
using Microsoft.Extensions.Logging;

namespace FanControl.Service.Host;

/// <summary>
/// 单进程运行时：初始化配置，运行两条独立轮询循环：
/// 1) 采样循环（0.1-5 秒）：读温度 → 计算目标 PWM（含平滑）→ 串口直接发送 → 发布数据包；
/// 2) 通信循环（仅蓝牙）：互斥型轮询 —— 有连接按 1-30 秒发送 PWM，
///    无连接按 3-300 秒尝试重连（可关闭自动重连）。
/// </summary>
public sealed class FanControlRuntime : IAsyncDisposable
{
    private readonly AppState _appState;
    private readonly IConfigManager _configManager;
    private readonly TemperatureProviderFactory _providerFactory;
    private readonly FanSpeedProviderFactory _fanSpeedProviderFactory;
    private readonly CommunicationChannelFactory _channelFactory;
    private readonly FanController _fanController;
    private readonly FileLoggerProvider _fileLoggerProvider;
    private readonly ILogger<FanControlRuntime> _logger;
    private readonly CancellationTokenSource _cts = new();
    private int _disposed;

    private ITemperatureProvider? _provider;
    private ITemperatureProvider? _gpuProvider;
    private IFanSpeedProvider? _fanSpeedProvider;
    private ICommunicationChannel? _comChannel;
    private ICommunicationChannel? _bleChannel;
    private Task? _samplingTask;
    private Task? _communicationTask;
    private bool _autoFallbackDone;
    private int _lastLoggedInterval = -1;

    // 时间基准：平滑按"记录一段时间内的目标值"计算，因此用单调时钟累计秒数，
    // 不受系统时间调整影响；采样循环与通信循环同进程，仅在采样循环内访问。
    private readonly Stopwatch _clock = Stopwatch.StartNew();
    private readonly PwmSmoother _smoother = new();
    private volatile float _latestSmoothedPwm = float.NaN;
    private volatile float _latestCpuCelsius = float.NaN;
    private volatile float _latestGpuCelsius = float.NaN;
    private string _commSignature = string.Empty;
    private volatile bool _commConfigDirty;
    private volatile bool _forceReconnect;
    private FanControlMode? _lastMode;
    private TemperatureSource? _lastSource;
    private bool _bleWasConnected;
    // 本次断链是否已经通知过：一次断线只弹一条气泡，重连成功后再复位
    private bool _bleOutageNotified;
    // 这条连接是否真的成功写出过数据：没写出过就断的算「没连上」，不弹断连通知
    private bool _bleLinkVerified;
    // 上次断连通知的时间戳：设备反复连上又掉线时，一分钟内不重复弹
    private long _lastBleNotifyTicks;

    public FanControlRuntime(
        AppState appState,
        IConfigManager configManager,
        TemperatureProviderFactory providerFactory,
        FanSpeedProviderFactory fanSpeedProviderFactory,
        CommunicationChannelFactory channelFactory,
        FanController fanController,
        FileLoggerProvider fileLoggerProvider,
        ILogger<FanControlRuntime> logger)
    {
        _appState = appState;
        _configManager = configManager;
        _providerFactory = providerFactory;
        _fanSpeedProviderFactory = fanSpeedProviderFactory;
        _channelFactory = channelFactory;
        _fanController = fanController;
        _fileLoggerProvider = fileLoggerProvider;
        _logger = logger;
    }

    public AppConfig CurrentConfig => _appState.Current;

    public DataPacket? LatestPacket => _appState.LatestPacket;

    /// <summary>
    /// 枚举系统显卡名称（WMI → nvidia-smi 兜底），供设置页 GPU 选择下拉直接显示真实显卡名。
    /// </summary>
    public async Task<IReadOnlyList<string>> EnumerateGpuNamesAsync()
    {
        var names = new List<string>(EnumerateWmiVideoControllers());
        if (names.Count == 0)
        {
            names.AddRange(await EnumerateNvidiaGpuNamesAsync());
        }

        return names.Distinct().ToList();
    }

    private static IReadOnlyList<string> EnumerateWmiVideoControllers()
    {
        try
        {
            var names = new List<string>();
            using var searcher = new ManagementObjectSearcher(
                "root\\CIMV2",
                "SELECT Name FROM Win32_VideoController");
            using var collection = searcher.Get();
            foreach (ManagementBaseObject item in collection)
            {
                if (item["Name"] is string name && !string.IsNullOrWhiteSpace(name))
                {
                    names.Add(name.Trim());
                }
            }

            return names;
        }
        catch
        {
            return Array.Empty<string>();
        }
    }

    private static async Task<IReadOnlyList<string>> EnumerateNvidiaGpuNamesAsync()
    {
        try
        {
            using var process = new Process
            {
                StartInfo = new ProcessStartInfo(
                    "nvidia-smi",
                    "--query-gpu=name --format=csv,noheader,nounits")
                {
                    RedirectStandardOutput = true,
                    UseShellExecute = false,
                    CreateNoWindow = true,
                },
            };

            if (!process.Start())
            {
                return Array.Empty<string>();
            }

            var output = await process.StandardOutput.ReadToEndAsync();
            return output
                .Split('\n', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries);
        }
        catch
        {
            return Array.Empty<string>();
        }
    }

    /// <summary>最近一次采样/计算失败的原因（成功时清空）。</summary>
    public string? LastError { get; private set; }

    /// <summary>最近一次失败的错误码与参数：界面按 code 翻译，LastError 只是中文兜底。</summary>
    public ErrorInfo? LastErrorInfo { get; private set; }

    /// <summary>最近一次通信失败（连接/写入/校时）的原因，链路恢复正常时清空。</summary>
    public string? LastCommError { get; private set; }

    /// <summary>通信失败的错误码与参数：界面优先用它翻译，LastCommError 是中文兜底。</summary>
    public ErrorInfo? LastCommErrorInfo { get; private set; }

    /// <summary>新数据包到达时触发（后台线程），UI 需自行切回 UI 线程。</summary>
    public event EventHandler? DataChanged;

    /// <summary>温度数据源切换时触发（后台线程），UI 用于清空趋势曲线。</summary>
    public event EventHandler? TemperatureSourceChanged;

    /// <summary>通信链路状态（设置页显示用）。</summary>
    public CommunicationState ConnectionState { get; private set; } = CommunicationState.Waiting;

    public event EventHandler? ConnectionStateChanged;

    /// <summary>蓝牙连接状态变化时触发（true=已连接，false=断开）。</summary>
    public event EventHandler<bool>? BleConnectionChanged;

    /// <summary>
    /// 蓝牙链路不可用（运行中断开，或重连连续失败）时触发：一次断线只触发一次，
    /// 重连成功后复位。断连通知订阅它，而不是 BleConnectionChanged —— 后者在「从来没连上」时不会触发。
    /// </summary>
    public event EventHandler? BleDisconnected;

    /// <summary>温度采样开始失败（上次成功/无错误）时触发，用于异常通知。</summary>
    public event EventHandler? TemperatureErrorOccurred;

    /// <summary>配置被应用后触发（含 ATKACPI 自动回退），托盘与界面据此刷新语言/文案。</summary>
    public event EventHandler? ConfigChanged;

    public async Task StartAsync()
    {
        await _appState.InitializeAsync();
        SyncHardwareModules();
        ApplyLoggingSettings(await _configManager.LoadSystemConfigAsync());
        _logger.LogInformation(
            "监控循环启动：数据源 {Source}，通信 {Channel}，采样 {Sample}ms",
            CurrentConfig.TemperatureSource,
            CurrentConfig.CommunicationType,
            CurrentConfig.PollIntervalMilliseconds);
        // 令牌先取出来：任务真正开始跑之前 CTS 可能已经释放，lambda 里再取会抛 ObjectDisposedException
        var token = _cts.Token;
        _samplingTask = Task.Run(() => RunSamplingLoopAsync(token));
        _communicationTask = Task.Run(() => RunCommunicationLoopAsync(token));
    }

    public async Task ApplyConfigAsync(AppConfig config)
    {
        await _appState.ApplyAsync(config).ConfigureAwait(false);
        SyncHardwareModules();
        RaiseConfigChanged();
    }

    // LHM 只枚举当前配置用得到的模块：GPU 模块要加载 NVAPI/ADL（+37 MB），主板/EC 再 +8 MB。
    private void SyncHardwareModules()
    {
        var config = CurrentConfig;
        LhmComputerHost.Configure(config.TemperatureSource, config.GpuTemperatureSource, config.FanSpeedSource);
    }

    private void RaiseConfigChanged()
    {
        try
        {
            ConfigChanged?.Invoke(this, EventArgs.Empty);
        }
        catch
        {
            // 订阅方（托盘/日志）异常不影响配置应用
        }
    }

    /// <summary>读取系统配置（日志位置/开关/保留数量等）。</summary>
    public Task<SystemConfig> LoadSystemConfigAsync()
    {
        return _configManager.LoadSystemConfigAsync();
    }

    /// <summary>保存系统配置并立即应用日志设置（目录/开关/保留数量）。</summary>
    public async Task SaveSystemConfigAsync(SystemConfig system)
    {
        await _configManager.SaveSystemConfigAsync(system);
        ApplyLoggingSettings(system);
    }

    private void ApplyLoggingSettings(SystemConfig system)
    {
        _fileLoggerProvider.Configure(
            _configManager.GetLogDirectory(system),
            system.LogEnabled,
            Math.Max(1, system.MaxLogFiles));
    }

    /// <summary>手动重建连接：立即断开并重试（托盘/设置页触发，任意线程可调用）。</summary>
    public void RequestReconnect()
    {
        _forceReconnect = true;
        _commConfigDirty = true;
    }

    public async ValueTask DisposeAsync()
    {
        // 幂等：容器（provider）与宿主都可能释放同一个单例，第二次进来不能再碰已释放的 CTS
        if (Interlocked.Exchange(ref _disposed, 1) != 0)
        {
            return;
        }

        try
        {
            _cts.Cancel();
        }
        catch (ObjectDisposedException)
        {
            // 已经释放过：继续往下走，别让退出路径抛异常
        }

        foreach (var task in new[] { _samplingTask, _communicationTask })
        {
            if (task is null)
            {
                continue;
            }

            try
            {
                await task;
            }
            catch (OperationCanceledException)
            {
                // 正常停止
            }
            catch
            {
                // 停止阶段异常不影响退出
            }
        }

        _cts.Dispose();

        try
        {
            foreach (var channel in new[] { _comChannel, _bleChannel })
            {
                if (channel?.IsConnected == true)
                {
                    await channel.DisconnectAsync();
                }
            }

            if (_provider is IDisposable disposable)
            {
                disposable.Dispose();
            }

            if (_fanSpeedProvider is IDisposable disposableFan)
            {
                disposableFan.Dispose();
            }
        }
        catch
        {
            // 忽略释放异常
        }
    }

    /// <summary>采样循环：按 0.1-5 秒读取温度、计算（含平滑）并发布；串口通道在此直接发送。</summary>
    private async Task RunSamplingLoopAsync(CancellationToken cancellationToken)
    {
        while (!cancellationToken.IsCancellationRequested)
        {
            var sampleIntervalMs = Math.Clamp(_appState.Current.PollIntervalMilliseconds, 100, 5000);
            if (sampleIntervalMs != _lastLoggedInterval)
            {
                _lastLoggedInterval = sampleIntervalMs;
                _logger.LogInformation("采样间隔已更新为 {Interval}ms", sampleIntervalMs);
            }

            var stopwatch = System.Diagnostics.Stopwatch.StartNew();

            try
            {
                var config = _appState.Current;
                await EnsureCommConfigAsync(config);
                var provider = GetOrCreateProvider(config.TemperatureSource);
                var snapshot = await provider.ReadAsync(cancellationToken, config.GpuSelection);

                // ATKACPI 在华硕新机型上多数不支持温度方法：主源完全无数据时自动切到 LHM
                if (!_autoFallbackDone
                    && config.TemperatureSource == TemperatureSource.AtkAcpi
                    && snapshot.CpuTemperatureCelsius is null
                    && snapshot.GpuTemperatureCelsius is null)
                {
                    _autoFallbackDone = true;
                    _logger.LogWarning("ATKACPI 当前机型无温度数据，自动切换到 LibreHardwareMonitor。");
                    await _appState.ApplyAsync(
                        _appState.Current with
                        {
                            TemperatureSource = TemperatureSource.LibreHardwareMonitor,
                        });
                }

                // CPU 与 GPU 获取方式分开。单个源失败只影响该温度，不中断整轮采样/绘图。
                var gpuSource = config.GpuTemperatureSource ?? TemperatureSource.NvidiaSmiAdl;
                if (gpuSource == TemperatureSource.NvidiaSmiAdl)
                {
                    // 专用链路：NVIDIA nvidia-smi → AMD ADL
                    try
                    {
                        var gpu = GpuTemperatureReader.Read(config.GpuSelection);
                        snapshot = snapshot with { GpuTemperatureCelsius = gpu };
                    }
                    catch (Exception gpuEx)
                    {
                        _logger.LogWarning(gpuEx, "GPU 温度读取失败（NVIDIA-SMI / ADL），继续使用主源数据。");
                    }
                }
                else if (gpuSource != config.TemperatureSource)
                {
                    try
                    {
                        var gpuSnapshot = await GetOrCreateGpuProvider(gpuSource)
                            .ReadAsync(cancellationToken, config.GpuSelection);
                        snapshot = snapshot with { GpuTemperatureCelsius = gpuSnapshot.GpuTemperatureCelsius };
                    }
                    catch (Exception gpuEx)
                    {
                        _logger.LogWarning(gpuEx, "GPU 温度读取失败（{Source}），继续使用主源数据。", gpuSource);
                    }
                }

                // 转速数据源独立选择：与温度源相同时直接复用快照（避免重复读取），
                // 不同则调用所选转速 Provider；读取失败不中断本轮采样。
                // 按枚举名比较而不是数值：LenovoWmi 在温度枚举是 5、转速枚举是 4，
                // 数值比较会把「联想温度源 + 联想转速源」误判成不同源。
                var fanRpm = snapshot.FanRpm;
                if (!string.Equals(
                        config.FanSpeedSource.ToString(),
                        config.TemperatureSource.ToString(),
                        StringComparison.Ordinal))
                {
                    try
                    {
                        fanRpm = await GetOrCreateFanSpeedProvider(config.FanSpeedSource)
                            .ReadAsync(cancellationToken);
                    }
                    catch (Exception rpmEx)
                    {
                        _logger.LogWarning(rpmEx, "风扇转速读取失败（{Source}），本轮无转速数据。", config.FanSpeedSource);
                        fanRpm = snapshot.FanRpm;
                    }
                }

                var targetPwm = _fanController.CalculateTargetPwm(
                    config.FanControlMode,
                    snapshot.CpuTemperatureCelsius ?? double.NaN,
                    snapshot.GpuTemperatureCelsius ?? snapshot.CpuTemperatureCelsius ?? double.NaN,
                    config.ManualPwmPercent,
                    config.Curve,
                    // 必须是本轮真正读到的转速：温度源与转速源不同时 snapshot.FanRpm 恒为 null，
                    // 用它会一直把 TargetRpm 映射到曲线首点（0%），风扇根本转不起来。
                    fanRpm,
                    config.RpmCurve);

                var smoothedPwm = ApplySmoothing(config, targetPwm);
                // 温度无有效数据（NaN）时保持上次 PWM，不把 NaN/末点 100% 发给固件
                if (!double.IsNaN(smoothedPwm ?? double.NaN))
                {
                    _latestSmoothedPwm = (float)smoothedPwm!.Value;
                }

                _latestCpuCelsius = snapshot.CpuTemperatureCelsius is double c
                    ? (float)c
                    : float.NaN;
                _latestGpuCelsius = snapshot.GpuTemperatureCelsius is double g
                    ? (float)g
                    : float.NaN;

                _appState.Publish(new DataPacket(
                    snapshot.CpuTemperatureCelsius ?? double.NaN,
                    snapshot.GpuTemperatureCelsius ?? double.NaN,
                    fanRpm ?? 0,
                    _latestSmoothedPwm,
                    config.FanControlMode.ToString(),
                    DateTimeOffset.Now,
                    targetPwm ?? double.NaN));

                LastError = null;
                LastErrorInfo = null;
                try
                {
                    DataChanged?.Invoke(this, EventArgs.Empty);
                }
                catch
                {
                    // 订阅方（UI）异常不影响采样循环状态
                }
            }
            catch (OperationCanceledException)
            {
                throw;
            }
            catch (Exception ex)
            {
                var firstError = LastError is null;
                LastError = ex.Message;
                LastErrorInfo = (ex as FanControlException)?.Error ?? ErrorInfo.Of("runtime.samplingFailed", ex.Message);
                // 采样失败按「异常」记：数据源临时读不到，后台下一轮自己重试
                ErrorLog.Write(_logger, LastErrorInfo, ex, $"数据源 {_appState.Current.TemperatureSource}");

                if (firstError)
                {
                    TemperatureErrorOccurred?.Invoke(this, EventArgs.Empty);
                }

                // ATKACPI 在华硕新机型上多数不支持温度方法（G-Helper 也弃用），
                // 首次失败时自动切换到 LibreHardwareMonitor 并持久化，避免用户卡在不可用数据源上。
                if (!_autoFallbackDone
                    && _appState.Current.TemperatureSource == TemperatureSource.AtkAcpi
                    && ex is NotSupportedException or InvalidOperationException)
                {
                    _autoFallbackDone = true;
                    _logger.LogWarning("ATKACPI 当前机型不支持温度读取，自动切换到 LibreHardwareMonitor。");
                    await _appState.ApplyAsync(
                        _appState.Current with
                        {
                            TemperatureSource = TemperatureSource.LibreHardwareMonitor,
                        });
                    RaiseConfigChanged();
                }
            }

            var sampleMs = (int)stopwatch.ElapsedMilliseconds;
            var remaining = sampleIntervalMs - sampleMs;
            if (remaining > 0)
            {
                try
                {
                    await Task.Delay(remaining, cancellationToken);
                }
                catch (OperationCanceledException)
                {
                    break;
                }
            }

            // 实际周期明显超过目标时记录偏差，便于定位"设置间隔与实际刷新不一致"
            var cycleMs = (int)stopwatch.ElapsedMilliseconds;
            if (cycleMs > sampleIntervalMs * 1.5 + 500)
            {
                _logger.LogWarning(
                    "实际采样周期 {Cycle}ms 超过目标 {Interval}ms（本轮采样耗时 {Sample}ms）",
                    cycleMs,
                    sampleIntervalMs,
                    sampleMs);
            }
        }
    }

    /// <summary>
    /// 通信循环（仅蓝牙）：互斥型轮询 —— 已连接按 1-30 秒发送 PWM；
    /// 未连接按 3-300 秒尝试重连（可关闭）。串口由采样循环负责，此处仅空转。
    /// </summary>
    /// <summary>
    /// 通信循环：COM 与 BLE 统一复用同一套轮询时间——
    /// 已连接按 BlePollIntervalSeconds 发送 PWM+温度+首次下发时间；
    /// 未连接按 BleReconnectIntervalSeconds 尝试重连（BLE 受自动重连开关控制，COM 始终重连）。
    /// </summary>
    private async Task RunCommunicationLoopAsync(CancellationToken cancellationToken)
    {
        while (!cancellationToken.IsCancellationRequested)
        {
            var config = _appState.Current;
            _commConfigDirty = false;
            await EnsureCommConfigAsync(config);
            var channel = GetChannel(config.CommunicationType);

            // 连接状态变化事件（托盘状态用）；断开时只在「这条连接真的通过」时补一次通知
            if (channel.IsConnected != _bleWasConnected)
            {
                _bleWasConnected = channel.IsConnected;
                if (_bleWasConnected)
                {
                    _bleLinkVerified = false;
                    _bleOutageNotified = false;
                }
                else if (_bleLinkVerified)
                {
                    // 连接成功过、写过数据，这次才算「断开」：连都没连上就失败不弹
                    _bleLinkVerified = false;
                    RaiseBleDisconnectedOnce();
                }

                try
                {
                    BleConnectionChanged?.Invoke(this, _bleWasConnected);
                }
                catch
                {
                    // 订阅方异常不影响通信循环
                }
            }

            var pollInterval = channel.IsConnected
                ? TimeSpan.FromSeconds(Math.Clamp(config.BlePollIntervalSeconds, 1, 30))
                : TimeSpan.FromSeconds(Math.Clamp(config.BleReconnectIntervalSeconds, 1, 60));

            // 手动重建：断开当前通道并立即重连（不受自动重连开关限制）
            var forceReconnect = _forceReconnect;
            _forceReconnect = false;
            if (forceReconnect)
            {
                // 手动「重建连接」不算掉线：清掉连接标记，避免下一轮误判成断开而弹通知
                await DisconnectChannelsAsync();
                _bleWasConnected = false;
                _bleLinkVerified = false;
                channel = GetChannel(config.CommunicationType);
            }

            try
            {
                if (channel.IsConnected)
                {
                    // 连接状态下每个轮询周期都下发时间：固件重启/首条丢失时也能在下一轮重新校时
                    await channel.SendTimeAsync(DateTime.Now, cancellationToken);
                    // 有一次写入成功，说明链路是真的通的：这条连接断开时才值得通知
                    _bleLinkVerified = true;

                    if (!float.IsNaN(_latestSmoothedPwm))
                    {
                        await channel.SendAsync(new PwmCommand(_latestSmoothedPwm), cancellationToken);
                        await channel.SendTemperaturesAsync(
                            _latestCpuCelsius,
                            float.IsNaN(_latestGpuCelsius) ? null : _latestGpuCelsius,
                            cancellationToken);
                    }

                    SetConnectionState(CommunicationState.Connected);
                    ClearCommError();
                }
                else
                {
                    var shouldReconnect = config.AutoReconnectBle || forceReconnect;

                    if (shouldReconnect)
                    {
                        await channel.ConnectAsync(cancellationToken);
                    }

                    SetConnectionState(CommunicationState.Reconnecting);
                }
            }
            catch (OperationCanceledException)
            {
                throw;
            }
            catch (Exception ex)
            {
                SetConnectionState(CommunicationState.Reconnecting);
                SetCommError(ex);

                // 写入失败说明链路已经不可用（设备关机、走远、固件重启）：
                // 不把通道标成断开的话 IsConnected 会一直是 true —— 既不会重连，也永远不会触发断连通知。
                if (channel.IsConnected)
                {
                    try
                    {
                        await channel.DisconnectAsync(cancellationToken).ConfigureAwait(false);
                    }
                    catch
                    {
                        // 释放失败不影响下一轮重连
                    }
                }
            }

            // 等待下个周期；配置变更时立即退出等待，马上按新配置重连
            var waitUntil = DateTime.UtcNow + pollInterval;
            while (DateTime.UtcNow < waitUntil && !_commConfigDirty)
            {
                try
                {
                    await Task.Delay(250, cancellationToken);
                }
                catch (OperationCanceledException)
                {
                    break;
                }
            }
        }
    }

    /// <summary>
    /// PWM 时间窗口平滑：记录最近 PwmSmoothingWindowSeconds 秒内的目标 PWM，
    /// 输出其时间加权平均，温度剧烈波动时转速平缓过渡。
    /// 平滑强度只由窗口秒数决定，与采样间隔无关（采样 100ms 或 5s 效果一致）。
    /// </summary>
    private double? ApplySmoothing(AppConfig config, double? targetPwm)
    {
        // 无有效目标（null 或 NaN，如温度源无数据）时保持上次平滑值，不污染平滑状态
        if (targetPwm is not double target || double.IsNaN(target))
        {
            return null;
        }

        // 平滑开关关闭或手动模式：直接使用目标 PWM，不做平滑
        if (!config.SmoothingEnabled || config.FanControlMode == FanControlMode.Manual)
        {
            _smoother.Reset();
            return target;
        }

        // 切换控制模式/温度源时清空窗口：新通道的目标值与旧通道无可比性，
        // 继续累积会让切换瞬间的转速缓慢爬行
        if (_lastMode != config.FanControlMode || _lastSource != config.TemperatureSource)
        {
            _lastMode = config.FanControlMode;
            _lastSource = config.TemperatureSource;
            _smoother.Reset();
        }

        return _smoother.Update(target, _clock.Elapsed.TotalSeconds, config.PwmSmoothingWindowSeconds);
    }

    private ITemperatureProvider GetOrCreateProvider(TemperatureSource source)
    {
        if (_provider is null || _provider.Source != source)
        {
            _provider = _providerFactory.Create(source);
            try
            {
                TemperatureSourceChanged?.Invoke(this, EventArgs.Empty);
            }
            catch
            {
                // 订阅方异常不影响采样循环状态
            }
        }

        return _provider;
    }

    /// <summary>
    /// GPU 独立温度源：与 CPU 源分开缓存，避免两个 Provider 轮流重建触发
    /// TemperatureSourceChanged（会导致仪表盘趋势图每轮被清空而闪烁）。
    /// </summary>
    private ITemperatureProvider GetOrCreateGpuProvider(TemperatureSource source)
    {
        if (_gpuProvider is null || _gpuProvider.Source != source)
        {
            _gpuProvider = _providerFactory.Create(source);
        }

        return _gpuProvider;
    }

    private IFanSpeedProvider GetOrCreateFanSpeedProvider(FanSpeedSource source)
    {
        if (_fanSpeedProvider is null || _fanSpeedProvider.Source != source)
        {
            _fanSpeedProvider = _fanSpeedProviderFactory.Create(source);
        }

        return _fanSpeedProvider;
    }

    private ICommunicationChannel GetChannel(CommunicationType type)
    {
        if (type == CommunicationType.Com)
        {
            return _comChannel ??= _channelFactory.Create(CommunicationType.Com);
        }

        return _bleChannel ??= _channelFactory.Create(CommunicationType.Ble);
    }

    /// <summary>
    /// 通信配置变更检测：切换端口/波特率/BLE 设备（MAC）/通信方式后，
    /// 断开旧通道并重置重连状态，下一周期按新配置重新握手。
    /// </summary>
    private async Task EnsureCommConfigAsync(AppConfig config)
    {
        var signature = $"{config.CommunicationType}|{config.ComPort}|{config.ComBaudRate}|{config.BleDeviceMac}|{config.BleDeviceName}";
        if (signature == _commSignature)
        {
            return;
        }

        _commSignature = signature;
        _logger.LogInformation("通信配置变更（{Signature}），断开旧通道并重新握手。", signature);
        _commConfigDirty = true;

        await DisconnectChannelsAsync();
        _bleWasConnected = false;
        _bleLinkVerified = false;
        SetConnectionState(CommunicationState.Waiting);
    }

    private async Task DisconnectChannelsAsync()
    {
        foreach (var channel in new[] { _comChannel, _bleChannel })
        {
            if (channel?.IsConnected == true)
            {
                try
                {
                    await channel.DisconnectAsync();
                }
                catch
                {
                    // 释放失败不影响后续
                }
            }
        }

        _comChannel = null;
        _bleChannel = null;
    }

    /// <summary>记录通信失败：同一原因只记一次，避免每个重连周期刷日志。</summary>
    private void SetCommError(Exception ex)
    {
        var info = (ex as FanControlException)?.Error ?? ErrorInfo.Of("comm.failed", ex.Message);
        if (LastCommError == ex.Message && LastCommErrorInfo?.Code == info.Code)
        {
            return;
        }

        LastCommError = ex.Message;
        LastCommErrorInfo = info;
        // 通信链路失败按「异常」记：同一原因只写一条，避免重连周期刷日志
        ErrorLog.Write(_logger, info);
    }

    private void ClearCommError()
    {
        if (LastCommError is null)
        {
            return;
        }

        LastCommError = null;
        LastCommErrorInfo = null;
    }

    /// <summary>一次断线只通知一次：重连成功后由连接状态跳变复位。</summary>
    private void RaiseBleDisconnectedOnce()
    {
        // 通知只针对蓝牙链路：串口掉线不进系统通知
        if (_appState.Current.CommunicationType != CommunicationType.Ble)
        {
            return;
        }

        var now = Environment.TickCount64;
        if (_bleOutageNotified || now - _lastBleNotifyTicks < 60_000)
        {
            return;
        }

        _bleOutageNotified = true;
        _lastBleNotifyTicks = now;
        try
        {
            BleDisconnected?.Invoke(this, EventArgs.Empty);
        }
        catch
        {
            // 订阅方异常不影响通信循环
        }
    }

    private void SetConnectionState(CommunicationState state)
    {
        if (ConnectionState == state)
        {
            return;
        }

        ConnectionState = state;
        try
        {
            ConnectionStateChanged?.Invoke(this, EventArgs.Empty);
        }
        catch
        {
            // 订阅方异常不影响通信循环
        }
    }
}
