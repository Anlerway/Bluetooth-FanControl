using System.Runtime.InteropServices;

namespace FanControl.Service.Hardware;

/// <summary>
/// ACPI 热区温度计数器（PDH 版）：等价于
/// <c>new PerformanceCounter("Thermal Zone Information", "Temperature", "\\_TZ.THRM")</c>，
/// 但只依赖 pdh.dll，不需要 System.Diagnostics.PerformanceCounter 包
/// （该程序集原本由 Microsoft.WindowsDesktop.App 提供，去掉 WinForms 后就不再可用）。
/// </summary>
internal static class AcpiThermalZoneCounter
{
    private const string CounterPath = @"\Thermal Zone Information(\_TZ.THRM)\Temperature";
    private const uint PdhFmtDouble = 0x00000200;
    private const uint PdhCstatusValidData = 0x00000000;
    private static readonly TimeSpan RetryInterval = TimeSpan.FromSeconds(60);

    private static readonly object Sync = new();
    private static IntPtr _query;
    private static IntPtr _counter;
    private static long _nextRetryTimestamp;
    private static bool _unavailable;

    /// <summary>读取热区温度（摄氏度）；计数器不可用时返回 null，并在 60 秒后再试。</summary>
    public static double? Read()
    {
        lock (Sync)
        {
            if (!EnsureOpen())
            {
                return null;
            }

            if (PdhCollectQueryData(_query) != 0)
            {
                Close();
                return null;
            }

            var status = PdhGetFormattedCounterValue(_counter, PdhFmtDouble, out _, out var value);
            if (status != 0 || value.CStatus != PdhCstatusValidData)
            {
                return null;
            }

            // 与 G-Helper 一致：开尔文值，合理区间内换算为摄氏度
            var kelvin = value.DoubleValue;
            return kelvin > 0 && kelvin < 500 ? kelvin - 273.15 : null;
        }
    }

    private static bool EnsureOpen()
    {
        if (_counter != IntPtr.Zero)
        {
            return true;
        }

        if (_unavailable && Environment.TickCount64 < _nextRetryTimestamp)
        {
            return false;
        }

        if (PdhOpenQueryW(null, UIntPtr.Zero, out var query) != 0)
        {
            MarkUnavailable();
            return false;
        }

        if (PdhAddCounterW(query, CounterPath, UIntPtr.Zero, out var counter) != 0 || counter == IntPtr.Zero)
        {
            PdhCloseQuery(query);
            MarkUnavailable();
            return false;
        }

        _query = query;
        _counter = counter;
        _unavailable = false;
        return true;
    }

    private static void MarkUnavailable()
    {
        _unavailable = true;
        _nextRetryTimestamp = Environment.TickCount64 + (long)RetryInterval.TotalMilliseconds;
    }

    private static void Close()
    {
        if (_query != IntPtr.Zero)
        {
            PdhCloseQuery(_query);
        }

        _query = IntPtr.Zero;
        _counter = IntPtr.Zero;
    }

    [StructLayout(LayoutKind.Explicit)]
    private struct PdhFmtCounterValue
    {
        [FieldOffset(0)]
        public uint CStatus;

        [FieldOffset(8)]
        public double DoubleValue;
    }

    [DllImport("pdh.dll", CharSet = CharSet.Unicode)]
    private static extern uint PdhOpenQueryW(string? dataSource, UIntPtr userData, out IntPtr query);

    [DllImport("pdh.dll", CharSet = CharSet.Unicode)]
    private static extern uint PdhAddCounterW(IntPtr query, string fullCounterPath, UIntPtr userData, out IntPtr counter);

    [DllImport("pdh.dll")]
    private static extern uint PdhCollectQueryData(IntPtr query);

    [DllImport("pdh.dll")]
    private static extern uint PdhGetFormattedCounterValue(IntPtr counter, uint format, out uint type, out PdhFmtCounterValue value);

    [DllImport("pdh.dll")]
    private static extern uint PdhCloseQuery(IntPtr query);
}
