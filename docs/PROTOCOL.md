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

## 进程间（FanControl.UI.exe ↔ FanControl.Host.exe）

- 传输：命名管道 `\\.\pipe\FanControl.Host.v1`，单实例、双向字节流。
- 分帧：4 字节小端长度 + UTF-8 JSON（> 1 MB 视为错误），与 `FanControl.Shared/Contracts/PipeFraming.cs` 一致。
- 后台连上即推一帧 `state`，此后每 400 ms 一帧；配置改动额外广播 `config`。
- 界面侧必须以 `FILE_FLAG_OVERLAPPED` 打开句柄并给读写设超时：后台先写首帧，同步写会把界面线程堵在管道缓冲上。

命令（界面 → 后台）：

| cmd | 载荷 | 说明 |
| --- | --- | --- |
| `hello` | — | 握手，回 `{"event":"hello","protocol":1,"version":"1.4.0"}` |
| `getState` | — | 立即补一帧 `state` |
| `set` | `key`、`value` | 改单个配置项；成功回 `{"event":"ok","cmd":"set"}`，失败回 `{"event":"error","code":…,"arg1":…,"arg2":…,"message":…}` |
| `setCurve` / `setRpmCurve` | `points: [[温度, PWM], …]` | 至少 2 点，后台按 x 排序并 clamp 到 0–100 |
| `reconnect` | — | 强制重建 BLE / COM 连接 |
| `showWindow` | — | 唤出 / 激活界面窗口 |

事件（后台 → 界面）：`hello` / `state` / `config` / `ok` / `error`。

`state` 字段：`cpu`、`gpu`、`rpm`、`pwm`、`rawPwm`、`mode`、`connectionState`、`connected`、`communicationType`、`deviceName`、`lastError`、`errorCode`、`errorArg1`、`errorArg2`、`timestamp`（epoch ms）。

### 错误码

后台不产生面向用户的成品文案（除 `message` 兜底），而是给 `code` + 至多两个参数，界面按当前语言查表拼装：

```json
{"event":"error","code":"config.rangeInt","arg1":"100","arg2":"5000","message":"取值必须是 100-5000 之间的整数。"}
```

| 前缀 | code | 参数 |
| --- | --- | --- |
| `ipc.` | `badJson`、`unknownCommand`、`missingKey`、`missingValue`、`missingPoints`、`badPoint` | `unknownCommand` 的 arg1 是命令名 |
| `config.` | `unknownKey`、`curveTooFew`、`curveFlat`、`enum`、`notString`、`notBool`、`rangeInt`、`rangeNumber`、`applyFailed` | `unknownKey` arg1=键名；`enum` arg1=类型名；`range*` arg1=下限、arg2=上限；`applyFailed` arg1=异常信息 |
| `runtime.` | `samplingFailed` | arg1=异常信息（采样线程未归类的异常） |
| `ble.` | `notConfigured`、`deviceNotFound`、`deviceNotFoundByMac`、`nusServiceMissing`、`nusServiceMissingFor`、`nusCharacteristicMissing`、`notConnected`、`timeout`、`gattUnavailable`、`openFailed`、`writeFailed` | `deviceNotFound*` arg1=设备名/MAC；`timeout` arg1=秒；`openFailed` arg1=Win32 码；`writeFailed` arg1=HRESULT |
| `com.` | `openTimeout`、`noHandshake`、`notConnected` | arg1=COM 口 |
| `curve.` / `runtimeCurve.` | `empty`、`notIncreasing` | — |
| `autostart.` | `noProcessPath`、`taskFailed` | `taskFailed` arg1=schtasks 退出码 |
| `error.` | `generic` | 兜底 |

界面侧的文案表在 `src/FanControl.UI.Cpp/src/i18n.cpp`（`kErrors[]`，中英各一份，`{0}`/`{1}` 占位）；未登记的 code 退回 `message` 字段。

`config` 字段（camelCase）：`temperatureSource`、`gpuTemperatureSource`、`gpuSelection`、`fanSpeedSource`、`fanControlMode`、`communicationType`、`comPort`、`comBaudRate`、`bleDeviceMac`、`bleDeviceName`、`pollIntervalMilliseconds`、`blePollIntervalSeconds`、`bleReconnectIntervalSeconds`、`autoReconnectBle`、`pwmSmoothingWindowSeconds`、`notifyOnBleDisconnect`、`notifyOnTemperatureError`、`language`、`temperatureUnit`、`theme`、`manualPwmPercent`、`smoothingEnabled`、`curve`、`rpmCurve`。

`set` 的取值范围：`pollIntervalMilliseconds` 100–5000、`blePollIntervalSeconds` 1–30、`bleReconnectIntervalSeconds` 1–60、`pwmSmoothingWindowSeconds` 0–30、`manualPwmPercent` 0–100；枚举 key 的合法值见 `src/FanControl.Host/Ipc/ConfigPatch.cs`。

安全：仅本地命名管道，ACL 为默认（同机同用户可连），后台不监听任何网络端口。

> `FanControl.Shared/Contracts` 里的 `IpcMessage` / `IpcResponse` 是早期信封设计，分帧规则与本协议一致，但当前实车报文以上表为准（`cmd` / `event` 扁平结构）。
