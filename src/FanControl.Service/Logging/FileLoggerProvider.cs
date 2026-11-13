using System.Threading.Channels;
using Microsoft.Extensions.Logging;

namespace FanControl.Service.Logging;

/// <summary>
/// 文件日志：写入所选目录（用户数据目录或安装目录）\Logs\fancontrol-日期.log。
/// 支持运行时切换目录/开关/保留数量；日志文件最多保留 MaxLogFiles 个，超出自动清除最旧的。
/// 写盘在后台任务上批量进行，采样线程只做一次入队，不再同步阻塞在 File.AppendAllText 上。
/// </summary>
public sealed class FileLoggerProvider : ILoggerProvider
{
    private static readonly TimeSpan PruneInterval = TimeSpan.FromMinutes(10);

    private readonly object _sync = new();
    private readonly Channel<string> _queue = Channel.CreateUnbounded<string>(
        new UnboundedChannelOptions { SingleReader = true, SingleWriter = false });
    private readonly Task _writer;

    private string _directory;
    private bool _enabled = true;
    private int _maxLogFiles = 20;
    private DateTime _lastPruneUtc = DateTime.MinValue;

    public FileLoggerProvider(string directory)
    {
        _directory = directory;
        Prune();
        _writer = Task.Run(WriteLoopAsync);
    }

    /// <summary>运行时更新日志设置（设置页/首启弹窗后立即生效）。</summary>
    public void Configure(string directory, bool enabled, int maxLogFiles)
    {
        lock (_sync)
        {
            _directory = directory;
            _enabled = enabled;
            _maxLogFiles = Math.Max(1, maxLogFiles);
            PruneLocked();
        }
    }

    public ILogger CreateLogger(string categoryName) => new FileLogger(this, categoryName);

    public void Dispose()
    {
        _queue.Writer.TryComplete();
        try
        {
            _writer.Wait(TimeSpan.FromSeconds(2));
        }
        catch
        {
            // 退出阶段不因日志刷盘失败阻塞
        }
    }

    internal void Write(string category, LogLevel level, string message)
    {
        if (!Volatile.Read(ref _enabled))
        {
            return;
        }

        var line = $"{DateTime.Now:HH:mm:ss.fff} [{level}] [{category}] {message}{Environment.NewLine}";

        // 队列满或已关闭时丢弃：日志不能反过来拖慢采样循环
        _queue.Writer.TryWrite(line);
    }

    private async Task WriteLoopAsync()
    {
        var batch = new List<string>(64);
        try
        {
            while (await _queue.Reader.WaitToReadAsync().ConfigureAwait(false))
            {
                while (batch.Count < 128 && _queue.Reader.TryRead(out var line))
                {
                    batch.Add(line);
                }

                Flush(batch);
                batch.Clear();
            }

            while (_queue.Reader.TryRead(out var remaining))
            {
                batch.Add(remaining);
            }

            Flush(batch);
        }
        catch
        {
            // 后台写盘异常不影响主流程
        }
    }

    private void Flush(List<string> lines)
    {
        if (lines.Count == 0)
        {
            return;
        }

        lock (_sync)
        {
            if (!_enabled)
            {
                return;
            }

            try
            {
                Directory.CreateDirectory(_directory);
                var path = Path.Combine(_directory, $"fancontrol-{DateTime.Now:yyyyMMdd}.log");
                File.AppendAllLines(path, lines);

                if (DateTime.UtcNow - _lastPruneUtc >= PruneInterval)
                {
                    PruneLocked();
                }
            }
            catch
            {
                // 日志写入失败不影响主流程
            }
        }
    }

    private void Prune()
    {
        lock (_sync)
        {
            PruneLocked();
        }
    }

    /// <summary>仅保留最新 _maxLogFiles 个常规日志（fancontrol-yyyyMMdd.log，不含崩溃日志）。</summary>
    private void PruneLocked()
    {
        _lastPruneUtc = DateTime.UtcNow;
        try
        {
            if (!Directory.Exists(_directory))
            {
                return;
            }

            var files = Directory.GetFiles(_directory, "fancontrol-*.log")
                .Where(f => !Path.GetFileName(f).Contains("-crash-", StringComparison.OrdinalIgnoreCase))
                .OrderByDescending(f => f)
                .ToList();

            foreach (var oldFile in files.Skip(_maxLogFiles))
            {
                File.Delete(oldFile);
            }
        }
        catch
        {
            // 清理失败不影响主流程
        }
    }

    private sealed class FileLogger : ILogger
    {
        private readonly FileLoggerProvider _owner;
        private readonly string _category;

        public FileLogger(FileLoggerProvider owner, string category)
        {
            _owner = owner;
            _category = category;
        }

        public IDisposable? BeginScope<TState>(TState state)
            where TState : notnull
        {
            return null;
        }

        public bool IsEnabled(LogLevel logLevel)
        {
            // 仅保留关键日志：Warning / Error / Critical，降低日常日志噪音
            return logLevel >= LogLevel.Warning;
        }

        public void Log<TState>(
            LogLevel logLevel,
            EventId eventId,
            TState state,
            Exception? exception,
            Func<TState, Exception?, string> formatter)
        {
            if (!IsEnabled(logLevel))
            {
                return;
            }

            var message = formatter(state, exception);
            if (exception is not null)
            {
                message += $" {exception}";
            }

            _owner.Write(_category, logLevel, message);
        }
    }
}
