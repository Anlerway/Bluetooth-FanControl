using FanControl.Service.Hardware;
using FanControl.Service.Hardware.Lenovo;
using FanControl.Shared.Enums;
using Microsoft.Extensions.Logging.Abstractions;

namespace FanControl.Tests;

public sealed class LenovoFanTableReaderTests
{
    [Fact]
    public void SensorIds_SortedDistinct_CpuIsFirst_GpuIsSecond()
    {
        var rows = new[]
        {
            Row(fanId: 0, sensorId: 2),
            Row(fanId: 1, sensorId: 0),
            Row(fanId: 1, sensorId: 1),
        };

        Assert.Equal(new[] { 0, 1, 2 }, LenovoFanTableReader.SensorIds(rows));
        Assert.Equal(0, LenovoFanTableReader.CpuSensorId(rows));
        Assert.Equal(1, LenovoFanTableReader.GpuSensorId(rows));
    }

    [Fact]
    public void SensorIds_SingleSensor_NoGpuSensor()
    {
        var rows = new[] { Row(sensorId: 3) };

        Assert.Equal(3, LenovoFanTableReader.CpuSensorId(rows));
        Assert.Null(LenovoFanTableReader.GpuSensorId(rows));
    }

    [Fact]
    public void SensorIds_EmptyTable_NoSensor()
    {
        Assert.Null(LenovoFanTableReader.CpuSensorId(Array.Empty<LenovoFanTableRow>()));
        Assert.Null(LenovoFanTableReader.GpuSensorId(Array.Empty<LenovoFanTableRow>()));
    }

    [Fact]
    public void FanIds_SortedDistinct_DefaultsToFanZero()
    {
        var rows = new[] { Row(fanId: 1), Row(fanId: 0), Row(fanId: 1) };

        Assert.Equal(new[] { 0, 1 }, LenovoFanTableReader.FanIds(rows));
        Assert.Equal(new[] { 0 }, LenovoFanTableReader.FanIds(Array.Empty<LenovoFanTableRow>()));
    }

    [Fact]
    public void MaxFanRpm_PrefersCurrentThenDefaultThenTable()
    {
        var speeds = new ushort[] { 0, 1000, 2000, 3000, 4000, 5000, 5600, 6000, 6200, 6400, 6600 };

        Assert.Equal(4200, Row(speeds: speeds, currentMax: 4200, defaultMax: 5000).MaxFanRpm);
        Assert.Equal(5000, Row(speeds: speeds, currentMax: null, defaultMax: 5000).MaxFanRpm);
        Assert.Equal(6600, Row(speeds: speeds).MaxFanRpm);
        Assert.Equal(0, Row(speeds: Array.Empty<ushort>()).MaxFanRpm);
    }

    [Theory]
    [InlineData(0, 0)]
    [InlineData(1, 1000)]
    [InlineData(5, 5000)]
    [InlineData(10, 6600)]
    public void FanRpm_LevelReadings_UseFanTable(int raw, double expected)
    {
        var row = Row(speeds: new ushort[] { 0, 1000, 2000, 3000, 4000, 5000, 5600, 6000, 6200, 6400, 6600 });

        Assert.Equal(expected, LenovoFanTableReader.FanRpm(raw, row));
    }

    [Fact]
    public void FanRpm_PercentReading_UsesMaxFanRpm()
    {
        var row = Row(speeds: new ushort[] { 0, 1000, 2000 }, currentMax: 4000);

        Assert.Equal(2000, LenovoFanTableReader.FanRpm(50, row));
        Assert.Equal(4000, LenovoFanTableReader.FanRpm(100, row));
    }

    [Fact]
    public void FanRpm_RawRpm_PassesThrough()
    {
        Assert.Equal(2500, LenovoFanTableReader.FanRpm(2500, null));
        Assert.Equal(2500, LenovoFanTableReader.FanRpm(2500, Row()));
    }

    [Fact]
    public void FanRpm_Implausible_ReturnsNull()
    {
        Assert.Null(LenovoFanTableReader.FanRpm(-1, null));
        Assert.Null(LenovoFanTableReader.FanRpm(50, null));
        Assert.Null(LenovoFanTableReader.FanRpm(20000, null));
    }

    [Fact]
    public void IsPlausibleTemperature_RejectsPlaceholders()
    {
        Assert.False(LenovoFanTableReader.IsPlausibleTemperature(0));
        Assert.True(LenovoFanTableReader.IsPlausibleTemperature(55));
        Assert.False(LenovoFanTableReader.IsPlausibleTemperature(127));
        Assert.False(LenovoFanTableReader.IsPlausibleTemperature(65535));
    }

    [Fact]
    public void ReadFanRpm_TakesMaxAcrossFans()
    {
        var backend = new FakeLenovoBackend();
        backend.Rows.Add(Row(fanId: 0, sensorId: 0, speeds: new ushort[] { 0, 1000, 2000, 3000, 4000, 5000, 5600, 6000, 6200, 6400, 6600 }));
        backend.Rows.Add(Row(fanId: 1, sensorId: 1, speeds: new ushort[] { 0, 900, 1800, 2700, 3600, 4500, 5000, 5400, 5800, 6000, 6200 }));
        backend.FanSpeeds[0] = 4;
        backend.FanSpeeds[1] = 8;

        Assert.Equal(5800, LenovoFanTableReader.ReadFanRpm(backend, backend.Rows));
    }

