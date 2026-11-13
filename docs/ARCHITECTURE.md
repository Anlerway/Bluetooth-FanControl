# FanControl 架构

## 进程与工程

| 工程 | 作用 |
| --- | --- |
| `FanControl.Shared` | 共享契约：枚举、模型、IPC 信封与分帧、MAC 匹配工具 |
| `FanControl.Service` | 类库：采样循环、硬件数据源（LibreHardwareMonitor / ATK ACPI）、通信（BLE / COM）、配置、日志、托盘 |
| `FanControl.UI` | WPF 主程序（MVVM，CommunityToolkit.Mvvm） |
| `tests/FanControl.Tests` | xUnit 单元测试 |

当前形态是**单进程**：`FanControl.UI/App.xaml.cs` 通过 `FanControlServices.BuildFanControlProvider` 装配运行时并在 UI 进程内 `StartAsync`，托盘是与主窗口同进程的 STA 线程。

## 通信

- 设备侧（ESP32）：BLE（Nordic UART Service）或串口，载荷都是一行 ASCII 文本，详见 [PROTOCOL.md](PROTOCOL.md)。
- BLE 走 Win32 `bluetoothleapis`（`BluetoothGATT*`）+ SetupAPI 枚举设备接口，**不依赖 WinRT 投影**；因此工程的 TFM 是 `net8.0-windows` 而不是 `net8.0-windows10.0.19041.0`。
- 托盘用 `Shell_NotifyIcon` + `TrackPopupMenuEx`（P/Invoke），**不依赖 WinForms**。

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

## 仓库边界

仓库只放源码、构建脚本、必要工程文档和测试：

- 进仓库：`*.cs` / `*.xaml` / `*.csproj` / `*.slnx` / `Directory.Build.props` / `.github/workflows` / `docs/` / `screenshots/` / `tests/`；
- 不进仓库：`bin/`、`obj/`、`.vs/`、发布目录、zip 与安装包等中间产物。打包脚本默认输出到仓库外的 `..\artifacts\`。
