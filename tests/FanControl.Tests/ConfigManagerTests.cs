using FanControl.Service.Config;
using FanControl.Shared.Enums;
using FanControl.Shared.Models;
using Microsoft.Extensions.Logging.Abstractions;

namespace FanControl.Tests;

public class ConfigManagerTests : IDisposable
{
    private readonly string _root = Path.Combine(
        Path.GetTempPath(),
        "FanControl.Tests." + Guid.NewGuid().ToString("N"));

    // AppData 根目录 = _root（注入 systemConfigDirectory）；exe 根目录 = installDir\Userdata
    private ConfigManager CreateManager(string? installDirectory = null) =>
        new(
            NullLogger<ConfigManager>.Instance,
            _root,
            installDirectory ?? Path.Combine(_root, "install"));

    private string AppDataConfigDir => Path.Combine(_root, "Config");
    private string ExeConfigDir => Path.Combine(Path.Combine(_root, "install"), "Userdata", "Config");

    [Fact]
    public async Task LoadSystemConfig_MissingFile_ReturnsDefaults()
    {
        var manager = CreateManager();

        var config = await manager.LoadSystemConfigAsync();

        Assert.Equal(ConfigLocation.AppData, config.UserDataLocation);
    }

    [Fact]
    public async Task SaveThenLoadSystemConfig_RoundTrip()
    {
        var manager = CreateManager();

        await manager.SaveSystemConfigAsync(new SystemConfig
        {
            UserDataLocation = ConfigLocation.ExeDirectory,
            LogEnabled = false,
        });

        var loaded = await manager.LoadSystemConfigAsync();

        Assert.Equal(ConfigLocation.ExeDirectory, loaded.UserDataLocation);
        Assert.False(loaded.LogEnabled);
        Assert.True(File.Exists(Path.Combine(ExeConfigDir, "system.json")));
    }

    [Fact]
    public void GetLogDirectory_RespectsUserDataLocation()
    {
        var manager = CreateManager();

        Assert.Equal(
            Path.Combine(_root, "Logs"),
            manager.GetLogDirectory(new SystemConfig { UserDataLocation = ConfigLocation.AppData }));
        Assert.Equal(
            Path.Combine(Path.Combine(_root, "install"), "Userdata", "Logs"),
            manager.GetLogDirectory(new SystemConfig { UserDataLocation = ConfigLocation.ExeDirectory }));
    }

    [Fact]
    public void GetAppConfigFilePath_FollowsUserDataLocation()
    {
        var manager = CreateManager();

        Assert.Equal(
            Path.Combine(AppDataConfigDir, "appconfig.json"),
            manager.GetAppConfigFilePath(new SystemConfig { UserDataLocation = ConfigLocation.AppData }));
        Assert.Equal(
            Path.Combine(ExeConfigDir, "appconfig.json"),
            manager.GetAppConfigFilePath(new SystemConfig { UserDataLocation = ConfigLocation.ExeDirectory }));
    }

    [Fact]
    public async Task LoadAppConfig_MissingFile_ReturnsDefaults()
    {
        var manager = CreateManager();

        var config = await manager.LoadAppConfigAsync();

        Assert.Equal(TemperatureSource.LibreHardwareMonitor, config.TemperatureSource);
        Assert.Equal(FanControlMode.CpuTemp, config.FanControlMode);
        Assert.Equal("COM3", config.ComPort);
    }

    [Fact]
    public async Task SaveThenLoadAppConfig_AppDataRoundTrip()
    {
        var manager = CreateManager();
        await manager.SaveSystemConfigAsync(new SystemConfig
        {
            UserDataLocation = ConfigLocation.AppData,
        });

        var expected = new AppConfig
        {
            TemperatureSource = TemperatureSource.AtkAcpi,
            FanControlMode = FanControlMode.Mixed,
            CommunicationType = CommunicationType.Ble,
            BleDeviceName = "ESP32-Fan",
            Theme = ThemeType.Dark,
        };
        await manager.SaveAppConfigAsync(expected);

        Assert.True(File.Exists(Path.Combine(AppDataConfigDir, "appconfig.json")));
        var actual = await manager.LoadAppConfigAsync();
        Assert.Equal(expected.TemperatureSource, actual.TemperatureSource);
        Assert.Equal(expected.FanControlMode, actual.FanControlMode);
        Assert.Equal(expected.CommunicationType, actual.CommunicationType);
        Assert.Equal(expected.BleDeviceName, actual.BleDeviceName);
        Assert.Equal(expected.Theme, actual.Theme);
    }

    [Fact]
    public async Task SaveThenLoadAppConfig_ExeDirectoryMode()
    {
        var manager = CreateManager();
        await manager.SaveSystemConfigAsync(new SystemConfig
        {
            UserDataLocation = ConfigLocation.ExeDirectory,
        });

        var expected = new AppConfig { FanControlMode = FanControlMode.SystemFan };
        await manager.SaveAppConfigAsync(expected);

        Assert.True(File.Exists(Path.Combine(ExeConfigDir, "appconfig.json")));
        Assert.Equal(
            FanControlMode.SystemFan,
            (await manager.LoadAppConfigAsync()).FanControlMode);
    }