    [Fact]
    public void ReadFanRpm_SkipsFansWithoutReading()
    {
        var backend = new FakeLenovoBackend();
        backend.Rows.Add(Row(fanId: 0, speeds: new ushort[] { 0, 1000, 2000 }));
        backend.FanSpeeds[0] = 2;

        Assert.Equal(2000, LenovoFanTableReader.ReadFanRpm(backend, backend.Rows));
    }

    [Fact]
    public async Task TemperatureProvider_ReadsCpuGpuAndFanRpm()
    {
        var backend = new FakeLenovoBackend();
        backend.Rows.Add(Row(fanId: 0, sensorId: 0, speeds: new ushort[] { 0, 1000, 2000, 3000, 4000, 5000, 5600, 6000, 6200, 6400, 6600 }));
        backend.Rows.Add(Row(fanId: 1, sensorId: 1, speeds: new ushort[] { 0, 900, 1800, 2700, 3600, 4500, 5000, 5400, 5800, 6000, 6200 }));
        backend.SensorTemps[0] = 55;
        backend.SensorTemps[1] = 61;
        backend.FanSpeeds[0] = 5;
        var provider = new LenovoWmiTemperatureProvider(backend, NullLogger<LenovoWmiTemperatureProvider>.Instance);

        var snapshot = await provider.ReadAsync();

        Assert.Equal(TemperatureSource.LenovoWmi, provider.Source);
        Assert.Equal(55, snapshot.CpuTemperatureCelsius);
        Assert.Equal(61, snapshot.GpuTemperatureCelsius);
        Assert.Equal(5000, snapshot.FanRpm);
    }

    [Fact]
    public async Task TemperatureProvider_FallsBackToGameZoneWithoutSensorTable()
    {
        var backend = new FakeLenovoBackend();
        backend.CpuTemperature = 48;
        backend.GpuTemperature = 52;
        var provider = new LenovoWmiTemperatureProvider(backend, NullLogger<LenovoWmiTemperatureProvider>.Instance);

        var snapshot = await provider.ReadAsync();

        Assert.Equal(48, snapshot.CpuTemperatureCelsius);
        Assert.Equal(52, snapshot.GpuTemperatureCelsius);
        Assert.Null(snapshot.FanRpm);
    }

    [Fact]
    public async Task TemperatureProvider_NotLenovo_ReturnsEmpty()
    {
        var backend = new FakeLenovoBackend { IsAvailable = false };
        backend.SensorTemps[0] = 55;
        backend.Rows.Add(Row());
        var provider = new LenovoWmiTemperatureProvider(backend, NullLogger<LenovoWmiTemperatureProvider>.Instance);

        var snapshot = await provider.ReadAsync();

        Assert.Null(snapshot.CpuTemperatureCelsius);
        Assert.Null(snapshot.GpuTemperatureCelsius);
        Assert.Null(snapshot.FanRpm);
    }

    [Fact]
    public async Task FanSpeedProvider_ReturnsMappedRpm()
    {
        var backend = new FakeLenovoBackend();
        backend.Rows.Add(Row(fanId: 0, speeds: new ushort[] { 0, 1000, 2000, 3000, 4000, 5000, 5600, 6000, 6200, 6400, 6600 }));
        backend.FanSpeeds[0] = 6;
        var provider = new LenovoWmiFanSpeedProvider(backend, NullLogger<LenovoWmiFanSpeedProvider>.Instance);

        Assert.Equal(FanSpeedSource.LenovoWmi, provider.Source);
        Assert.Equal(5600, await provider.ReadAsync());
    }

    [Fact]
    public async Task FanSpeedProvider_NotLenovo_ReturnsNull()
    {
        var backend = new FakeLenovoBackend { IsAvailable = false };
        var provider = new LenovoWmiFanSpeedProvider(backend, NullLogger<LenovoWmiFanSpeedProvider>.Instance);

        Assert.Null(await provider.ReadAsync());
    }

    private static LenovoFanTableRow Row(
        int fanId = 0,
        int sensorId = 0,
        ushort[]? speeds = null,
        ushort[]? temps = null,
        int? currentMax = null,
        int? defaultMax = null)
        => new(
            Mode: null,
            FanId: fanId,
            SensorId: sensorId,
            FanSpeeds: speeds ?? Array.Empty<ushort>(),
            SensorTemps: temps ?? Array.Empty<ushort>(),
            DefaultFanMaxSpeed: defaultMax,
            CurrentFanMaxSpeed: currentMax);

    private sealed class FakeLenovoBackend : ILenovoWmiBackend
    {
        public bool IsAvailable { get; set; } = true;

        public List<LenovoFanTableRow> Rows { get; } = new();

        public Dictionary<int, int> FanSpeeds { get; } = new();

        public Dictionary<int, int> SensorTemps { get; } = new();

        public int? CpuTemperature { get; set; }

        public int? GpuTemperature { get; set; }

        public IReadOnlyList<LenovoFanTableRow> GetFanTable() => Rows;

        public int? GetCurrentFanSpeed(int fanId)
            => FanSpeeds.TryGetValue(fanId, out var value) ? value : null;

        public int? GetCurrentSensorTemperature(int sensorId)
            => SensorTemps.TryGetValue(sensorId, out var value) ? value : null;

        public int? GetCpuTemperature() => CpuTemperature;

        public int? GetGpuTemperature() => GpuTemperature;
    }
}
