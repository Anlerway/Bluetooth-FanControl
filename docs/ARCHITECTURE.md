# FanControl 架构

（骨架阶段占位，随实现逐步完善。）

- 前台：`FanControl.UI`（WinUI 3，MVVM，仅负责展示与交互，不执行硬件/通信逻辑）
- 后台：`FanControl.Service`（.NET 8，硬件监控与风扇控制核心，可作 Windows 服务或用户级任务）
- 托盘：`FanControl.Tray`（用户级独立进程，提供打开 UI / 启停服务 / 退出入口）
- 通信：NamedPipe（UI ↔ Service）、COM/BLE（Service ↔ ESP32）
- 配置：JSON（安装目录 / 用户数据两种模式）

## PWM 平滑

采样链路对「曲线查表得到的目标 PWM」做平滑，不是对 RPM 滤波（固件无斜坡）：

```
温度采样 → 控制模式查曲线（CurveInterpolator）→ PwmSmoother → BLE/COM 下发
```

`FanControl.Service/Fan/PwmSmoother.cs` 记录最近 `PwmSmoothingWindowSeconds` 秒内的目标 PWM，
按每个样本的**保持时长**加权求平均（零阶保持的滑动平均）。因此平滑强度只由窗口秒数决定，
与采样间隔解耦：采样 100ms 或 5s 时，同样的窗口给出相同的过渡时长。

- `SmoothingEnabled=false` 或手动模式：直接下发目标值，并清空窗口；
- 切换控制模式 / 温度源：清空窗口，避免旧通道的值把切换瞬间拖成缓慢爬行；
- 温度源无数据（null/NaN）：保持上次输出，不写入样本。

详细计划见 [PLAN.md](../PLAN.md)。