    [Fact]
    public async Task CorruptJson_ReturnsDefaults()
    {
        var manager = CreateManager();
        Directory.CreateDirectory(AppDataConfigDir);
        await File.WriteAllTextAsync(Path.Combine(AppDataConfigDir, "system.json"), "{ not json !!");
        await File.WriteAllTextAsync(Path.Combine(AppDataConfigDir, "appconfig.json"), "###");

        var system = await manager.LoadSystemConfigAsync();
        var app = await manager.LoadAppConfigAsync();

        Assert.Equal(ConfigLocation.AppData, system.UserDataLocation);
        Assert.Equal(TemperatureSource.LibreHardwareMonitor, app.TemperatureSource);
    }

    [Fact]
    public async Task SaveSystemConfig_LocationChange_MigratesAppConfig()
    {
        var manager = CreateManager();

        // 初始在 AppData 保存配置
        await manager.SaveSystemConfigAsync(new SystemConfig
        {
            UserDataLocation = ConfigLocation.AppData,
        });
        var expected = new AppConfig { FanControlMode = FanControlMode.SystemFan };
        await manager.SaveAppConfigAsync(expected);
        Assert.True(File.Exists(Path.Combine(AppDataConfigDir, "appconfig.json")));

        // 切换到 exe 目录：配置应随之迁移
        await manager.SaveSystemConfigAsync(new SystemConfig
        {
            UserDataLocation = ConfigLocation.ExeDirectory,
        });

        Assert.False(File.Exists(Path.Combine(AppDataConfigDir, "appconfig.json")));
        Assert.True(File.Exists(Path.Combine(ExeConfigDir, "appconfig.json")));
        Assert.Equal(
            FanControlMode.SystemFan,
            (await manager.LoadAppConfigAsync()).FanControlMode);
    }

    [Fact]
    public async Task SaveSystemConfig_LocationChange_MigratesLogs()
    {
        var manager = CreateManager();

        await manager.SaveSystemConfigAsync(new SystemConfig
        {
            UserDataLocation = ConfigLocation.AppData,
        });
        var oldLogs = Path.Combine(_root, "Logs");
        Directory.CreateDirectory(oldLogs);
        await File.WriteAllTextAsync(Path.Combine(oldLogs, "fancontrol-20260101.log"), "line");

        await manager.SaveSystemConfigAsync(new SystemConfig
        {
            UserDataLocation = ConfigLocation.ExeDirectory,
        });

        Assert.True(File.Exists(
            Path.Combine(Path.Combine(_root, "install"), "Userdata", "Logs", "fancontrol-20260101.log")));
        Assert.False(File.Exists(Path.Combine(oldLogs, "fancontrol-20260101.log")));
    }

    [Fact]
    public async Task SaveSystemConfig_NoLocationChange_DoesNotMigrate()
    {
        var manager = CreateManager();

        await manager.SaveSystemConfigAsync(new SystemConfig
        {
            UserDataLocation = ConfigLocation.AppData,
        });
        var marker = Path.Combine(AppDataConfigDir, "appconfig.json");
        Directory.CreateDirectory(AppDataConfigDir);
        await File.WriteAllTextAsync(marker, "keep-me");

        await manager.SaveSystemConfigAsync(new SystemConfig
        {
            UserDataLocation = ConfigLocation.AppData,
        });

        Assert.True(File.Exists(marker));
        Assert.Equal("keep-me", await File.ReadAllTextAsync(marker));
    }

    [Fact]
    public async Task SaveSystemConfig_FirstSave_NoPriorConfig_MigrateIsNoOp()
    {
        var manager = CreateManager();

        await manager.SaveSystemConfigAsync(new SystemConfig
        {
            UserDataLocation = ConfigLocation.ExeDirectory,
        });

        var loaded = await manager.LoadSystemConfigAsync();
        Assert.Equal(ConfigLocation.ExeDirectory, loaded.UserDataLocation);
    }

    [Fact]
    public void LegacyLayout_NormalizedToConfigSubdirectory()
    {
        // 构造前先写入旧版布局：system.json / appconfig.json 平铺在根目录，日志在根目录 Logs
        Directory.CreateDirectory(Path.Combine(_root, "Logs"));
        File.WriteAllText(Path.Combine(_root, "system.json"), "{\"userDataLocation\":1}");
        File.WriteAllText(Path.Combine(_root, "appconfig.json"), "{}");
        File.WriteAllText(Path.Combine(_root, "Logs", "fancontrol-20260101.log"), "line");

        // 构造时自动迁移到新布局
        var manager = CreateManager();

        Assert.True(File.Exists(Path.Combine(AppDataConfigDir, "system.json")));
        Assert.True(File.Exists(Path.Combine(AppDataConfigDir, "appconfig.json")));
        Assert.True(File.Exists(Path.Combine(_root, "Logs", "fancontrol-20260101.log")));
        Assert.False(File.Exists(Path.Combine(_root, "system.json")));
        Assert.False(File.Exists(Path.Combine(_root, "appconfig.json")));
    }

    public void Dispose()
    {
        if (Directory.Exists(_root))
        {
            Directory.Delete(_root, recursive: true);
        }
    }
}
