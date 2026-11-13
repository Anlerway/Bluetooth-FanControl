using FanControl.Shared.Contracts;
using Xunit;

namespace FanControl.Tests;

public sealed class BleDeviceMatcherPathTests
{
    [Theory]
    [InlineData(@"BTHLE\DEV_489D31E38902\9&477A29F&0&489D31E38902", "489D31E38902")]
    [InlineData(@"BTHLEDevice\{00001800-0000-1000-8000-00805F9B34FB}_489D31E38902\A&3A91C584&5&0014", "489D31E38902")]
    [InlineData(@"BTHLEDevice\{6e400001-b5a3-f393-e0a9-e50e24dcca9e}_cd05a089f4b9\a&39107ada&0&0028", "CD05A089F4B9")]
    public void ParsesMacFromDeviceInstanceId(string instanceId, string expected)
    {
        Assert.Equal(expected, BleDeviceMatcher.ParseMacFromDevicePath(instanceId));
    }

    [Theory]
    [InlineData(null)]
    [InlineData("")]
    [InlineData(@"BTHLE\DEV_000000000000\9&1&0")]
    [InlineData(@"USB\VID_1234&PID_5678\5&1&2")]
    public void ReturnsEmptyWhenNoMacPresent(string? instanceId)
    {
        Assert.Equal(string.Empty, BleDeviceMatcher.ParseMacFromDevicePath(instanceId));
    }
}
