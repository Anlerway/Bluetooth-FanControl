using System.IO;
using FanControl.Service.Logging;
using FanControl.Shared.Contracts;

namespace FanControl.Tests;

public class ErrorSeverityTests
{
    [Theory]
    [InlineData("config.rangeInt", ErrorSeverity.Validation)]
    [InlineData("config.curveFlat", ErrorSeverity.Validation)]
    [InlineData("curve.notIncreasing", ErrorSeverity.Validation)]
    [InlineData("runtimeCurve.empty", ErrorSeverity.Validation)]
    [InlineData("ipc.missingKey", ErrorSeverity.Validation)]
    [InlineData("config.applyFailed", ErrorSeverity.Error)]
    [InlineData("ipc.unknownCommand", ErrorSeverity.Error)]
    [InlineData("ble.notConfigured", ErrorSeverity.Notice)]
    [InlineData("ble.timeout", ErrorSeverity.Warning)]
    [InlineData("com.openTimeout", ErrorSeverity.Warning)]
    [InlineData("comm.failed", ErrorSeverity.Warning)]
    [InlineData("runtime.samplingFailed", ErrorSeverity.Warning)]
    [InlineData("autostart.taskFailed", ErrorSeverity.Error)]
    [InlineData("error.generic", ErrorSeverity.Error)]
    public void MapsCodeToSeverity(string code, ErrorSeverity expected)
        => Assert.Equal(expected, ErrorSeverityMap.For(code));

    [Fact]
    public void ExplicitSeverityWinsOverCodePrefix()
    {
        var info = ErrorInfo.Of("ble.timeout", "超时").WithSeverity(ErrorSeverity.Error);

        Assert.Equal(ErrorSeverity.Error, info.Severity);
        Assert.Equal("ble.timeout", info.Code);
        Assert.Equal("超时", info.Message);
    }

    [Theory]
    [InlineData(ErrorSeverity.Notice, "提示")]
    [InlineData(ErrorSeverity.Warning, "异常")]
    [InlineData(ErrorSeverity.Error, "错误")]
    [InlineData(ErrorSeverity.Validation, "验证错误")]
    public void LevelTagsMatchSeverity(ErrorSeverity severity, string expected)
        => Assert.Equal(expected, LevelTags.Of(severity));

    [Fact]
    public void ValidationErrorIsTaggedInFileLog()
    {
        var directory = Path.Combine(Path.GetTempPath(), "fancontrol-severity-test-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(directory);
        try
        {
            using (var provider = new FileLoggerProvider(directory))
            {
                ErrorLog.Write(provider.CreateLogger("test"), ErrorInfo.Of("config.rangeInt", "必须在 0–100 之间。"));
            }

            var text = File.ReadAllText(Directory.GetFiles(directory, "fancontrol-*.log").Single());
            Assert.Contains("[验证错误]", text);
            Assert.Contains("config.rangeInt", text);
        }
        finally
        {
            Directory.Delete(directory, true);
        }
    }
}
