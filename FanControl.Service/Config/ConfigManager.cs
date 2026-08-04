using System.Text.Json;
using FanControl.Shared.Contracts;
using FanControl.Shared.Enums;
using FanControl.Shared.Models;
using Microsoft.Extensions.Logging;

namespace FanControl.Service.Config;

/// <summary>
/// JSON 配置管理器。
/// - 数据根目录：%AppData%\FanControl（AppData 模式）或 exe 所在目录\Userdata（ExeDirectory 模式）；
/// - 子目录固定：根目录\Config\（system.json + appconfig.json）、根目录\Logs\（日志）；
/// - 切换数据位置时，已有 Config\ 与 Logs\ 整体迁移到新根目录，避免数据丢失；
/// - 写入采用临时文件 + 原子替换，损坏/缺失时回退默认值并记录日志；
/// - 构造时自动把旧版布局（根目录平铺 / data 子目录）迁移到新布局。
/// </summary>
public sealed class ConfigManager : IConfigManager
{
    private readonly ILogger<ConfigManager> _logger;
    private readonly string _installDirectory;
    private readonly string _appDataRoot;   // %AppData%\FanControl（测试可注入）
    private readonly string _exeDataRoot;   // exe 所在目录\Userdata

    public ConfigManager(
        ILogger<ConfigManager> logger,
        string? systemConfigDirectory = null,
        string? installDirectory = null)
    {
        _logger = logger;
        _installDirectory = installDirectory ?? AppContext.BaseDirectory;

        if (systemConfigDirectory is null)
        {
            var appData = Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData);
            _appDataRoot = Path.Combine(appData, "FanControl");
        }
        else
        {
            _appDataRoot = systemConfigDirectory;
        }

