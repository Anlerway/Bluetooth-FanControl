using System.Text.Json;
using FanControl.Shared.Contracts;
using FanControl.Shared.Enums;
using FanControl.Shared.Models;
using Microsoft.Extensions.Logging;

namespace FanControl.Service.Config;

/// <summary>
/// JSON 配置管理器。
/// - 数据根目录：%AppData%\FanControl（AppData 模式）、exe 所在目录\Userdata（ExeDirectory 模式），
///   或首次运行向导里挑的任意文件夹（Custom 模式，路径记在 system.json 的 userDataPath）；
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
    private string? _customRoot;            // location.txt 指向的自定义数据根，没有则为 null

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
        _customRoot = ReadLocationPointer(_appDataRoot);
        NormalizeLegacyLayout();
    }

    /// <summary>程序安装目录。</summary>
    public string InstallDirectory => _installDirectory;

    /// <summary>AppData 模式数据根目录（%AppData%\FanControl）。</summary>
    public string AppDataRoot => _appDataRoot;

    /// <summary>exe 模式数据根目录（exe 所在目录\Userdata）。</summary>
    public string ExeDataRoot => _exeDataRoot;

    /// <summary>自定义数据根（location.txt 指向的目录）；没选过自定义则为 null。</summary>
    public string? CustomDataRoot => _customRoot;

    /// <summary>自定义数据根的指路文件。system.json 自己住在新目录里，只能靠它找回来。</summary>
    public string LocationPointerPath => Path.Combine(_appDataRoot, "location.txt");

    /// <summary>还没写过 system.json（首次运行）：界面据此弹一次欢迎向导。</summary>
    public bool IsFirstRun => !File.Exists(SystemConfigFilePath);

    /// <summary>
    /// system.json 定位：优先新布局（根目录\Config\system.json），兼容旧布局（根目录平铺 / data 子目录）。
    /// 不存在时返回默认目标（AppData 根目录\Config\system.json）。
    /// </summary>
    public string SystemConfigFilePath
    {
        get
        {
            foreach (var candidate in CandidateSystemConfigPaths())
            {
                if (File.Exists(candidate))
                {
                    return candidate;
                }
            }

            // 一个都没有：默认写到当前有效的根目录（自定义过就写回那个目录）
            return Path.Combine(_customRoot ?? _appDataRoot, "Config", "system.json");
        }
    }

    private IEnumerable<string> CandidateSystemConfigPaths()
    {
        if (_customRoot is not null)
        {
            yield return Path.Combine(_customRoot, "Config", "system.json");
        }

        yield return Path.Combine(_appDataRoot, "Config", "system.json");
        yield return Path.Combine(_exeDataRoot, "Config", "system.json");
        yield return Path.Combine(_appDataRoot, "system.json");
        yield return Path.Combine(_appDataRoot, "data", "system.json");
        yield return Path.Combine(_exeDataRoot, "system.json");
    }

    /// <summary>按系统配置解析数据根目录（AppData / exe\Userdata / 自定义）。</summary>
    public string GetDataRoot(SystemConfig systemConfig)
        => ResolveRoot(systemConfig.UserDataLocation, systemConfig.UserDataPath);

    /// <summary>把位置模式解析成数据根；自定义但路径不可用时退回 AppData。</summary>
    public string ResolveRoot(ConfigLocation location, string? customPath)
        => location switch
        {
            ConfigLocation.ExeDirectory => _exeDataRoot,
            ConfigLocation.Custom when !string.IsNullOrWhiteSpace(customPath) && Path.IsPathFullyQualified(customPath)
                => customPath.Trim(),
            _ => _appDataRoot,
        };

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
        var previous = await LoadSystemConfigAsync(cancellationToken).ConfigureAwait(false);
        var fromRoot = GetDataRoot(previous);
        var toRoot = GetDataRoot(config);

        // 顺序有讲究：先写新根目录的 system.json，再写/删指针，最后搬家。
        // 中间任何一步崩溃，下次启动都还能找到一份能用的 system.json（最多丢日志）。
        await WriteJsonAsync(
            Path.Combine(GetConfigDirectory(config), "system.json"),
            config,
            cancellationToken).ConfigureAwait(false);
        UpdateLocationPointer(config);

        if (!string.Equals(fromRoot, toRoot, StringComparison.OrdinalIgnoreCase))
        {
            MigrateDataRoot(fromRoot, toRoot);
        }
    }

    /// <summary>
    /// 维护 %AppData%\FanControl\location.txt：Custom 模式写绝对路径，其余模式删掉。
    /// 这个文件是唯一无条件可写的位置（AppData），所以指针只能放在这里。
    /// </summary>
    private void UpdateLocationPointer(SystemConfig config)
    {
        var path = config.UserDataLocation == ConfigLocation.Custom ? config.UserDataPath?.Trim() : null;
        if (!string.IsNullOrEmpty(path) && Path.IsPathFullyQualified(path))
        {
            try
            {
                Directory.CreateDirectory(_appDataRoot);
                File.WriteAllText(LocationPointerPath, path);
                _customRoot = path;
            }
            catch (Exception ex)
            {
                _logger.LogWarning(ex, "写自定义位置指针失败：{Path}", LocationPointerPath);
            }

            return;
        }

        try
        {
            if (File.Exists(LocationPointerPath))
            {
                File.Delete(LocationPointerPath);
            }
        }
        catch (Exception ex)
        {
            _logger.LogWarning(ex, "删除自定义位置指针失败：{Path}", LocationPointerPath);
        }

        _customRoot = null;
    }

    private static string? ReadLocationPointer(string appDataRoot)
    {
        try
        {
            var path = Path.Combine(appDataRoot, "location.txt");
            if (!File.Exists(path))
            {
                return null;
            }

            var text = File.ReadAllText(path).Trim();
            // 目录被用户删掉/挪走时指针作废，回落到 AppData，不能把启动卡死
            return text.Length > 0 && Path.IsPathFullyQualified(text) && Directory.Exists(text) ? text : null;
        }
        catch
        {
            return null;
        }
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
        try
        {
            await WriteJsonAsync(path, config, cancellationToken);
        }
        catch (DirectoryNotFoundException)
        {
            // 数据目录被用户删掉/挪走时先补建，再写一次：配置文件丢了不该让整次设置更改失败
            var directory = Path.GetDirectoryName(path);
            if (!string.IsNullOrEmpty(directory))
            {
                Directory.CreateDirectory(directory);
            }

            await WriteJsonAsync(path, config, cancellationToken);
        }
    }

    /// <summary>切换数据位置：把旧根目录的 Config\ 与 Logs\ 迁移到新根目录。</summary>
    private void MigrateDataRoot(string fromRoot, string toRoot)
    {
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
