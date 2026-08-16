# 通信协议

## IPC（UI ↔ Service，NamedPipe `\\.\pipe\FanControl.ipc`）

- 帧格式（M3 实现）：4 字节小端长度前缀 + UTF-8 JSON 载荷。
- 请求信封：`IpcMessage { Command, RequestId, PayloadJson }`。
- 响应信封：`IpcResponse { RequestId, Success, Error, PayloadJson }`。
- 命令：Ping / GetConfig / SetConfig / SetMode / SetCurve / SetCommunicationType / GetSnapshot / Restart / Shutdown。
- JSON 统一使用 `FanControl.Shared.Contracts.JsonDefaults.Options`（camelCase、忽略 null）。
- `Restart` / `Shutdown` 会触发服务优雅停止；进程重启由外部托管（托盘/任务计划）负责。

## ESP32（Service ↔ 设备）

- 串口帧：一行 ASCII，格式 `PWM:<0-100>\r\n`，示例：`PWM:45.0`；
- BLE：载荷与串口相同（占位，M5 定义 GATT 服务/特征）；
- 后续可扩展 `FAN:RPM`、`PING` 等指令。