        _exeDataRoot = Path.Combine(_installDirectory, "Userdata");
        NormalizeLegacyLayout();
    }

    /// <summary>程序安装目录。</summary>
    public string InstallDirectory => _installDirectory;

    /// <summary>AppData 模式数据根目录（%AppData%\FanControl）。</summary>
    public string AppDataRoot => _appDataRoot;

    /// <summary>exe 模式数据根目录（exe 所在目录\Userdata）。</summary>
    public string ExeDataRoot => _exeDataRoot;

    /// <summary>
    /// system.json 定位：优先新布局（根目录\Config\system.json），兼容旧布局（根目录平铺 / data 子目录）。
    /// 不存在时返回默认目标（AppData 根目录\Config\system.json）。
    /// </summary>
    public string SystemConfigFilePath
    {
        get
        {
            foreach (var candidate in new[]
            {
                Path.Combine(_appDataRoot, "Config", "system.json"),
                Path.Combine(_exeDataRoot, "Config", "system.json"),
                Path.Combine(_appDataRoot, "system.json"),
                Path.Combine(_appDataRoot, "data", "system.json"),
                Path.Combine(_exeDataRoot, "system.json"),
            })
            {
                if (File.Exists(candidate))
                {
                    return candidate;
                }
            }

            return Path.Combine(_appDataRoot, "Config", "system.json");
        }
    }

    /// <summary>按系统配置解析数据根目录（AppData 或 exe\Userdata）。</summary>
    public string GetDataRoot(SystemConfig systemConfig)
        => systemConfig.UserDataLocation == ConfigLocation.ExeDirectory
            ? _exeDataRoot
            : _appDataRoot;

    /// <summary>按系统配置解析配置目录（根目录\Config，存放 system.json 与 appconfig.json）。</summary>
    public string GetConfigDirectory(SystemConfig systemConfig)
        => Path.Combine(GetDataRoot(systemConfig), "Config");

    /// <summary>按系统配置解析日志目录（根目录\Logs）。</summary>
    public string GetLogDirectory(SystemConfig systemConfig)
        => Path.Combine(GetDataRoot(systemConfig), "Logs");

    /// <summary>按系统配置解析用户配置（appconfig.json）完整路径。</summary>
    public string GetAppConfigFilePath(SystemConfig systemConfig)
        => Path.Combine(GetConfigDirectory(systemConfig), "appconfig.json");

    public async Task<SystemConfig> LoadSystemConfigAsync(CancellationToken cancellationToken = default)
    {
        var result = await TryReadJsonAsync<SystemConfig>(SystemConfigFilePath, cancellationToken);
        if (result is null)
        {
            if (File.Exists(SystemConfigFilePath))
            {
                _logger.LogWarning(
                    "system.json 读取失败或损坏，使用默认系统配置。路径: {Path}",
                    SystemConfigFilePath);
            }

            return new SystemConfig();
        }

        return result;
    }

    public async Task SaveSystemConfigAsync(SystemConfig config, CancellationToken cancellationToken = default)
    {
        // 位置变化时，把已有数据（Config\ + Logs\）整体迁移到新的数据根目录，避免丢失。
        var previous = await LoadSystemConfigAsync(cancellationToken);
        if (previous.UserDataLocation != config.UserDataLocation)
        {
            MigrateDataRoot(previous.UserDataLocation, config.UserDataLocation);
        }

        await WriteJsonAsync(
            Path.Combine(GetConfigDirectory(config), "system.json"),
            config,
            cancellationToken);
    }

    public async Task<AppConfig> LoadAppConfigAsync(CancellationToken cancellationToken = default)
    {
        var path = GetAppConfigFilePath(await LoadSystemConfigAsync(cancellationToken));
        var result = await TryReadJsonAsync<AppConfig>(path, cancellationToken);
        if (result is null)
        {
            if (File.Exists(path))
            {
                _logger.LogWarning(
                    "appconfig.json 读取失败或损坏，使用默认应用配置。路径: {Path}",
                    path);
            }

            return new AppConfig();
        }

        return result;
    }

    public async Task SaveAppConfigAsync(AppConfig config, CancellationToken cancellationToken = default)
    {
        var path = GetAppConfigFilePath(await LoadSystemConfigAsync(cancellationToken));
        await WriteJsonAsync(path, config, cancellationToken);
    }

    /// <summary>切换数据位置：把旧根目录的 Config\ 与 Logs\ 迁移到新根目录。</summary>
    private void MigrateDataRoot(ConfigLocation from, ConfigLocation to)
    {
        var fromRoot = from == ConfigLocation.ExeDirectory ? _exeDataRoot : _appDataRoot;
        var toRoot = to == ConfigLocation.ExeDirectory ? _exeDataRoot : _appDataRoot;

        if (string.Equals(fromRoot, toRoot, StringComparison.OrdinalIgnoreCase))
        {
            return;
        }

        try
        {
            MoveDirectoryContents(Path.Combine(fromRoot, "Config"), Path.Combine(toRoot, "Config"));
            MoveDirectoryContents(Path.Combine(fromRoot, "Logs"), Path.Combine(toRoot, "Logs"));
            _logger.LogInformation("用户数据已迁移：{From} -> {To}", fromRoot, toRoot);
        }
        catch (Exception ex)
        {
            _logger.LogWarning(ex, "用户数据迁移失败（可能因目标目录无写权限），保留原位置数据。");
        }
    }

    /// <summary>
    /// 旧版布局兼容迁移（构造时执行一次）：
    /// 旧版 system.json 固定位于 %AppData%\FanControl\system.json 或 data\system.json；
    /// 旧版配置/日志可能平铺在数据根目录或 data\ 子目录、或 exe 目录下（appconfig.json / Logs）。
    /// 统一迁入新布局：根目录\Config\ 与根目录\Logs\。
    /// </summary>
    private void NormalizeLegacyLayout()
    {
        try
        {
            // 1) 定位旧 system.json 并按其记录的位置模式迁入对应根目录的新布局
            string? legacySystem = null;
            foreach (var p in new[]
            {
                Path.Combine(_appDataRoot, "system.json"),
                Path.Combine(_appDataRoot, "data", "system.json"),
                Path.Combine(_exeDataRoot, "system.json"),
            })
            {
                if (File.Exists(p))
                {
                    legacySystem = p;
                    break;
                }
            }

            if (legacySystem is not null)
            {
                var mode = ConfigLocation.AppData;
                try
                {
                    using var doc = JsonDocument.Parse(File.ReadAllText(legacySystem));
                    var root = doc.RootElement;
                    if (root.TryGetProperty("userDataLocation", out var loc)
                        || root.TryGetProperty("configLocation", out loc))
                    {
                        mode = (ConfigLocation)loc.GetInt32();
                    }
                }
                catch
                {
                    // 无法解析时按 AppData 处理
                }

                var targetRoot = mode == ConfigLocation.ExeDirectory ? _exeDataRoot : _appDataRoot;
                MoveFile(legacySystem, Path.Combine(targetRoot, "Config", "system.json"));
            }

            // 2) AppData 根目录旧布局（平铺 / data 子目录）-> 新布局
            MoveFile(Path.Combine(_appDataRoot, "appconfig.json"), Path.Combine(_appDataRoot, "Config", "appconfig.json"));
            MoveFile(Path.Combine(_appDataRoot, "data", "appconfig.json"), Path.Combine(_appDataRoot, "Config", "appconfig.json"));
            MoveDirectoryContents(Path.Combine(_appDataRoot, "data", "Logs"), Path.Combine(_appDataRoot, "Logs"));

            // 3) exe 目录旧布局（appconfig.json / Logs 平铺在 exe 目录）-> exe\Userdata 新布局
            MoveFile(Path.Combine(_installDirectory, "appconfig.json"), Path.Combine(_exeDataRoot, "Config", "appconfig.json"));
            MoveDirectoryContents(Path.Combine(_installDirectory, "Logs"), Path.Combine(_exeDataRoot, "Logs"));
        }
        catch (Exception ex)
        {
            _logger.LogWarning(ex, "旧数据布局迁移失败（不影响新布局使用）。");
        }
    }

    private static void MoveFile(string from, string to)
    {
        if (!File.Exists(from) || string.Equals(from, to, StringComparison.OrdinalIgnoreCase))
        {
            return;
        }

        Directory.CreateDirectory(Path.GetDirectoryName(to)!);
        File.Move(from, to, overwrite: true);
    }

    private static void MoveDirectoryContents(string from, string to)
    {
        if (!Directory.Exists(from) || string.Equals(from, to, StringComparison.OrdinalIgnoreCase))
        {
            return;
        }

        Directory.CreateDirectory(to);
        foreach (var file in Directory.GetFiles(from))
        {
            File.Move(file, Path.Combine(to, Path.GetFileName(file)), overwrite: true);
        }

        foreach (var directory in Directory.GetDirectories(from))
        {
            MoveDirectoryContents(directory, Path.Combine(to, Path.GetFileName(directory)));
        }

        try
        {
            Directory.Delete(from, recursive: true);
        }
        catch
        {
            // 残留空目录不影响功能
        }
    }

    private static async Task<T?> TryReadJsonAsync<T>(string path, CancellationToken cancellationToken)
        where T : class
    {
        if (!File.Exists(path))
        {
            return null;
        }

        try
        {
            await using var stream = File.OpenRead(path);
            return await JsonSerializer.DeserializeAsync<T>(stream, JsonDefaults.Options, cancellationToken);
        }
        catch (JsonException)
        {
            return null;
        }
        catch (IOException)
        {
            return null;
        }
    }

    private static async Task WriteJsonAsync<T>(string path, T value, CancellationToken cancellationToken)
        where T : class
    {
        var directory = Path.GetDirectoryName(path);
        if (!string.IsNullOrEmpty(directory))
        {
            Directory.CreateDirectory(directory);
        }

        var temporaryPath = path + ".tmp";
        await File.WriteAllTextAsync(
            temporaryPath,
            JsonSerializer.Serialize(value, JsonDefaults.Options),
            cancellationToken);

        File.Move(temporaryPath, path, overwrite: true);
    }
}
