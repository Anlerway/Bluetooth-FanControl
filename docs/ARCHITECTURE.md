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

## 资源占用（本机实测）

测试机：Windows 11 / 2560×1600 @150% / 后台以 `dotnet FanControl.Host.dll` 运行（非管理员，CPU 温度读不到，其余传感器正常）。

| 进程 | 私有内存 | 工作集 | 线程 |
| --- | --- | --- | --- |
| `FanControl.Host.exe`（后台常驻） | 42 MB | 85 MB | 26–33 |
| `FanControl.UI.exe`（界面，实时收数据） | 26 MB | 43 MB | 24 |
| `FanControl.UI.exe`（界面，空闲） | 17 MB | 31 MB | 24 |
| 旧单进程 WPF 版（对照） | 82 MB | — | 24 |

界面渲染目标固定用 `D2D1_RENDER_TARGET_TYPE_SOFTWARE`：同一台机器上硬件 `HwndRenderTarget` 会把私有内存推到 65 MB、线程涨到 45 个（多出来的是显卡驱动线程），而界面只在收到状态帧或鼠标交互时重绘（约 2.5 次/秒），软件光栅的 CPU 代价约 4% 单核，更划算。

## 进程间协议

管道名 `FanControl.Host.v1`，协议版本 1，帧格式与字段见 [PROTOCOL.md](PROTOCOL.md)。

- 后台连上即推一份 `state`，随后每 400 ms 推一次；配置变更额外广播 `config`。
- 界面命令：`hello` / `getState` / `set`（按 key 改单个配置项）/ `setCurve` / `setRpmCurve` / `reconnect` / `showWindow`。
- 界面侧句柄以 `FILE_FLAG_OVERLAPPED` 打开，读写都带超时；**不能**在连接后立刻同步写——后台先写首帧，同步写会把界面线程堵在管道缓冲上。

## 本地化

- 语言是配置项（`AppConfig.Language`，`zh-CN` / `en-US`），存在后台的配置文件里；**后台是唯一写盘方**，界面只是发 `{"cmd":"set","key":"language"}`。
- 界面侧的字符串表在 `src/FanControl.UI.Cpp/src/i18n.cpp`：`kEntries[]` 以**中文原文为 key** 映射到英文，`fc::T(L"仪表盘")` 在英文模式下查表、中文模式下原样返回，漏翻只会显示中文，不会显示空串或 key。界面是立即模式：每次 `App::Paint()` 都按当前语言重新排版并重建命中区域，命中 id（如 `set:lang`、`chip:apply`）是固定字符串而不是显示文案，所以换语言后按钮不会失效。
- 界面启动后收到的第一帧 `config` 会同步语言（`App::ApplyConfig`），所以换语言后重启界面仍是同一种。
- 后台不拼用户文案，只给**错误码 + 参数**：`ErrorInfo(Code, Arg1, Arg2, Message)`（`src/FanControl.Shared/Contracts/ErrorInfo.cs`），provider 抛 `FanControlException(ErrorInfo)`；IPC 的 `error` 帧与 `state.errorCode` 带这三个字段，界面用 `fc::ErrorText(code, arg1, arg2, message)` 查 `kErrors[]` 拼当前语言，未登记的 code 退回 `message`。错误码表见 [PROTOCOL.md](PROTOCOL.md#错误码)。
- 托盘属于后台进程，它的菜单与气泡文案在 `src/FanControl.Service/Tray/TrayStrings.cs`，由 `TrayStrings.SetLanguage` 跟随配置；`FanControlRuntime.ConfigChanged` 在配置变更后触发，`Program` 据此刷新托盘文案。

## 设备通信

- 设备侧（ESP32）：BLE（Nordic UART Service）或串口，载荷都是一行 ASCII 文本，详见 [PROTOCOL.md](PROTOCOL.md)。
- BLE 走 Win32 `bluetoothleapis`（`BluetoothGATT*`）+ SetupAPI 枚举设备接口，**不依赖 WinRT 投影**；因此工程的 TFM 是 `net8.0-windows` 而不是 `net8.0-windows10.0.19041.0`。
- 托盘用 `Shell_NotifyIcon` + `TrackPopupMenuEx`（P/Invoke），**不依赖 WinForms**。
- 界面不直接碰设备与注册表：蓝牙设备列表、串口列表、显卡列表分别由后台的 `scanBle` / `listPorts` / `listGpus` 命令返回，开机自启由 `setAutostart` 写入计划任务（`schtasks /TN FanControl`）——这些都要管理员权限，只能待在后台进程。

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
