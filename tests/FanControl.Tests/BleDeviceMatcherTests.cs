using FanControl.Shared.Contracts;

namespace FanControl.Tests;

public class BleDeviceMatcherTests
{
    [Fact]
    public void FormatMacAddress_ProducesColonSeparatedUppercase()
    {
        var formatted = BleDeviceMatcher.FormatMacAddress(0xAABBCCDDEEFF);

        Assert.Equal("AA:BB:CC:DD:EE:FF", formatted);
    }

    [Theory]
    [InlineData("AA:BB:CC:DD:EE:FF", "aa:bb:cc:dd:ee:ff")]
    [InlineData("aabbccddeeff", "AA-BB-CC-DD-EE-FF")]
    [InlineData("AA:BB:CC:DD:EE:FF", "aabbccddeeff")]
    public void MacEquals_IgnoresCaseAndSeparators(string left, string right)
        => Assert.True(BleDeviceMatcher.MacEquals(left, right));

    [Theory]
    [InlineData("AA:BB:CC:DD:EE:FF", "AA:BB:CC:DD:EE:00")]
    [InlineData("AA:BB:CC:DD:EE:FF", "")]
    [InlineData("", "AA:BB:CC:DD:EE:FF")]
    [InlineData("AA:BB:CC:DD:EE", "AA:BB:CC:DD:EE:FF")]
    public void MacEquals_RejectsDifferentOrIncompleteValues(string left, string right)
        => Assert.False(BleDeviceMatcher.MacEquals(left, right));

    [Fact]
    public void ParseMacFromId_ReadsDeviceAddressFromWindowsId()
    {
        const string id = "BluetoothLE#BluetoothLE11:22:33:44:55:66-aa:bb:cc:dd:ee:ff";

        Assert.Equal("AABBCCDDEEFF", BleDeviceMatcher.ParseMacFromId(id));
    }

    [Fact]
    public void ParseMacFromId_EmptyOrNonMacId_ReturnsEmpty()
    {
        Assert.Equal(string.Empty, BleDeviceMatcher.ParseMacFromId(null));
        Assert.Equal(string.Empty, BleDeviceMatcher.ParseMacFromId(string.Empty));
        Assert.Equal(string.Empty, BleDeviceMatcher.ParseMacFromId("USB#VID_1234&PID_5678#7&1a2b3c4d&0&1"));
    }
}
