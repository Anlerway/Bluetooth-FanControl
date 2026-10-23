using FanControl.Service.Fan;

namespace FanControl.Tests;

public class PwmSmootherTests
{
    [Fact]
    public void FirstSample_ReturnsTargetImmediately()
    {
        var smoother = new PwmSmoother();

        var result = smoother.Update(40, nowSeconds: 100, windowSeconds: 3);

        Assert.Equal(40, result);
    }

    [Fact]
    public void ConstantTarget_ReturnsSameValue()
    {
        var smoother = new PwmSmoother();

        for (var t = 0.0; t <= 10; t += 0.1)
        {
            Assert.Equal(40, smoother.Update(40, t, 3) ?? double.NaN, 6);
        }
    }

    [Fact]
    public void StepInput_OutputIsAverageOfWindow_NotInstantJump()
    {
        var smoother = new PwmSmoother();
        for (var t = 0.0; t < 10.0; t += 0.1)
        {
            smoother.Update(0, t, 3);
        }

        // 在 t=10 时刻目标从 0 跳到 100：窗口内只有最后一个瞬间是 100，
        // 输出应远小于 100（旧实现固定系数 EMA 会直接跳到 50）
        var firstAfterStep = smoother.Update(100, 10.0, 3) ?? double.NaN;

        Assert.True(firstAfterStep < 5, $"阶跃后首拍应接近旧值，实际 {firstAfterStep}");
    }

    [Fact]
    public void StepInput_ConvergesToNewTargetAfterOneWindow()
    {
        var smoother = new PwmSmoother();
        for (var t = 0.0; t < 10.0; t += 0.1)
        {
            smoother.Update(0, t, 3);
        }

        // 阶跃后再持续采样满一个窗口（3 秒），窗口内才全是新值
        double last = 0;
        for (var t = 10.0; t <= 13.0; t += 0.1)
        {
            last = smoother.Update(100, t, 3) ?? double.NaN;
        }

        Assert.Equal(100, last, 6);
    }

    [Fact]
    public void SameSmoothingDuration_IsIndependentOfSampleInterval()
    {
        var fast = new PwmSmoother();
        var slow = new PwmSmoother();

        // 两种采样率下都是 t=0 起为 0、t=10 起为 100，窗口 4 秒；
        // 只有采样密度不同，过渡时长应当一致
        for (var t = 0.0; t < 10.0; t += 0.01)
        {
            fast.Update(0, t, 4);
        }

        for (var t = 0.0; t < 10.0; t += 1.0)
        {
            slow.Update(0, t, 4);
        }

        for (var t = 10.0; t <= 12.0; t += 0.01)
        {
            fast.Update(100, t, 4);
        }

        for (var t = 10.0; t <= 12.0; t += 1.0)
        {
            slow.Update(100, t, 4);
        }

        // 阶跃后 2 秒：窗口 [8,12] 一半旧值一半新值，两者都应接近 50
        var fastMid = fast.Update(100, 12.0, 4) ?? double.NaN;
        var slowMid = slow.Update(100, 12.0, 4) ?? double.NaN;

        Assert.InRange(fastMid, 48, 52);
        Assert.InRange(slowMid, 48, 52);
    }

    [Fact]
    public void UnevenSamples_AreWeightedByHeldTime()
    {
        var smoother = new PwmSmoother();
        // t=0 记 0，之后长时间保持（到 t=9.9 才有下一个样本），t=10 记 100
        smoother.Update(0, 0, 10);
        smoother.Update(100, 10, 10);

        // 窗口 [0,10]：0 值保持 10 秒中的绝大部分，输出应接近 0 而不是两样本的算术平均 50
        var result = smoother.Update(100, 10.0, 10) ?? double.NaN;

        Assert.True(result < 1, $"应按保持时长加权，实际 {result}");
    }

    [Fact]
    public void InvalidTarget_KeepsLastOutputAndDoesNotPolluteWindow()
    {
        var smoother = new PwmSmoother();
        smoother.Update(40, 0, 3);
        smoother.Update(40, 1, 3);

        Assert.Equal(40, smoother.Update(null, 2, 3));
        Assert.Equal(40, smoother.Update(double.NaN, 3, 3));
        Assert.Equal(40, smoother.Update(40, 4, 3));
    }

    [Fact]
    public void ZeroWindow_IsPassthrough()
    {
        var smoother = new PwmSmoother();
        smoother.Update(0, 0, 3);

        Assert.Equal(80, smoother.Update(80, 1, 0));
        Assert.Equal(20, smoother.Update(20, 2, -1));
    }

    [Fact]
    public void ClockGoingBackwards_ResetsWindow()
    {
        var smoother = new PwmSmoother();
        smoother.Update(0, 100, 3);
        smoother.Update(0, 101, 3);

        // 时钟回退：不应算出负数时长或旧窗口的加权结果
        Assert.Equal(70, smoother.Update(70, 5, 3));
    }

    [Fact]
    public void Reset_ClearsHistory()
    {
        var smoother = new PwmSmoother();
        smoother.Update(0, 0, 3);
        smoother.Update(0, 1, 3);

        smoother.Reset();

        Assert.Null(smoother.LastOutput);
        Assert.Equal(0, smoother.SampleCount);
        Assert.Equal(90, smoother.Update(90, 2, 3));
    }

    [Fact]
    public void LongRun_BufferStaysBounded()
    {
        var smoother = new PwmSmoother();

        // 采样 100ms、窗口 3 秒，跑 10 分钟：样本数应稳定在窗口内
        for (var t = 0.0; t < 600; t += 0.1)
        {
            smoother.Update(50, t, 3);
        }

        Assert.InRange(smoother.SampleCount, 2, 40);
    }
}
