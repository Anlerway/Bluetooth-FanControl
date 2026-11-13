# FanControl 架构

## 进程与工程

| 工程 | 作用 |
| --- | --- |
| `src/FanControl.Shared` | 共享契约：枚举、模型（AppConfig / DataPacket / SystemConfig）、IPC 信封与分帧、MAC 匹配工具 |
| `src/FanControl.Service` | 类库：采样与通信循环、硬件数据源（LibreHardwareMonitor / ATK ACPI / WMI / AIDA64 / 模拟）、BLE 与 COM 通道、配置、日志、原生托盘 |
| `src/FanControl.Host` | 后台守护进程（C#，`requireAdministrator`）：装配运行时、托盘、命名管道服务端、唤醒界面 |
| `src/FanControl.UI.Cpp` | 界面（C++20，Direct2D + DirectWrite 自绘，Win11 设置风格）：只做呈现与交互，通过管道读写后台 |
| `tests/FanControl.Tests` | xUnit 单元测试 |

前后台分离的两个进程：

```text
FanControl.Host.exe (管理员, 常驻)          FanControl.UI.exe (普通权限, 按需)
├─ FanControlRuntime                      ├─ App (D2D 立即模式 UI)
│   ├─ 采样循环 (100–5000 ms)  ──┐        ├─ IpcClient (overlapped 读写, 带超时)
│   ├─ 通信循环 (BLE / COM)      │        └─ 读线程 → PostMessage(WM_APP+1)
│   ├─ PwmSmoother               │
│   └─ AppState ─────────────────┴── 命名管道 FanControl.Host.v1 ──→ 状态推送 400 ms
├─ TrayHost (Shell_NotifyIcon, 主线程消息泵)
└─ IpcServer (每连接一线程)
```

## 进程边界与生命周期

- 后台单实例：互斥体 `Local\FanControl.Host`；第二次启动只 `Set` 事件 `Local\FanControl.ShowWindow` 后退出，由已在运行的实例唤出界面。
- 界面单实例：互斥体 `Local\FanControl.UI`；重复启动时 `FindWindowW(L"FanControlUI")` 把已有窗口激活。
- 界面找不到后台时，按自身目录拼 `FanControl.Host.exe` 并用 `ShellExecuteW(..., L"runas", ...)` 提权拉起，随后重试连接（8 × 250 ms）。
- 进程组：后台用 `CreateProcessW` 起 `FanControl.UI.exe --spawned` 并 `AssignProcessToJobObject`，job 带 `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE`，后台退出时界面一起结束，任务管理器里两者归为一行。
- 已知取舍：由管理员后台拉起的界面会继承管理员令牌。

## 状态归属

| 归后台（唯一真源） | 归界面（临时状态） |
| --- | --- |
| 温度 / 转速 / PWM / 原始目标 PWM / 连接状态 / LastError | 当前页、滚动位置、曲线拖拽中的草稿点 |
| `AppConfig`（唯一写盘方，界面只能发 `set` / `setCurve` 请求） | 主题与单位的即时渲染（跟随后台下发值） |
| 托盘图标、通知、开机自启 | 窗口大小、DPI、命中测试状态 |

判据：**界面关掉还要继续转的、需要管理员权限的，都归后台**。`FanController` 与 `PwmSmoother` 必须在后台，否则关窗口会把风扇停在最后一个 PWM。

## 线程模型

- 后台：托盘消息泵主线程、采样循环（`PeriodicTimer`）、通信循环、IPC 接受 + 每客户端读线程、日志 Channel 写线程；`WaitOne(500)` 线程监听「唤出界面」事件。
- 界面：主线程（消息循环 + D2D 绘制）+ IPC 读线程（只 `PostMessage`，不碰 UI 对象）。
- LibreHardwareMonitor 的 `Computer.Update()` 由 `LhmComputerHost.EnsureFresh()` 统一节流（800 ms），温度与转速共享同一份快照，避免一个采样周期打两遍 EC/SMBus。

## 进程间协议

管道名 `FanControl.Host.v1`，协议版本 1，帧格式与字段见 [PROTOCOL.md](PROTOCOL.md)。

- 后台连上即推一份 `state`，随后每 400 ms 推一次；配置变更额外广播 `config`。
- 界面命令：`hello` / `getState` / `set`（按 key 改单个配置项）/ `setCurve` / `setRpmCurve` / `reconnect` / `showWindow`。
- 界面侧句柄以 `FILE_FLAG_OVERLAPPED` 打开，读写都带超时；**不能**在连接后立刻同步写——后台先写首帧，同步写会把界面线程堵在管道缓冲上。

## 设备通信

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
与采样间隔解耦：采样 100 ms 或 5 s 时，同样的窗口给出相同的过渡时长。

- `SmoothingEnabled=false` 或手动模式：直接下发目标值，并清空窗口；
- 切换控制模式 / 温度源：清空窗口，避免旧通道的值把切换瞬间拖成缓慢爬行；
- 温度源无数据（null/NaN）：保持上次输出，不写入样本。

## 仓库边界

仓库只放源码、构建脚本、必要工程文档和测试：

- 进仓库：`*.cs` / `*.cpp` / `*.h` / `*.rc` / `*.csproj` / `CMakeLists.txt` / `build.bat` / `*.slnx` / `Directory.Build.props` / `.github/workflows` / `docs/` / `screenshots/` / `tests/`；
- 不进仓库：`bin/`、`obj/`、`build/`、`.vs/`、发布目录、zip 与安装包等中间产物。打包脚本默认输出到仓库外的 `..\..\artifacts\`。
