using System.Globalization;
using System.Windows;
using System.Windows.Media;
using FanControl.Shared.Enums;
using FanControl.UI.Localization;

namespace FanControl.UI.Controls;

/// <summary>
/// 温度趋势图（自绘）：固定长度环形缓冲 + 平滑曲线 + CPU/GPU 温度（左轴）+ 目标 PWM（右轴）。
/// 缓冲、点列表与画笔都在实例上复用，渲染过程不产生逐帧分配。
/// </summary>
public sealed class TrendChart : FrameworkElement
{
    private const int MaxPoints = 240;
    private static readonly Typeface LabelTypeface = new("Microsoft YaHei UI");

    private readonly (double Cpu, double Gpu, double Pwm)[] _buffer = new (double, double, double)[MaxPoints];
    private readonly List<Point> _points = new(MaxPoints);

    private int _count;
    private int _next;
    private TemperatureUnit _unit = TemperatureUnit.Celsius;
    private double _pixelsPerDip = 1.0;

    private Pen? _borderPen;
    private Pen? _gridPen;
    private Pen? _cpuPen;
    private Pen? _gpuPen;
    private Pen? _pwmPen;
    private int _penGeneration = -1;

    private enum Series
    {
        Cpu,
        Gpu,
        Pwm,
    }

    public TrendChart()
    {
        ClipToBounds = true;
    }

    public TemperatureUnit TemperatureUnit
    {
        get => _unit;
        set
        {
            if (_unit != value)
            {
                _unit = value;
                InvalidateVisual();
            }
        }
    }

    /// <summary>环形缓冲第 index 条（0 = 最旧）。</summary>
    private (double Cpu, double Gpu, double Pwm) At(int index)
        => _buffer[(_next - _count + index + MaxPoints * 2) % MaxPoints];

    public void AddPoint(double cpu, double gpu, double pwm)
    {
        _buffer[_next] = (cpu, gpu, pwm);
        _next = (_next + 1) % MaxPoints;
        if (_count < MaxPoints)
        {
            _count++;
        }

        InvalidateVisual();
    }

    public void Clear()
    {
        _count = 0;
        _next = 0;
        InvalidateVisual();
    }

    protected override void OnRender(DrawingContext dc)
    {
        EnsurePens();

        var size = RenderSize;
        dc.DrawRectangle(ThemeService.Brush("ChartBgBrush"), _borderPen, new Rect(size));

        var labelBrush = ThemeService.Brush("TextSecondaryBrush");
        _pixelsPerDip = VisualTreeHelper.GetDpi(this).PixelsPerDip;

        if (_count == 0)
        {
            DrawText(dc, LocalizationManager.Get("Chart.Waiting"), size.Width / 2, size.Height / 2, labelBrush, false);
            return;
        }

        // 左侧留温度轴，右侧留 PWM 轴
        var plot = new Rect(48, 8, Math.Max(10, size.Width - 112), Math.Max(10, size.Height - 30));

        double minValue = double.MaxValue;
        double maxValue = double.MinValue;
        var hasGpu = false;
        for (var i = 0; i < _count; i++)
        {
            var sample = At(i);
            if (!double.IsNaN(sample.Cpu))
            {
                minValue = Math.Min(minValue, sample.Cpu);
                maxValue = Math.Max(maxValue, sample.Cpu);
            }

            if (!double.IsNaN(sample.Gpu))
            {
                hasGpu = true;
                minValue = Math.Min(minValue, sample.Gpu);
                maxValue = Math.Max(maxValue, sample.Gpu);
            }
        }

        // 所有温度源都无有效数据时（例如某个源获取失败）不画温度轴，避免 NaN 坐标导致渲染异常
        if (minValue > maxValue)
        {
            return;
        }

        var minTemp = Math.Max(0, Math.Floor((minValue - 2) / 10) * 10);
        var maxTemp = Math.Max(minTemp + 10, Math.Ceiling((maxValue + 2) / 10) * 10);

        for (var t = minTemp; t <= maxTemp + 0.001; t += 10)
        {
            var y = plot.Bottom - (t - minTemp) / (maxTemp - minTemp) * plot.Height;
            dc.DrawLine(_gridPen, new Point(plot.Left, y), new Point(plot.Right, y));
            var display = TemperatureUnitHelper.ToDisplay(t, _unit);
            DrawText(dc, $"{display:0}°", 40, y, labelBrush, true);
        }

        // 右轴：目标 PWM 0-100%
        for (var p = 0; p <= 100.001; p += 20)
        {
            var y = plot.Bottom - p / 100.0 * plot.Height;
            dc.DrawLine(_gridPen, new Point(plot.Right, y), new Point(plot.Right + 40, y));
            DrawText(dc, $"{p:0}", plot.Right + 46, y, labelBrush, false);
        }

        DrawSeries(dc, plot, minTemp, maxTemp, Series.Cpu, _cpuPen!);
        if (hasGpu)
        {
            DrawSeries(dc, plot, minTemp, maxTemp, Series.Gpu, _gpuPen!);
        }

        DrawSeries(dc, plot, 0, 100, Series.Pwm, _pwmPen!);

        DrawLegend(dc, plot.Right - 128, plot.Top, ThemeService.Brush("AccentBrush"), LocalizationManager.Get("Chart.Cpu"));
        if (hasGpu)
        {
            DrawLegend(dc, plot.Right - 86, plot.Top, ThemeService.Brush("GpuBrush"), LocalizationManager.Get("Chart.Gpu"));
        }

        DrawLegend(dc, plot.Right - 44, plot.Top, Brushes.Orange, LocalizationManager.Get("Chart.Pwm"));
    }

