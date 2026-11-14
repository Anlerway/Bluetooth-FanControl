using System.Text.Json;
using FanControl.Service.Config;
using FanControl.Shared.Contracts;
using FanControl.Shared.Enums;
using FanControl.Shared.Models;

namespace FanControl.Host.Ui;

/// <summary>
/// system.json 的运行期视图（日志开关、最大日志文件数、数据目录）。这些设置不属于 AppConfig，
/// 由宿主独占写盘；界面通过 setSystem / setDataLocation 命令下发，改动立即落盘。
/// 数据目录同时决定 Config\ 与 Logs\ 的归属：切换时已有数据整体迁移，日志目录当场跟着换。
/// </summary>
internal sealed class SystemSettings
{
    private readonly ConfigManager _configManager;
    private readonly SemaphoreSlim _gate = new(1, 1);
    private SystemConfig _current;

    public SystemSettings(ConfigManager configManager, SystemConfig initial)
    {
        _configManager = configManager;
        _current = initial;
    }

    public SystemConfig Current => Volatile.Read(ref _current);

    /// <summary>当前生效的日志目录（数据目录切换后立刻指向新位置）。</summary>
    public string LogDirectory => _configManager.GetLogDirectory(Current);

    public event EventHandler? Changed;

    public async Task<ErrorInfo?> ApplyAsync(string key, JsonElement value)
    {
        SystemConfig updated;
        switch (key.ToLowerInvariant())
        {
            case "logenabled":
                if (value.ValueKind is not (JsonValueKind.True or JsonValueKind.False))
                {
                    return ErrorInfo.Of("config.notBool", "该配置需要 true 或 false。", key);
                }

                updated = Current with { LogEnabled = value.GetBoolean() };
                break;

            case "userdatalocation":
            {
                // 旧界面传纯字符串；新界面也可以传 { location, path } 一起改
                var locationValue = value;
                string? path = null;
                if (value.ValueKind == JsonValueKind.Object)
                {
                    locationValue = value.TryGetProperty("location", out var inner) ? inner : default;
                    if (value.TryGetProperty("path", out var pathElement) && pathElement.ValueKind == JsonValueKind.String)
                    {
                        path = pathElement.GetString();
                    }
                }

                var location = ParseLocation(locationValue);
                if (location is null)
                {
                    return ErrorInfo.Of("config.enum", "无法识别的取值。", key);
                }

                if (location == ConfigLocation.Custom && !IsUsableCustomPath(path))
                {
                    return ErrorInfo.Of("config.path", "自定义目录必须是完整路径。", path ?? string.Empty);
                }

                updated = Current with
                {
                    UserDataLocation = location.Value,
                    UserDataPath = location == ConfigLocation.Custom ? path!.Trim() : null,
                };
                break;
            }

            case "maxlogfiles":
                if (value.ValueKind != JsonValueKind.Number || !value.TryGetInt32(out var files) || files < 1 || files > 200)
                {
                    return ErrorInfo.Of("config.rangeInt", "取值必须是 1-200 之间的整数。", "1", "200");
                }

                updated = Current with { MaxLogFiles = files };
                break;

            default:
                return ErrorInfo.Of("config.unknownKey", "未知配置项：" + key, key);
        }

        return await SaveAsync(updated).ConfigureAwait(false);
    }

    /// <summary>切换数据目录（首次运行向导与设置页共用）。location 是枚举名。</summary>
    public async Task<ErrorInfo?> SetDataLocationAsync(string locationText, string? path)
    {
        var location = ParseLocationText(locationText);
        if (location is null)
        {
            return ErrorInfo.Of("config.enum", "无法识别的取值。", "userDataLocation");
        }

        if (location == ConfigLocation.Custom && !IsUsableCustomPath(path))
        {
            return ErrorInfo.Of("config.path", "自定义目录必须是完整路径。", path ?? string.Empty);
        }

        var updated = Current with
        {
            UserDataLocation = location.Value,
            UserDataPath = location == ConfigLocation.Custom ? path!.Trim() : null,
        };
        return await SaveAsync(updated).ConfigureAwait(false);
    }

    private async Task<ErrorInfo?> SaveAsync(SystemConfig updated)
    {
        await _gate.WaitAsync().ConfigureAwait(false);
        try
        {
            await _configManager.SaveSystemConfigAsync(updated).ConfigureAwait(false);
            Volatile.Write(ref _current, updated);
        }
        catch (Exception ex)
        {
            return ErrorInfo.Of("config.applyFailed", "保存设置失败：" + ex.Message, ex.Message);
        }
        finally
        {
            _gate.Release();
        }

        Changed?.Invoke(this, EventArgs.Empty);
        return null;
    }

    private static bool IsUsableCustomPath(string? path)
        => !string.IsNullOrWhiteSpace(path) && Path.IsPathFullyQualified(path);

    private static ConfigLocation? ParseLocationText(string? text)
    {
        if (string.IsNullOrWhiteSpace(text))
        {
            return null;
        }

        if (text.Equals(nameof(ConfigLocation.AppData), StringComparison.OrdinalIgnoreCase))
        {
            return ConfigLocation.AppData;
        }

        if (text.Equals(nameof(ConfigLocation.ExeDirectory), StringComparison.OrdinalIgnoreCase))
        {
            return ConfigLocation.ExeDirectory;
        }

        if (text.Equals(nameof(ConfigLocation.Custom), StringComparison.OrdinalIgnoreCase))
        {
            return ConfigLocation.Custom;
        }

        return null;
    }

    private static ConfigLocation? ParseLocation(JsonElement value)
    {
        if (value.ValueKind == JsonValueKind.Number && value.TryGetInt32(out var index))
        {
            // 旧协议的编号：0 = AppData、1 = ExeDirectory、2 = Custom（与枚举值顺序不同，别弄反）
            return index switch
            {
                0 => ConfigLocation.AppData,
                1 => ConfigLocation.ExeDirectory,
                2 => ConfigLocation.Custom,
                _ => null,
            };
        }

        if (value.ValueKind == JsonValueKind.String)
        {
            return ParseLocationText(value.GetString());
        }

        return null;
    }
}
