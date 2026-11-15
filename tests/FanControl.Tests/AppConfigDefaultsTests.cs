using FanControl.Shared.Models;

namespace FanControl.Tests;

/// <summary>
/// 默认曲线是 1.4.0 现场调出来的那条（9 个点）：新装用户第一次打开界面就该看到这条，
/// 而不是早期占位的 4 点/6 点折线。改默认值时必须同步这里，避免又被改回占位值。
/// </summary>
public class AppConfigDefaultsTests
{
    [Fact]
    public void DefaultTemperatureCurve_MatchesTunedProfile()
    {
        var config = new AppConfig();

        Assert.Equal(9, config.Curve.Count);
        Assert.Equal(0.3, config.Curve[0].TemperatureCelsius, 3);
        Assert.Equal(21.2, config.Curve[0].PwmPercent, 3);
        Assert.Equal(53.5, config.Curve[4].TemperatureCelsius, 3);
        Assert.Equal(51.3, config.Curve[4].PwmPercent, 3);
        Assert.Equal(119.7, config.Curve[8].TemperatureCelsius, 3);
        Assert.Equal(75.4, config.Curve[8].PwmPercent, 3);

        // 曲线必须严格递增（ConfigPatch 校验规则），否则默认值直接非法
        for (var i = 1; i < config.Curve.Count; i++)
        {
            Assert.True(config.Curve[i].TemperatureCelsius > config.Curve[i - 1].TemperatureCelsius);
        }
    }

    [Fact]
    public void DefaultRpmCurve_MatchesTunedProfile()
    {
        var config = new AppConfig();

        Assert.Equal(9, config.RpmCurve.Count);
        Assert.Equal(87, config.RpmCurve[0].Rpm, 3);
        Assert.Equal(25.5, config.RpmCurve[0].PwmPercent, 3);
        Assert.Equal(5390, config.RpmCurve[5].Rpm, 3);
        Assert.Equal(69.0, config.RpmCurve[5].PwmPercent, 3);
        Assert.Equal(9957, config.RpmCurve[8].Rpm, 3);
        Assert.Equal(77.1, config.RpmCurve[8].PwmPercent, 3);

        for (var i = 1; i < config.RpmCurve.Count; i++)
        {
            Assert.True(config.RpmCurve[i].Rpm > config.RpmCurve[i - 1].Rpm);
        }
    }
}
