# FanControl 架构

## 进程与工程

| 工程 | 作用 |
| --- | --- |
| `src/FanControl.Shared` | 共享契约：枚举、模型（AppConfig / DataPacket / SystemConfig）、错误码 `ErrorInfo`、JSON 默认值、MAC 匹配工具 |
| `src/FanControl.Service` | 类库：采样与通信循环、硬件数据源（LibreHardwareMonitor / ATK ACPI / WMI / AIDA64 / 模拟）、BLE 与 COM 通道、配置、日志、原生托盘 |
| `src/FanControl.Host` | 单一进程宿主（C#，产物 `FanControl.exe`，`requireAdministrator`）：装配运行时、托盘、界面桥、界面命令路由 |
| `src/FanControl.UI.Cpp` | 界面（C++20，Direct2D + DirectWrite 自绘，Win11 设置风格）：编译成 `FanControl.UI.dll`，由宿主进程内加载 |
| `tests/FanControl.Tests` | xUnit 单元测试 |

只有一个进程，界面是它加载的 DLL：

```text
FanControl.exe (requireAdministrator, 常驻)
├─ FanControlRuntime   采样循环 100–5000 ms、通信循环 BLE/COM、PwmSmoother、AppState
├─ TrayHost            Shell_NotifyIcon + 主线程消息泵（关窗口后仍在此转）
├─ CommandRouter       解析界面命令、每 400 ms 生成 state/config JSON
├─ UiBridge            LoadLibrary FanControl.UI.dll，导出 FcUiRun/FcUiPush/FcUiSetCommandSink
└─ 界面线程 (STA)      └→ FanControl.UI.dll → Win32 窗口 + Direct2D 绘制
```

为什么不是「后台进程 + 界面进程」：后台必须留在 C#（LibreHardwareMonitor 只有 C# 版，跨主板传感器覆盖靠它），而让 C# 住进 C++ 进程要用 hostfxr 引导整个 .NET 运行时，成本远高于反向的 `LoadLibrary` —— 所以宿主是 C#，界面是 DLL。

## 进程边界与生命周期

- 单实例：互斥体 `Local\FanControl`。第二次启动只 `Set` 事件 `Local\FanControl.ShowWindow` 后退出，由已在运行的实例把窗口唤到前台。
- 窗口不等于进程：关掉窗口只是销毁窗口、退出界面线程，托盘与采样照常；托盘菜单「打开主界面」会重新 `FcUiRun()` 建出窗口。
- `--autostart`（计划任务用）只留托盘、不弹窗口；普通启动（安装器、快捷方式、开始菜单）直接显示窗口。
- 开机自启：计划任务 `FanControl`，动作 <安装目录>\FanControl.exe --autostart；`AutostartManager` 负责创建/删除与查询。
- 没有命名管道、没有启动器、没有 Job Object：进程组的需求随单一进程一起消失。

## 状态归属

| 归宿主（唯一真源） | 归界面（临时状态） |
| --- | --- |
| 温度 / 转速 / PWM / 原始目标 PWM / 连接状态 / LastError | 当前页、滚动位置、曲线拖拽中的草稿点 |
| `AppConfig`（唯一写盘方，界面只能发 `set` / `setCurve` 请求） | 主题与单位的即时渲染（跟宿主下发值） |
| 托盘图标、通知、开机自启 | 窗口尺寸、DPI、命中测试状态 |

判据：**关掉窗口还要继续的、需要管理员权限的，都归宿主**。`FanController` 与 `PwmSmoother` 在宿主里，所以关窗口不会把风扇停在最后一个 PWM。

## 线程模型

- 宿主主线程：`Main` 之后进入 `Task.Delay(Timeout.Infinite)` 等退出信号；托盘消息泵在 `TrayHost` 自己的线程上跑。
- 界面线程：`UiBridge.EnsureStarted()` 里新建的 STA 线程，跑 DLL 里的消息循环与 D2D 绘制；关窗口后线程退出，下次唤出再建。
- 采样循环（`PeriodicTimer`）、通信循环、日志 Channel 写线程、命令处理（`Task.Run`）：都在宿主侧。
- LibreHardwareMonitor 的 `Computer.Update()` 由 `LhmComputerHost.EnsureFresh()` 统一节流（800 ms），温度与转速共享同一份快照，避免一个采样周期打两遍 EC/SMBus。
- `LhmComputerHost.Configure(...)` 按配置只开用得上的模块（CPU / GPU / 主板·EC），启动与每次 `ApplyConfigAsync` 都会调一次；模块只增不减，重建延迟到下一次 `Get()`，避免在别的线程正遍历硬件时 `Close()`。

## 资源占用（本机实测）

