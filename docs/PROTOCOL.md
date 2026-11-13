# 通信协议

## BLE（Service ↔ ESP32）

- 服务：Nordic UART Service `6e400001-b5a3-f393-e0a9-e50e24dcca9e`
  - RX（写入）`6e400002-b5a3-f393-e0a9-e50e24dcca9e`
  - TX（通知）`6e400003-b5a3-f393-e0a9-e50e24dcca9e`
- 载荷为一行 ASCII 文本（`\n` 结尾），与串口完全一致：
  - `PWM:<0-100>` —— 目标 PWM，示例 `PWM:45.0`；
  - `TIME:yyyy/MM/dd HH:mm:ss` —— 校时；
  - `CPU<度数>` / `GPU<度数>` —— 供 OLED 显示的温度。
- 当前实现只写入、**不消费 TX 通知**：订阅失败只记警告，不影响风扇控制。
- Windows 侧实现见 `FanControl.Service/Communication/BluetoothLeInterop.cs`（Win32 `bluetoothleapis`）。

## 串口（Service ↔ ESP32）

- 帧格式：一行 ASCII，`PWM:<0-100>\r\n`，示例 `PWM:45.0`；
- 其余指令与 BLE 相同。

## 进程间契约（保留，尚未接线）

`FanControl.Shared/Contracts/` 下的 `IpcMessage` / `IpcResponse` / `IpcCommandType` / `PipeFraming` 定义了 UI ↔ 后台进程的线路格式：

- 分帧：4 字节小端长度前缀 + UTF-8 JSON 载荷；
- 请求信封 `IpcMessage { Command, RequestId, PayloadJson }`，响应信封 `IpcResponse { RequestId, Success, Error, PayloadJson }`；
- JSON 统一走 `FanControl.Shared.Contracts.JsonDefaults.Options`（camelCase、忽略 null）。

目前没有管道服务端/客户端，这批契约留作将来进程分离时复用，已有序列化测试覆盖。
