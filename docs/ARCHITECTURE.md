# FanControl 架构

（骨架阶段占位，随实现逐步完善。）

- 前台：`FanControl.UI`（WinUI 3，MVVM，仅负责展示与交互，不执行硬件/通信逻辑）
- 后台：`FanControl.Service`（.NET 8，硬件监控与风扇控制核心，可作 Windows 服务或用户级任务）
- 托盘：`FanControl.Tray`（用户级独立进程，提供打开 UI / 启停服务 / 退出入口）
- 通信：NamedPipe（UI ↔ Service）、COM/BLE（Service ↔ ESP32）
- 配置：JSON（安装目录 / 用户数据两种模式）

详细计划见 [PLAN.md](../PLAN.md)。