测试机：Windows 11 / 2560×1600 `150%，宿主以 `dotnet FanControl.dll` 运行（沙箱非管理员，CPU 温度读不到，其余传感器正常），温度源临时设为「模拟数据」以便出图。

| 形态 | 私有内存 | 工作集 | 线程 |
| --- | --- | --- | --- |
| 单一进程 `FanControl.exe`（界面打开、实时收数据） | 35 MB | 77 MB | 42–43 |
| 同一进程，关掉界面只剩托盘 | 17–20 MB | 18–20 MB | 41–42 |
| 拆分版对照：后台 `FanControl.Host.exe` | 42 MB | 85 MB | 26–33 |
| 拆分版对照：界面 `FanControl.UI.exe` | 26 MB | 43 MB | 24 |
| 1.3.0 单进程 WPF 版（对照） | 82 MB | — | 24 |

关掉窗口后进程仍驻留托盘，所以退到托盘时会主动收一次：界面的 `WM_DESTROY` 发 `{"cmd":"uiClosed"}`，宿主等 600 ms 再 `SetProcessWorkingSetSize(-1, -1)`（`Ui/MemoryTrim.cs`），实测工作集从 104 MB 掉到 18 MB。

LibreHardwareMonitor 的模块是内存大头：全开（CPU+GPU+主板+控制器）时工作集 119 MB、私有 65 MB；只开 CPU 时 77 / 35 MB。其中 GPU 模块要加载 NVAPI/ADL，约 +37 MB，主板·EC 约 +8 MB，因此现在按配置按需开。

界面渲染目标固定用 `D2D1_RENDER_TARGET_TYPE_SOFTWARE`：同一台机器上硬件 `HwndRenderTarget` 会把私有内存推到 65 MB、线程涨到 45 个（多出来的是显卡驱动线程），而界面只在收到状态帧或鼠标交互时重绘（约 2.5 次/秒），软件光栅的 CPU 代价约 4% 单核。

## 硬件数据源

温度与转速各有一个策略接口（`ITemperatureProvider` / `IFanSpeedProvider`），`TemperatureProviderFactory` / `FanSpeedProviderFactory` 按配置的枚举值从容器里取单例，界面只发枚举名。

| 枚举值 | 温度 | 转速 | 实现要点 |
| --- | --- | --- | --- |
| `AtkAcpi` | ✓ | ✓ | 华硕 `\\.\ATKACPI` + `DeviceIoControl`（同 G-Helper） |
| `Wmi` | ✓ | ✓ | `MSAcpi_ThermalZoneTemperature` / `Win32_Fan` |
| `LibreHardwareMonitor` | ✓ | ✓ | 单例 `Computer`，按配置只开需要的模块（见上节内存说明） |
| `Aida64` | ✓ | ✓ | AIDA64 注册表 `SensorValues` |
| `NvidiaSmiAdl` | 仅 GPU | — | GPU 专用：nvidia-smi → ADL |
| `LenovoWmi` | ✓ | ✓ | 联想专有 WMI，见下 |
| `Simulated` | ✓ | ✓ | 演示数据 |

转速源与温度源同名时直接复用快照里的 `FanRpm`，否则另调转速 Provider。这里按**枚举名**比较而不是数值：`LenovoWmi` 在温度枚举里是 5、在转速枚举里是 4（温度枚举的 4 被 `NvidiaSmiAdl` 占着），按数值比较会把「联想温度源 + 联想转速源」误判成两个不同的源。

### 联想机型（Lenovo WMI）

参照 [Lenovo Legion Toolkit](https://github.com/LenovoLegionToolkit-Team/LenovoLegionToolkit) 的 WMI 调用面实现，**只读**（温度 + 转速），不写风扇曲线。代码在 `src/FanControl.Service/Hardware/Lenovo/`：

- `LenovoWmiBackend`（实现 `ILenovoWmiBackend`，方便单测替换）：
  - `LENOVO_FAN_TABLE_DATA`：只读表数据类，每行给出 `Fan_Id` / `Sensor_ID`、`FanTable_Data`（档位 → RPM）与 `SensorTable_Data`（档位 → 温度阈值，127 表示该档无效）；
  - `LENOVO_FAN_METHOD`：`Fan_GetCurrentFanSpeed(FanID)`、`Fan_GetCurrentSensorTemperature(SensorID)`；
  - `LENOVO_GAMEZONE_DATA`：`GetCPUTemperature` / `GetGPUTemperature`，作为没有传感器表时的兜底。
- 传感器与风扇编号从表里发现：`Sensor_ID` 最小的当 CPU、次小的当 GPU，`Fan_Id` 升序各读一遍取最大转速；`FanTable_Data` 的下标就是档位。
- 固件返回的风扇原始值单位随机型不同：`0..10` 按档位表查 RPM，`0..100` 按 `CurrentFanMaxSpeed` 折算百分比，`>100` 直接当 RPM，都对不上时返回 null。
- 非联想机型上 `LENOVO_*` 类枚举不到任何实例（判厂商走 `Win32_ComputerSystem.Manufacturer`），`IsAvailable` 为 false，Provider 直接返回空值并只记一条警告。
- 不写风扇曲线：LLT 自己的曲线控件也只剩历史代码，而 [LenovoLegionLinux 的已知问题](https://github.com/johnfanv2/LenovoLegionLinux/blob/main/docs/zh-hans/known-issues.md) 记录了不同代际的档位 / 百分比单位差异，写错会让部分机型直接断电。风扇输出的唯一通道仍然是蓝牙 BLE / 串口。

## 图标

- 唯一源文件 `src/FanControl.UI.Cpp/app.ico`（16 / 24 / 32 / 48 / 64 / 128 / 256 七档，256 档是 PNG 压缩）。
- 界面：`resource.rc` 把图标按**数字 101** 编进 DLL，注册窗口类时用 `LoadImageW(module, MAKEINTRESOURCEW(101), IMAGE_ICON, 0, 0, LR_DEFAULTSIZE | LR_SHARED)` 取；`wc.hIconSm` 另外取 16x16，标题栏与任务栏缩略图不会拿 256 档硬缩。注意不能写成 `LoadIconW(module, L"IDI_ICON1")`——那是按字符串名查资源，数字 ID 查不到，会静默退回系统默认图标。
- 宿主：`FanControl.Host.csproj` 的 `<ApplicationIcon>` 指向同一个 ico，资源管理器 / 任务栏 / 开始菜单用它。
- 托盘：`TrayHost` 用 `ExtractIconExW(Environment.ProcessPath, ...)` 取 exe 内嵌图标，取不到才回退 `IDI_APPLICATION`。
- 安装包：`installer.iss` 的 `SetupIconFile` 也是这个 ico，快捷方式与卸载列表沿用 exe 图标。

## 界面协议（进程内）

没有管道了，同一份 JSON 直接走函数调用：

- 宿主 → 界面：`CommandRouter` 每 400 ms 序列化一帧 `{"event":"state","config":{…},"state":{…}}`，`UiBridge.Push` 调 `FcUiPush(const char* utf8)`；界面在窗口线程解析后重绘。
- 界面 → 宿主：界面把命令序列化成 `{"cmd":"set","key":"theme","value":"\"Dark\""}` 这样的字符串，通过 `FcUiSetCommandSink` 注册的回调交给 `CommandRouter.HandleAsync`（回调在界面线程，宿主立刻转到线程池）。
- 命令与帧格式与 1.4.0 的命名管道完全一致，字段见 [PROTOCOL.md](PROTOCOL.md)；区别只是传输层从管道换成了一次函数调用。
- 可用命令：`hello` / `getState` / `set` / `setSystem` / `setCurve` / `setRpmCurve` / `reconnect` / `scanBle` / `listPorts` / `listGpus` / `setAutostart`。
## 本地化

- 语言是配置项（`AppConfig.Language`，`zh-CN` / `en-US`），存在后台的配置文件里；**后台是唯一写盘方**，界面只是发 `{"cmd":"set","key":"language"}`。
- 界面侧的字符串表在 `src/FanControl.UI.Cpp/src/i18n.cpp`：`kEntries[]` 以**中文原文为 key** 映射到英文，`fc::T(L"仪表盘")` 在英文模式下查表、中文模式下原样返回，漏翻只会显示中文，不会显示空串或 key。界面是立即模式：每次 `App::Paint()` 都按当前语言重新排版并重建命中区域，命中 id（如 `set:lang`、`chip:apply`）是固定字符串而不是显示文案，所以换语言后按钮不会失效。
- 界面启动后收到的第一帧 `config` 会同步语言（`App::ApplyConfig`），所以换语言后重启界面仍是同一种。
- 后台不拼用户文案，只给**错误码 + 参数**：`ErrorInfo(Code, Arg1, Arg2, Message)`（`src/FanControl.Shared/Contracts/ErrorInfo.cs`），provider 抛 `FanControlException(ErrorInfo)`；`error` 帧与 `state.errorCode` 带这三个字段，界面用 `fc::ErrorText(code, arg1, arg2, message)` 查 `kErrors[]` 拼当前语言，未登记的 code 退回 `message`。错误码表见 [PROTOCOL.md](PROTOCOL.md#错误码)。
- 托盘属于宿主进程，它的菜单与气泡文案在 `src/FanControl.Service/Tray/TrayStrings.cs`，由 `TrayStrings.SetLanguage` 跟随配置；`FanControlRuntime.ConfigChanged` 在配置变更后触发，`Program` 据此刷新托盘文案。

## 设备通信

- 设备侧（ESP32）：BLE（Nordic UART Service）或串口，载荷都是一行 ASCII 文本，详见 [PROTOCOL.md](PROTOCOL.md)。
- BLE 走 Win32 `bluetoothleapis`（`BluetoothGATT*`）+ SetupAPI 枚举设备接口，**不依赖 WinRT 投影**；因此工程的 TFM 是 `net8.0-windows` 而不是 `net8.0-windows10.0.19041.0`。
- 托盘用 `Shell_NotifyIcon` + `TrackPopupMenuEx`（P/Invoke），**不依赖 WinForms**。
- 界面不直接碰设备与注册表：蓝牙设备列表、串口列表、显卡列表分别由宿主的 `scanBle` / `listPorts` / `listGpus` 命令返回，开机自启由 `setAutostart` 写入计划任务（`schtasks /TN FanControl`）——这些都要管理员权限，只能待在宿主进程。

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