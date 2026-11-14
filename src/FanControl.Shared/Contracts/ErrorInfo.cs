namespace FanControl.Shared.Contracts;

/// <summary>
/// 结构化错误：<see cref="Code"/> 由界面翻译（见 src/FanControl.UI.Cpp/src/i18n.cpp 的 error 表），
/// <see cref="Arg1"/>/<see cref="Arg2"/> 是文案里的占位参数（范围上下限、端口名、Win32 错误码等），
/// <see cref="Message"/> 是中文兜底——写日志、老界面和未知 code 用得到。
/// </summary>
public sealed record ErrorInfo(string Code, string? Arg1 = null, string? Arg2 = null, string Message = "")
{
    public static ErrorInfo Of(string code, string message, string? arg1 = null, string? arg2 = null)
        => new(code, arg1, arg2, message);

    public static ErrorInfo Of(string code) => new(code);
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
