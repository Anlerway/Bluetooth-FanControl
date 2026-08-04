# FanControl

> 笔记本风扇控制一体方案：Windows 上位机（单进程单 exe）+ ESP32 固件（OLED 实时显示）。
> 支持多温度数据源、CPU/GPU 独立取数、按真实显卡名选 GPU、BLE/COM 双链路与 PWM 温控曲线。

![platform](https://img.shields.io/badge/Windows-10%2B-blue) ![.NET](https://img.shields.io/badge/.NET-8-green) ![ESP32](https://img.shields.io/badge/ESP32-SSD1306-orange)

**项目地址**：[anlerways/Bluetooth-FanControl](https://github.com/anlerways/Bluetooth-FanControl)
**固件仓库**：[anlerways/ESP32-LaptopFan](https://github.com/anlerways/ESP32-LaptopFan)

## 简介

FanControl 是一套用于笔记本的**风扇控制 + 状态显示**方案，由两部分组成：

- **上位机**（本仓库主体）：WinUI 3 桌面程序，单进程单 exe（UI + 托盘 + 后台监控循环），无需安装服务、以用户权限运行；
- **固件**：ESP32 + SSD1306 128×64 OLED 小屏，通过 BLE / USB 串口 / 经典蓝牙接收上位机数据，实时显示温度、模式与转速。

## 功能特性

### 上位机

- **多温度数据源**：LibreHardwareMonitor（默认）/ 华硕 ATKACPI（G-Helper 同款 DSTS）/ WMI / AIDA64 / 模拟数据；
- **CPU / GPU 获取方式独立选择**，GPU 还可按**真实显卡名称**（如 “NVIDIA GeForce RTX 4060 Laptop GPU”）指定读哪张卡，多卡取温不再只能取最高；
- **5 种控制模式**：手动 / CPU 温度 / GPU 温度 / 混合（取最高）/ 混合平均 / 目标转速（转速-PWM 曲线），温度曲线与转速曲线均可编辑；
- **PWM 平滑**：抑制温度波动导致的转速抖动，平滑系数可调；
- **双通信链路**：BLE（Nordic UART 服务）与 COM 串口，支持自动重连、固件校时（TIME 指令）、断开通知；
- **仪表盘**：实时温度 / PWM / 转速卡片 + 自绘趋势曲线（CPU/GPU/目标 PWM）；
- **托盘常驻**：关闭主窗口最小化到托盘，后台监控继续；支持开机自启（任务计划）、托盘温度预览、断开/错误通知；
- **配置自动保存**：设置页改动 400ms 防抖落盘，即时生效；
- **日志**：文件日志（安装目录或用户数据目录可选），便于排查。

### 固件（ESP32 + OLED）

- 主界面：状态栏（日期时间 + 蓝牙状态）+ 2×2 四宫格（CPU / GPU / MODE / FAN），22×22 图标 + 7×14 自定义字模，无分割线布局；
- 按键交互：菜单 / 自动 / 手动 / 转速 ±，长按休眠（关断 OLED 面板 + 风扇归零省电）；
- 10 分钟校时节流：上位机频繁下发 TIME 时只按分钟级间隔校准，避免秒位乱跳；
- 本地时钟按锚点 + millis 派生，校时相位不影响显示节奏；
- 蓝牙模式可切换（BLE / 经典蓝牙 SPP），无数据超时自动重新可见。

## 架构

单进程设计：UI（WinUI 3）与后台监控循环同进程运行，托盘为独立 STA 线程，退出时统一释放资源。

```text
FanControl.slnx
├── FanControl.Shared/    # 共享契约：Enums / Models（配置、数据包）
├── FanControl.Service/   # 类库：采样循环、硬件数据源、通信、配置、托盘
├── FanControl.UI/        # WinUI 3 主程序（输出 FanControl.exe）
├── FanControl.Installer/ # Inno Setup 脚本
├── firmware/LAPTOP_FAN/  # ESP32 固件（Arduino）
├── tests/FanControl.Tests
└── docs/                 # 架构与通信协议文档
```

采样链路：`温度数据源（CPU/GPU 独立）→ 控制模式查曲线 → PWM 平滑 → BLE/COM 下发 → OLED 显示`。

## 构建

前置：.NET 8 SDK（8.0.423+）、Visual Studio 2026（WinUI 3 工作负载）。

```powershell
# 类库与测试
dotnet build FanControl.Shared
dotnet build FanControl.Service
dotnet test tests/FanControl.Tests

# 主程序（WinUI 3，非打包模式，x64；需用 VS2026 MSBuild）
& "D:\APP\Visual Studio2026\MSBuild\Current\Bin\MSBuild.exe" FanControl.UI\FanControl.UI.csproj /restore /p:Configuration=Debug /p:Platform=x64

# 发布为自包含单进程 exe（免装 Windows App Runtime / .NET）
& "D:\APP\Visual Studio2026\MSBuild\Current\Bin\MSBuild.exe" FanControl.UI\FanControl.UI.csproj /restore /t:Publish /p:Configuration=Release /p:Platform=x64 /p:RuntimeIdentifier=win-x64 /p:WindowsAppSDKSelfContained=true /p:PublishDir=D:\PROJECT\CODEX\FAN\artifacts\exe\FanControl
```

## 运行

直接运行 [artifacts\exe\FanControl\FanControl.exe](artifacts/exe/FanControl/FanControl.exe)（整个目录一起拷贝）：

- 主窗口：仪表盘（实时温度/趋势）、设置、曲线编辑器；
- 托盘：打开主界面 / 开机自启（任务计划，G-Helper 风格）/ 退出；
- 关闭主窗口 = 最小化到托盘，后台监控继续；托盘“退出”才会停止监控并释放资源。

> Service 为纯类库（无独立进程），以用户级权限运行，不需要管理员。

## 数据源说明

| 数据源 | 说明 |
| --- | --- |
| LibreHardwareMonitor | 默认，真实硬件传感器（CPU Package/Tctl/Tdie、GPU 温度、风扇转速） |
| ATKACPI | 华硕机型专用（G-Helper 同款 DSTS 端点），CPU 温度 + 风扇转速 |
| WMI | MSAcpi_ThermalZoneTemperature 热区温度，GPU 走 nvidia-smi / AMD ADL |
| AIDA64 | 注册表 SensorValues（需 AIDA64 开启“允许监控数据写入注册表”） |
| 模拟 | 正弦波动数据，用于无硬件调试 / 演示 |

## 通信协议

一行 ASCII 文本帧，`\r\n` 结尾：

- 上位机 → 固件：`PWM:<0-100>`、`TIME:yyyy/MM/dd HH:mm:ss`、`TEMP:<cpu>[,<gpu>]`；
- 固件 → 上位机：`READY`（连接握手）、`STATUS:...`（查询应答）。

详见 [docs/PROTOCOL.md](docs/PROTOCOL.md)。

## 固件烧录

`firmware/LAPTOP_FAN/` 为 Arduino 工程（ESP32，需 Adafruit SSD1306 / GFX 库），支持板载按键与 OLED 显示；可烧录后与上位机通过 BLE 或串口配对使用。

## 文档

- [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)：模块与数据流
- [docs/PROTOCOL.md](docs/PROTOCOL.md)：通信协议
- [PLAN.md](PLAN.md)：开发计划与里程碑

## 参考与开源硬件

| 项目 | 说明 | 链接 |
| --- | --- | --- |
| G-Helper | 华硕设备控制参考 | <https://github.com/seerge/g-helper> |
| 适用的开源硬件及参考 | 开源硬件方案讲解（B 站视频） | <https://www.bilibili.com/video/BV1Lr421M7u2/> |
| LibreHardwareMonitor | 硬件监控库 | <https://github.com/LibreHardwareMonitor/LibreHardwareMonitor> |

## 免责声明

风扇控制涉及硬件散热，请谨慎调节转速曲线；固件/上位机按现状提供，使用风险自负。
