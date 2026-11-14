using System.Text.Json;
using FanControl.Service.Config;
using FanControl.Shared.Contracts;
using FanControl.Shared.Enums;
using FanControl.Shared.Models;

namespace FanControl.Host.Ipc;

/// <summary>
/// system.json 的运行期视图（日志开关、数据目录）。这些设置不属于 AppConfig，
/// 由守护进程独占写盘；界面通过 setSystem 命令下发，改动立即落盘。
/// 数据目录切换会把已有数据整体迁移到新位置，需要重启守护进程才完全生效。
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
                var location = ParseLocation(value);
                if (location is null)
                {
                    return ErrorInfo.Of("config.enum", "无法识别的取值。", key);
                }

                updated = Current with { UserDataLocation = location.Value };
                break;

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

    private static ConfigLocation? ParseLocation(JsonElement value)
    {
        if (value.ValueKind == JsonValueKind.Number && value.TryGetInt32(out var index))
        {
            return index switch
            {
                0 => ConfigLocation.AppData,
                1 => ConfigLocation.ExeDirectory,
                _ => null,
            };
        }

        if (value.ValueKind == JsonValueKind.String)
        {
            var text = value.GetString();
            if (string.Equals(text, nameof(ConfigLocation.ExeDirectory), StringComparison.OrdinalIgnoreCase))
            {
                return ConfigLocation.ExeDirectory;
            }

            if (string.Equals(text, nameof(ConfigLocation.AppData), StringComparison.OrdinalIgnoreCase))
            {
                return ConfigLocation.AppData;
            }
        }

        return null;
    }
}