    private void EnsurePens()
    {
        var generation = ThemeService.Generation;
        if (_penGeneration == generation && _cpuPen is not null)
        {
            return;
        }

        _borderPen = new Pen(ThemeService.Brush("BorderBrush"), 1);
        _gridPen = new Pen(ThemeService.Brush("ChartGridBrush"), 1)
        {
            DashStyle = new DashStyle(new double[] { 2, 3 }, 0),
        };
        _cpuPen = new Pen(ThemeService.Brush("AccentBrush"), 2);
        _gpuPen = new Pen(ThemeService.Brush("GpuBrush"), 2);
        _pwmPen = new Pen(Brushes.Orange, 1.5)
        {
            DashStyle = new DashStyle(new double[] { 4, 3 }, 0),
        };
        _penGeneration = generation;
    }

    private void DrawSeries(
        DrawingContext dc,
        Rect plot,
        double minValue,
        double maxValue,
        Series series,
        Pen pen)
    {
        var useRightAxis = series == Series.Pwm;
        var range = useRightAxis ? 100.0 : (maxValue - minValue);
        var baseValue = useRightAxis ? 0.0 : minValue;

        _points.Clear();
        for (var i = 0; i < _count; i++)
        {
            var sample = At(i);
            var value = series switch
            {
                Series.Cpu => sample.Cpu,
                Series.Gpu => sample.Gpu,
                _ => sample.Pwm,
            };

            if (double.IsNaN(value))
            {
                continue;
            }

            var x = plot.Left + i / (double)(MaxPoints - 1) * plot.Width;
            var y = plot.Bottom - (value - baseValue) / range * plot.Height;
            _points.Add(new Point(x, Math.Clamp(y, plot.Top, plot.Bottom)));
        }

        if (_points.Count < 2)
        {
            return;
        }

        // Catmull-Rom 转三次贝塞尔：曲线平滑过渡
        var geometry = new StreamGeometry();
        using (var ctx = geometry.Open())
        {
            ctx.BeginFigure(_points[0], false, false);
            if (_points.Count == 2)
            {
                ctx.LineTo(_points[1], true, false);
            }
            else
            {
                for (var i = 0; i < _points.Count - 1; i++)
                {
                    var p0 = _points[Math.Max(0, i - 1)];
                    var p1 = _points[i];
                    var p2 = _points[i + 1];
                    var p3 = _points[Math.Min(_points.Count - 1, i + 2)];
                    var c1 = new Point(p1.X + (p2.X - p0.X) / 6, p1.Y + (p2.Y - p0.Y) / 6);
                    var c2 = new Point(p2.X - (p3.X - p1.X) / 6, p2.Y - (p3.Y - p1.Y) / 6);
                    ctx.BezierTo(c1, c2, p2, true, false);
                }
            }
        }

        geometry.Freeze();
        dc.DrawGeometry(null, pen, geometry);
    }

    private void DrawLegend(DrawingContext dc, double x, double y, Brush brush, string name)
    {
        dc.DrawRectangle(brush, null, new Rect(x, y + 5, 12, 3));
        DrawText(dc, name, x + 17, y, ThemeService.Brush("TextSecondaryBrush"), false);
    }

    private void DrawText(DrawingContext dc, string text, double x, double y, Brush brush, bool textRight)
    {
        var formatted = new FormattedText(
            text,
            CultureInfo.CurrentCulture,
            FlowDirection.LeftToRight,
            LabelTypeface,
            10,
            brush,
            _pixelsPerDip);

        if (textRight)
        {
            x -= formatted.Width;
        }

        dc.DrawText(formatted, new Point(x, y - formatted.Height / 2));
    }
}
