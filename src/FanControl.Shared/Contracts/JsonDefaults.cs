using System.Text.Json;
using System.Text.Json.Serialization;

namespace FanControl.Shared.Contracts;

/// <summary>前后台统一使用的 JSON 序列化选项（camelCase、忽略 null）。</summary>
public static class JsonDefaults
{
    /// <summary>
    /// 枚举按名字读写：写出来是 "LibreHardwareMonitor" 而不是 2，手改配置和排错都看得懂；
    /// 读的时候同时接受名字（不区分大小写）与旧配置里的数字，1.3.0 的 appconfig.json 直接可用。
    /// </summary>
    public static readonly JsonSerializerOptions Options = new(JsonSerializerDefaults.Web)
    {
        DefaultIgnoreCondition = JsonIgnoreCondition.WhenWritingNull,
        Converters = { new JsonStringEnumConverter() },
    };
}
