using System.Text.Json;
using FanControl.Host.Ui;
using FanControl.Shared.Enums;
using FanControl.Shared.Models;
using Xunit;

namespace FanControl.Tests;

/// <summary>IPC 配置补丁：错误码是界面文案的唯一来源，改这里必须同步 docs/PROTOCOL.md 的 code 表。</summary>
public class ConfigPatchTests
{
    private static JsonElement Json(string json) => JsonDocument.Parse(json).RootElement;

    [Fact]
    public void UnknownKeyReportsKeyName()
    {
        var ok = ConfigPatch.TryApply(new AppConfig(), "nopeKey", Json("1"), out _, out var error);

        Assert.False(ok);
        Assert.NotNull(error);
        Assert.Equal("config.unknownKey", error!.Code);
        Assert.Equal("nopeKey", error.Arg1);
    }

    [Fact]
    public void UnknownEnumValueReportsTypeName()
    {
        var ok = ConfigPatch.TryApply(new AppConfig(), "theme", Json("\"Purple\""), out _, out var error);

        Assert.False(ok);
        Assert.Equal("config.enum", error!.Code);
        Assert.Equal(nameof(ThemeType), error.Arg1);
    }

    [Fact]
    public void OutOfRangeIntegerReportsBounds()
    {
        var ok = ConfigPatch.TryApply(new AppConfig(), "pollIntervalMilliseconds", Json("9999"), out _, out var error);

        Assert.False(ok);
        Assert.Equal("config.rangeInt", error!.Code);
        Assert.Equal("100", error.Arg1);
        Assert.Equal("5000", error.Arg2);
    }

    [Fact]
    public void NonBooleanReportsNotBool()
    {
        var ok = ConfigPatch.TryApply(new AppConfig(), "notifyOnBleDisconnect", Json("1"), out _, out var error);

        Assert.False(ok);
        Assert.Equal("config.notBool", error!.Code);
    }

    [Fact]
    public void ValidValueUpdatesConfig()
    {
        var ok = ConfigPatch.TryApply(new AppConfig(), "theme", Json("\"Dark\""), out var updated, out var error);

        Assert.True(ok);
        Assert.Null(error);
        Assert.Equal(ThemeType.Dark, updated.Theme);
    }

    [Fact]
    public void LanguageIsAccepted()
    {
        var ok = ConfigPatch.TryApply(new AppConfig(), "language", Json("\"en-US\""), out var updated, out _);

        Assert.True(ok);
        Assert.Equal("en-US", updated.Language);
    }

    [Fact]
    public void CurveNeedsTwoPoints()
    {
        var ok = ConfigPatch.TrySetCurve(new AppConfig(), new[] { (40d, 30d) }, out _, out var error);

        Assert.False(ok);
        Assert.Equal("config.curveTooFew", error!.Code);
    }

    [Fact]
    public void FlatCurveIsRejected()
    {
        var ok = ConfigPatch.TrySetCurve(new AppConfig(), new[] { (40d, 30d), (80d, 30d) }, out _, out var error);

        Assert.False(ok);
        Assert.Equal("config.curveFlat", error!.Code);
    }

    [Fact]
    public void CurveIsSortedAndClamped()
    {
        var ok = ConfigPatch.TrySetCurve(new AppConfig(), new[] { (90d, 100d), (30d, -5d) }, out var updated, out _);

        Assert.True(ok);
        Assert.Equal(2, updated.Curve.Count);
        Assert.Equal(30d, updated.Curve[0].TemperatureCelsius);
        Assert.Equal(0d, updated.Curve[0].PwmPercent);
        Assert.Equal(90d, updated.Curve[1].TemperatureCelsius);
    }

    [Fact]
    public void RpmCurveIsClipped()
    {
        var ok = ConfigPatch.TrySetRpmCurve(new AppConfig(), new[] { (0d, 0d), (30000d, 120d) }, out var updated, out _);

        Assert.True(ok);
        Assert.Equal(20000d, updated.RpmCurve[^1].Rpm);
        Assert.Equal(100d, updated.RpmCurve[^1].PwmPercent);
    }
}
