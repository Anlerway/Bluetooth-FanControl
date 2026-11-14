namespace FanControl.Service.Logging;

/// <summary>
/// 最近若干条运行日志的内存环形缓冲：界面「运行日志」页按级别上色、按 seq 增量拉取。
/// 与是否写盘无关（关掉文件日志后界面仍然能看当前会话），也不做任何格式化——文案由界面自己拼。
/// </summary>
public sealed class LogFeed
{
    public sealed record Entry(long Seq, string Time, int Level, string Tag, string Category, string Message);

    private const int Capacity = 2000;
    private readonly object _sync = new();
    private readonly Queue<Entry> _entries = new();
    private long _seq;

    /// <summary>记一条（level：0 提示 / 1 异常 / 2 错误 / 3 验证错误）。</summary>
    public void Add(DateTime time, string tag, int level, string category, string message)
    {
        lock (_sync)
        {
            _entries.Enqueue(new Entry(++_seq, time.ToString("HH:mm:ss.fff"), level, tag, category, message));
            while (_entries.Count > Capacity)
            {
                _entries.Dequeue();
            }
        }
    }

    /// <summary>取 seq 之后最多 limit 条：界面用返回的 lastSeq 做下次增量的起点。</summary>
    public List<Entry> Since(long seq, int limit)
    {
        lock (_sync)
        {
            var result = new List<Entry>(Math.Min(limit, _entries.Count));
            foreach (var entry in _entries)
            {
                if (entry.Seq <= seq)
                {
                    continue;
                }

                result.Add(entry);
                if (result.Count >= limit)
                {
                    break;
                }
            }

            return result;
        }
    }

    public long LastSeq
    {
        get
        {
            lock (_sync)
            {
                return _seq;
            }
        }
    }

    /// <summary>清空内存缓冲：不动磁盘上的日志文件。</summary>
    public void Clear()
    {
        lock (_sync)
        {
            _entries.Clear();
        }
    }
}
