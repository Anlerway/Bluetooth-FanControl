namespace FanControl.Service.Fan;

/// <summary>
/// 按时间窗口平滑 PWM：记录最近 WindowSeconds 内的目标值，输出其时间加权平均
/// （等价于对目标值做宽度为窗口的滑动平均 / 积分平均）。
///
/// 与固定系数 EMA 的区别：平滑强度由"窗口秒数"决定，而不是"每个采样点衰减多少"，
/// 因此采样间隔从 1000ms 改成 100ms 时，同样的窗口给出的过渡时长不变。
///
/// 实现约定：
/// - 每个样本代表"从该时刻起保持到下一个样本"的目标值（零阶保持），
///   窗口内按保持时长加权求平均，采样间隔不均匀也不会偏向样本密的区段；
/// - 窗口起点早于最早样本时，从最早样本开始积分（启动阶段不会被拉向 0）；
/// - 无有效目标（null/NaN）时不写入样本，返回上次输出，避免污染窗口；
/// - 时钟回退（长时间挂起后 Stopwatch 异常等）时清空窗口重新累积。
/// </summary>
public sealed class PwmSmoother
{
    private readonly List<Sample> _samples = new(64);

    private readonly record struct Sample(double TimeSeconds, double Value);

    /// <summary>最近一次输出的平滑值；尚无输出时为 null。</summary>
    public double? LastOutput { get; private set; }

    /// <summary>当前窗口内保留的样本数（含窗口起点前的边界样本）。</summary>
    public int SampleCount => _samples.Count;

    /// <summary>清空窗口与上次输出（切换模式/数据源、关闭平滑时调用）。</summary>
    public void Reset()
    {
        _samples.Clear();
        LastOutput = null;
    }

    /// <summary>
    /// 记录一个目标值并返回窗口内的平滑结果。
    /// </summary>
    /// <param name="targetPwm">本次目标 PWM；null 或 NaN 表示本轮无有效目标。</param>
    /// <param name="nowSeconds">单调递增的当前时间（秒）。</param>
    /// <param name="windowSeconds">平滑窗口长度（秒）；&lt;= 0 表示不平滑。</param>
    public double? Update(double? targetPwm, double nowSeconds, double windowSeconds)
    {
        if (targetPwm is not double target || double.IsNaN(target))
        {
            // 无有效目标：保持上次输出，不写入样本
            return LastOutput;
        }

        if (!(windowSeconds > 0) || double.IsInfinity(windowSeconds))
        {
            // 窗口无效（0/负数/NaN/无穷）：直通
            Reset();
            LastOutput = target;
            return target;
        }

        if (_samples.Count > 0 && nowSeconds < _samples[^1].TimeSeconds)
        {
            Reset();
        }

        _samples.Add(new Sample(nowSeconds, target));

        // 只保留窗口起点之前的最后一个样本作为边界（窗口起点处的取值由它决定）
        var cutoff = nowSeconds - windowSeconds;
        var drop = 0;
        while (drop + 1 < _samples.Count && _samples[drop + 1].TimeSeconds <= cutoff)
        {
            drop++;
        }

        if (drop > 0)
        {
            _samples.RemoveRange(0, drop);
        }

        var start = Math.Max(cutoff, _samples[0].TimeSeconds);
        var span = nowSeconds - start;
        if (span <= 0)
        {
            // 同一时刻重复更新：直接取最新值
            LastOutput = target;
            return target;
        }

        var integral = 0.0;
        for (var i = 0; i < _samples.Count; i++)
        {
            var from = Math.Max(_samples[i].TimeSeconds, start);
            var to = i + 1 < _samples.Count ? _samples[i + 1].TimeSeconds : nowSeconds;
            if (to > from)
            {
                integral += _samples[i].Value * (to - from);
            }
        }

        LastOutput = integral / span;
        return LastOutput;
    }
}
