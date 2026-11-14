namespace FanControl.Shared.Contracts;

/// <summary>
/// 错误级别，由轻到重：提示 &lt; 异常 &lt; 错误 &lt; 验证错误。
/// 界面按级别上色并加前缀，日志按级别写 <c>[提示]/[异常]/[错误]/[验证错误]</c>。
/// </summary>
public enum ErrorSeverity
{
    /// <summary>提示：还没配好、暂时不用处理的情况（例如尚未选择蓝牙设备）。</summary>
    Notice = 0,

    /// <summary>异常：设备/通信链路的临时故障，后台会自己重试。</summary>
    Warning = 1,

    /// <summary>错误：需要用户处理或反馈的失败。</summary>
    Error = 2,

    /// <summary>验证错误：提交的配置或指令不合法，被拒绝且没有落盘。</summary>
    Validation = 3,
}

/// <summary>错误码 → 级别：前缀优先，个别码单独列出。</summary>
public static class ErrorSeverityMap
{
    public static ErrorSeverity For(string code) => code switch
    {
        // 落盘失败是真正的错误，不跟着 config.* 归到验证错误
        "config.applyFailed" => ErrorSeverity.Error,
        "ipc.unknownCommand" => ErrorSeverity.Error,
        // 还没选设备属于提示：用户可能只是想先看看界面
        "ble.notConfigured" => ErrorSeverity.Notice,
        "runtime.samplingFailed" or "comm.failed" => ErrorSeverity.Warning,
        _ when code.StartsWith("config.", StringComparison.Ordinal) => ErrorSeverity.Validation,
        _ when code.StartsWith("curve.", StringComparison.Ordinal) => ErrorSeverity.Validation,
        _ when code.StartsWith("runtimeCurve.", StringComparison.Ordinal) => ErrorSeverity.Validation,
        _ when code.StartsWith("ipc.", StringComparison.Ordinal) => ErrorSeverity.Validation,
        _ when code.StartsWith("ble.", StringComparison.Ordinal) => ErrorSeverity.Warning,
        _ when code.StartsWith("com.", StringComparison.Ordinal) => ErrorSeverity.Warning,
        _ => ErrorSeverity.Error,
    };
}

/// <summary>
/// 结构化错误：<see cref="Code"/> 由界面翻译（见 src/FanControl.UI.Cpp/src/i18n.cpp 的 error 表），
/// <see cref="Arg1"/>/<see cref="Arg2"/> 是文案里的占位参数（范围上下限、端口名、Win32 错误码等），
/// <see cref="Message"/> 是中文兜底——写日志、老界面和未知 code 用得到。
/// </summary>
public sealed record ErrorInfo(string Code, string? Arg1 = null, string? Arg2 = null, string Message = "")
{
    /// <summary>显式指定的级别；为空时按 <see cref="ErrorSeverityMap"/> 从错误码推。</summary>
    public ErrorSeverity? SeverityOverride { get; init; }

    /// <summary>生效的级别：决定界面颜色/前缀与日志标签。</summary>
    public ErrorSeverity Severity => SeverityOverride ?? ErrorSeverityMap.For(Code);

    public static ErrorInfo Of(string code, string message, string? arg1 = null, string? arg2 = null)
        => new(code, arg1, arg2, message);

    public static ErrorInfo Of(string code) => new(code);

    /// <summary>覆盖默认级别（个别调用点比错误码前缀更清楚时用）。</summary>
    public ErrorInfo WithSeverity(ErrorSeverity severity) => this with { SeverityOverride = severity };
}

/// <summary>带错误码的异常：后台把它写进 state.errorCode，界面按 code 翻译。</summary>
public sealed class FanControlException : Exception
{
    public ErrorInfo Error { get; }

    public FanControlException(ErrorInfo error) : base(error.Message)
    {
        Error = error;
    }

    public FanControlException(ErrorInfo error, Exception inner) : base(error.Message, inner)
    {
        Error = error;
    }
}
