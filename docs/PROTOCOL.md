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

## 界面 ↔ 宿主（进程内，`FanControl.UI.dll` ↔ `FanControl.exe`）

- 传输：**函数调用**，不是管道。界面导出 C ABI，宿主用 `UiBridge` 加载 `FanControl.UI.dll`：
  - 宿主 → 界面：`FcUiPush(const char* utf8Json)`，一个完整的 UTF-8 JSON 对象，不带长度前缀。
  - 界面 → 宿主：`FcUiSetCommandSink(callback)` 注册的回调，参数同样是完整 JSON 字符串。
- 宿主每 400 ms 组装一帧 `state`（含 `config`）：内容与上一帧相同（除 `timestamp`）就跳过，最多每 1 s 补一次心跳；有变化立即推。界面连上后宿主先补一帧，界面也会发一次 `hello`。
- 回调在**界面线程**上触发，宿主收到后立刻 `Task.Run` 转到线程池，不在界面线程里做配置写盘或设备 IO。
- 因为不再跨进程，1.4.0 的 4 字节长度前缀分帧已经没有意义（`FanControl.Shared/Contracts/PipeFraming.cs` 保留但不再使用）。

命令（界面 → 后台）：

| cmd | 载荷 | 说明 |
| --- | --- | --- |
| `hello` | — | 握手，回 `{"event":"hello","protocol":1,"version":"1.4.0"}` |
| `getState` | — | 立即补一帧 `state` |
| `set` | `key`、`value` | 改单个配置项；成功回 `{"event":"ok","cmd":"set"}`，失败回 `{"event":"error","code":…,"arg1":…,"arg2":…,"message":…}` |
| `setCurve` / `setRpmCurve` | `points: [[温度, PWM], …]` | 至少 2 点，后台按 x 排序并 clamp 到 0–100 |
| `reconnect` | — | 强制重建 BLE / COM 连接 |
| `uiClosed` | — | 界面窗口销毁时发；宿主等 600 ms 后 `SetProcessWorkingSetSize(-1, -1)` 收起工作集 |
| `checkUpdate` | — | 查 GitHub 最新 release，先回 `{"event":"update","state":"checking"}`，再回最终结果 |
| `scanBle` | — | 枚举已配对的 BLE 设备，回 `{"event":"bleDevices","devices":[{"name":…,"mac":…}]}` |
| `listPorts` | — | 枚举串口，回 `{"event":"ports","ports":["COM3",…]}` |
| `listGpus` | — | 枚举显卡名（WMI，失败回退 nvidia-smi），回 `{"event":"gpus","gpus":[…]}` |
| `setAutostart` | `value`（布尔） | 写入/删除计划任务 `FanControl`，成功回 `{"event":"ok","cmd":"setAutostart","autostart":true}` |
| `setSystem` | `key`、`value` | 改**系统级**配置（存在 system.json，不进 AppConfig）：`logEnabled`（布尔）、`userDataLocation`（`AppData` / `ExeDirectory`）、`maxLogFiles`（1–200）；成功先广播 `state` 再回 `{"event":"ok","cmd":"setSystem"}` |

事件（宿主 → 界面）：`hello` / `state` / `config` / `ok` / `error` / `bleDevices` / `ports` / `gpus` / `update`。

`update` 事件：`state` 取 `checking` / `current`（已是最新）/ `newer`（发现新版本）/ `none`（仓库暂无发布）/ `failed`；`tag` 是 release 的 tag，`url` 是 release 页面，`detail` 是失败原因（异常信息）。版本比较时程序集版本规约成三段再比，避免 `1.4.0.0` 被当成比 `1.4.0` 新。

`state` 字段：`cpu`、`gpu`、`rpm`、`pwm`、`rawPwm`、`mode`、`connectionState`、`connected`、`communicationType`、`deviceName`、`lastError`、`errorCode`、`errorArg1`、`errorArg2`、`commError`、`commErrorCode`、`commErrorArg1`、`commErrorArg2`、`autostart`、`logEnabled`、`userDataLocation`（`AppData` / `ExeDirectory`）、`timestamp`（epoch ms）。

- `lastError*` 是**采样线程**的异常（温度/转速读取失败）；
- `commError*` 是**通信线程**的异常（BLE/串口连不上、写入失败），链路恢复时清空；
- `autostart` 是计划任务的实际状态，界面不自行猜测（托盘里也能改，改完广播 `state`）；
- `userDataLocation` 改动后要**重启 FanControl** 才切根目录（宿主会把已有 `Config\` 与 `Logs\` 迁到新根），界面在这行副标题里写明「重启后生效」。

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
| `comm.` | `failed` | arg1=异常信息（通信线程未归类的异常） |
| `ble.` | `notConfigured`、`deviceNotFound`、`deviceNotFoundByMac`、`nusServiceMissing`、`nusServiceMissingFor`、`nusCharacteristicMissing`、`notConnected`、`timeout`、`gattUnavailable`、`openFailed`、`writeFailed` | `deviceNotFound*` arg1=设备名/MAC；`timeout` arg1=秒；`openFailed` arg1=Win32 码；`writeFailed` arg1=HRESULT |
| `com.` | `openTimeout`、`noHandshake`、`notConnected` | arg1=COM 口 |
| `curve.` / `runtimeCurve.` | `empty`、`notIncreasing` | — |
| `autostart.` | `noProcessPath`、`taskFailed` | `taskFailed` arg1=schtasks 退出码 |
| `error.` | `generic` | 兜底 |

界面侧的文案表在 `src/FanControl.UI.Cpp/src/i18n.cpp`（`kErrors[]`，中英各一份，`{0}`/`{1}` 占位）；未登记的 code 退回 `message` 字段。

`config` 字段（camelCase）：`temperatureSource`、`gpuTemperatureSource`、`gpuSelection`、`fanSpeedSource`、`fanControlMode`、`communicationType`、`comPort`、`comBaudRate`、`bleDeviceMac`、`bleDeviceName`、`pollIntervalMilliseconds`、`blePollIntervalSeconds`、`bleReconnectIntervalSeconds`、`autoReconnectBle`、`pwmSmoothingWindowSeconds`、`notifyOnBleDisconnect`、`notifyOnTemperatureError`、`language`、`temperatureUnit`、`theme`、`manualPwmPercent`、`smoothingEnabled`、`curve`、`rpmCurve`。

`set` 的取值范围：`pollIntervalMilliseconds` 100–5000、`blePollIntervalSeconds` 1–30、`bleReconnectIntervalSeconds` 1–60、`pwmSmoothingWindowSeconds` 0.5–30、`manualPwmPercent` 0–100；枚举 key 的合法值见 `src/FanControl.Host/Ui/ConfigPatch.cs`。

安全：界面与宿主是同一个进程（宿主以管理员身份运行），JSON 只在进程内传递；不监听任何网络端口，也没有管道 ACL 问题。

> `FanControl.Shared/Contracts` 里的 `IpcMessage` / `IpcResponse` / `PipeFraming` 是命名管道时期的信封与分帧实现，现已不再使用；当前报文以上表为准（`cmd` / `event` 扁平结构）。
