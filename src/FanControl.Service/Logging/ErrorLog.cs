using FanControl.Shared.Contracts;
using Microsoft.Extensions.Logging;

namespace FanControl.Service.Logging;

/// <summary>带级别标签的日志状态：文件日志用它决定方括号里写哪一级。</summary>
public interface ISeverityTag
{
    string SeverityTag { get; }
}

/// <summary>级别中文名：与界面上的四档一一对应（提示 / 异常 / 错误 / 验证错误）。</summary>
public static class LevelTags
{
    public static string Of(ErrorSeverity severity) => severity switch
    {
        ErrorSeverity.Notice => "提示",
        ErrorSeverity.Warning => "异常",
        ErrorSeverity.Validation => "验证错误",
        _ => "错误",
    };

    public static string Of(LogLevel level) => level switch
    {
        LogLevel.Trace or LogLevel.Debug => "调试",
        LogLevel.Information => "提示",
        LogLevel.Warning => "异常",
        LogLevel.Error => "错误",
        _ => "严重",
    };
}

/// <summary>
/// 把结构化错误按级别写进日志：提示→Information、异常/验证错误→Warning、错误→Error，
/// 方括号里的标签取 <see cref="LevelTags"/>，所以验证错误在日志里是 <c>[验证错误]</c> 而不是笼统的 Error。
/// </summary>
public static class ErrorLog
{
    public static void Write(ILogger logger, ErrorInfo info, Exception? exception = null, string? context = null)
    {
        var level = info.Severity switch
        {
            ErrorSeverity.Notice => LogLevel.Information,
            ErrorSeverity.Warning or ErrorSeverity.Validation => LogLevel.Warning,
            _ => LogLevel.Error,
        };

        logger.Log(level, default(EventId), new Entry(info, context), exception, static (entry, _) => entry.Text);
    }

    private sealed record Entry(ErrorInfo Info, string? Context) : ISeverityTag
    {
        public string SeverityTag => LevelTags.Of(Info.Severity);

        public string Text => Context is null
            ? $"{Info.Code}：{Info.Message}"
            : $"{Info.Code}：{Info.Message}（{Context}）";
    }
}
