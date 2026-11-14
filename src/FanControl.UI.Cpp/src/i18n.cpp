#include "i18n.h"

#include <string>
#include <unordered_map>

namespace fc {
namespace {

struct Entry {
    const wchar_t* zh;
    const wchar_t* en;
};

const Entry kEntries[] = {
    { L"仪表盘", L"Dashboard" },
    { L"设置", L"Settings" },
    { L"曲线", L"Curve" },
    { L"关于", L"About" },
    { L"搜索设置", L"Search settings" },
    { L"服务运行中", L"Service running" },
    { L"演示数据（服务未就绪）", L"Demo data (service not ready)" },
    { L"深色", L"Dark" }, { L"跟随系统", L"System" },
    { L"浅色", L"Light" },
    { L"版本 1.4.0 · %s", L"Version 1.4.0 · %s" },
    { L"CPU 温度", L"CPU temperature" },
    { L"GPU 温度", L"GPU temperature" },
    { L"风扇 PWM", L"Fan PWM" },
    { L"LHM 封装传感器", L"LHM WMI sensors" },
    { L"未启用时回退 CPU", L"Falls back to CPU" },
    { L"转速见趋势", L"RPM in trend" },
    { L"最近 4 分钟", L"Last 4 minutes" },
    { L"连接状态", L"Connection" },
    { L"通信", L"Link" },
    { L"设备", L"Device" },
    { L"最近错误", L"Last error" },
    { L"已连接", L"Connected" },
    { L"未连接", L"Disconnected" },
    { L"无", L"None" },
    { L"通用", L"General" },
    { L"硬件", L"Hardware" },
    { L"控制", L"Control" },
    { L"维护", L"Maintenance" },
    { L"应用主题", L"Theme" },
    { L"跟随系统 / 深色 / 浅色", L"System / Dark / Light" },
    { L"语言", L"Language" },
    { L"界面语言", L"Interface language" },
    { L"简体中文", L"Simplified Chinese" },
    { L"温度单位", L"Temperature unit" },
    { L"仪表盘与曲线显示单位", L"Unit used on dashboard and curve" },
    { L"开机自启", L"Run at startup" },
    { L"登录后自动启动 FanControl", L"Start FanControl at sign-in" },
    { L"温度来源", L"Temperature source" },
    { L"转速来源", L"Fan speed source" },
    { L"通信方式", L"Connection type" },
    { L"蓝牙 BLE 或串口", L"Bluetooth LE or serial port" },
    { L"串口", L"Serial" },
    { L"蓝牙", L"Bluetooth" },
    { L"蓝牙 BLE", L"Bluetooth LE" },
    { L"平滑窗口", L"Smoothing window" },
    { L"PWM 变化的过渡时间", L"Ramp time for PWM changes" },
    { L"断连通知", L"Disconnect notification" },
    { L"蓝牙断开时弹出系统通知", L"Notify when the device disconnects" },
    { L"日志目录", L"Log folder" },
    { L"打开日志所在文件夹", L"Open the folder that holds log files" },
    { L"温度 → PWM 曲线", L"Temperature → PWM curve" },
    { L"温度 °C", L"Temperature °C" },
    { L"重置曲线", L"Reset curve" },
    { L"应用", L"Apply" },
    { L"拖动圆点调整映射，双击空白处新增点", L"Drag a point to change the mapping" },
    { L"版本", L"Version" },
    { L"界面", L"UI" },
    { L"运行方式", L"Runtime" },
    { L"配置文件", L"Config file" },
    { L"打开日志目录", L"Open log folder" },
    { L"检查更新", L"Check for updates" },
    { L"界面与采样在同一进程", L"UI and sampling share one process" },
    { L"未连接（演示数据）", L"Not connected (demo data)" },
    { L"1.4.0（C++ UI）", L"1.4.0 (C++ UI)" },
    { L"实时温度、转速与趋势", L"Live temperatures, RPM and trends" },
    { L"外观、硬件来源与控制行为", L"Appearance, hardware sources and control" },
    { L"温度到风扇转速的映射", L"Mapping from temperature to fan speed" },
    { L"版本与运行信息", L"Version and runtime info" },
    { L"未配对设备", L"No device paired" },
    { L"开机自启由安装器写入（待接线）", L"Startup entry is written by the installer (not wired yet)" },
    { L"已关闭开机自启（待接线）", L"Startup disabled (not wired yet)" },
    { L"命令已发送：", L"Command sent: " },
    { L"当前已是最新版本", L"You are on the latest version" },
    { L"模拟数据", L"Simulated" },
    { L"EC 直读", L"EC direct" },
    { L"摄氏度 °C", L"Celsius °C" },
    { L"华氏度 °F", L"Fahrenheit °F" },
    { L"English", L"English" },
    { L"GPU 温度源", L"GPU temperature source" },
    { L"GPU 选择", L"GPU selection" },
    { L"多显卡时指定读取哪一块", L"Pick which GPU to read on multi-GPU systems" },
    { L"自动", L"Auto" },
    { L"控制模式", L"Control mode" },
    { L"温度曲线 / 手动 PWM / 目标转速", L"Temperature curve / Manual PWM / Target RPM" },
    { L"手动 PWM", L"Manual PWM" },
    { L"手动模式下直接指定占空比", L"Fixed duty cycle in manual mode" },
    { L"温度异常通知", L"Temperature error notification" },
    { L"自动重连", L"Auto reconnect" },
    { L"断开后按间隔自动重试", L"Retry at the interval below when disconnected" },
    { L"重连间隔", L"Reconnect interval" },
    { L"断开后每隔多少秒重试一次", L"Seconds between reconnect attempts" },
    { L"温度数据源读取失败时提示", L"Notify when a temperature source fails" },
    { L"蓝牙设备", L"Bluetooth device" },
    { L"选择已配对的 NUS 风扇", L"Pick a paired NUS fan" },
    { L"串口端口", L"Serial port" },
    { L"选择与风扇相连的 COM 口", L"The COM port the fan is attached to" },
    { L"波特率", L"Baud rate" },
    { L"串口通信速率", L"Serial link speed" },
    { L"未选择设备", L"No device selected" },
    { L"温度曲线", L"Temperature curve" },
    { L"转速曲线", L"RPM curve" },
    { L"温度 °C → PWM %", L"Temperature °C → PWM %" },
    { L"转速 RPM → PWM %", L"RPM → PWM %" },
    { L"转速 RPM", L"RPM" },
    { L"打开发布页", L"Open releases" },
    { L"选择蓝牙设备", L"Select a Bluetooth device" },
    { L"选择串口", L"Select a serial port" },
    { L"选择 GPU", L"Select a GPU" },
    { L"扫描中…", L"Scanning…" },
    { L"没有可选项", L"Nothing to choose" },
    { L"温度与风扇", L"Temperature & fans" },
    { L"CPU 温度的读取方式", L"How the CPU temperature is read" },
    { L"显卡温度的读取方式", L"How the GPU temperature is read" },
    { L"风扇转速的读取方式", L"How the fan speed is read" },
    { L"决定 PWM 由谁计算", L"Who computes the PWM value" },
    { L"平滑输出", L"Smooth output" },
    { L"让 PWM 变化有过渡", L"Ease PWM changes over time" },
    { L"与风扇相连的 COM 口", L"COM port the fan is connected to" },
    { L"已配对的 NUS 风扇", L"Paired NUS fan" },
    { L"BLE 轮询", L"BLE polling" },
    { L"发送温度命令的间隔", L"Interval between temperature commands" },
    { L"断开后自动尝试恢复", L"Reconnect automatically after a drop" },
    { L"设备链路", L"Device link" },
    { L"系统", L"System" },
    { L"采样间隔", L"Sample interval" },
    { L"读取传感器的周期", L"How often the sensors are read" },
    { L"数据目录", L"Data folder" },
    { L"配置与日志的存放位置（重启后生效）", L"Where config and logs live (restart to apply)" },
    { L"日志记录", L"Logging" },
    { L"把运行日志写入数据目录", L"Write runtime logs to the data folder" },
    { L"重新加载", L"Reload" },
    { L"添加点", L"Add point" },
    { L"删除点", L"Delete point" },
    { L"重置", L"Reset" },
    { L"保存", L"Save" },
    { L"拖动圆点调整映射，点击空白处新增点", L"Drag a dot to adjust, click empty space to add a point" },
    { L"跟随系统", L"Follow system" },
    { L"安装目录", L"Install folder" },
    { L"应用数据目录", L"App data folder" },
    { L"重连中", L"Reconnecting" },
    { L"等待连接", L"Waiting for connection" },
    { L"风扇转速", L"Fan speed" },
    { L"重新连接", L"Reconnect" },
    { L"混合（取高）", L"Mixed (max)" },
    { L"混合（取平均）", L"Mixed (average)" },
    { L"系统风扇", L"System fan" },
    { L"目标转速", L"Target RPM" },
    { L"引用库", L"Libraries" },
    { L"参考项目", L"References" },
    { L"开源地址", L"Source code" },
    { L"检查更新", L"Check for updates" },
    { L"点击检查更新", L"Click to check for updates" },
    { L"正在检查更新…", L"Checking for updates…" },
    { L"已是最新版本", L"You are up to date" },
    { L"发现新版本", L"New version available:" },
    { L"打开下载页", L"Open download page" },
    { L"仓库暂无发布版本", L"No releases yet" },
    { L"检查更新失败：", L"Update check failed: " },
    { L"华硕设备控制参考", L"ASUS device control reference" },
    { L"适用的开源硬件及参考", L"Compatible open hardware and references" },
    { L"B 站视频", L"Bilibili video" },
    { L"硬件监控库", L"Hardware monitoring library" },
    { L"本仓库", L"This repository" },
    { L"固件", L"Firmware" },
    { L"Windows 通知", L"Windows notifications" },
};

Lang g_lang = Lang::Zh;

const std::unordered_map<std::wstring, const wchar_t*>& Table() {
    static const std::unordered_map<std::wstring, const wchar_t*> table = [] {
        std::unordered_map<std::wstring, const wchar_t*> map;
        for (const auto& entry : kEntries) map.emplace(entry.zh, entry.en);
        return map;
    }();
    return table;
}

} // namespace

void SetLang(Lang lang) { g_lang = lang; }
Lang GetLang() { return g_lang; }
bool IsEn() { return g_lang == Lang::En; }

const wchar_t* T(const wchar_t* zh) {
    if (g_lang == Lang::Zh || !zh) return zh;
    const auto& table = Table();
    const auto it = table.find(zh);
    return it == table.end() ? zh : it->second;
}

namespace {

// 后台错误码表：code 由 FanControl.Host 给出，{0}/{1} 由 ErrorText 用参数替换。
struct ErrEntry {
    const wchar_t* code;
    const wchar_t* zh;
    const wchar_t* en;
};

constexpr ErrEntry kErrors[] = {
    { L"config.unknownKey", L"未知配置项：{0}", L"Unknown setting: {0}" },
    { L"config.curveTooFew", L"曲线至少需要 2 个点。", L"A curve needs at least two points." },
    { L"config.curveFlat", L"两端 PWM 不能相同。", L"The two end PWM values must differ." },
    { L"config.enum", L"取值无法解析为 {0}。", L"Cannot parse the value as {0}." },
    { L"config.notString", L"取值必须是字符串。", L"The value must be a string." },
    { L"config.notBool", L"取值必须是布尔值。", L"The value must be true or false." },
    { L"config.rangeInt", L"取值必须是 {0}-{1} 之间的整数。", L"The value must be an integer between {0} and {1}." },
    { L"config.rangeNumber", L"取值必须是 {0}-{1} 之间的数值。", L"The value must be a number between {0} and {1}." },
    { L"config.applyFailed", L"应用配置失败：{0}", L"Failed to apply the settings: {0}" },
    { L"ipc.badJson", L"请求不是合法 JSON。", L"The request is not valid JSON." },
    { L"ipc.unknownCommand", L"未知命令：{0}", L"Unknown command: {0}" },
    { L"ipc.missingKey", L"缺少 key。", L"Missing \"key\"." },
    { L"ipc.missingValue", L"缺少 value。", L"Missing \"value\"." },
    { L"ipc.missingPoints", L"缺少 points 数组。", L"Missing the \"points\" array." },
    { L"ipc.badPoint", L"曲线点必须是 [x, y] 数组。", L"Each curve point must be an [x, y] array." },
    { L"error.generic", L"操作失败。", L"The operation failed." },
    { L"comm.failed", L"通信失败：{0}", L"Link failed: {0}" },
    { L"runtime.samplingFailed", L"采样失败：{0}", L"Sampling failed: {0}" },
    { L"ble.notConfigured", L"未配置 BLE 设备，请在设置中选择设备。", L"No BLE device is configured. Pick one in Settings." },
    { L"ble.deviceNotFound", L"未找到已配对的蓝牙设备：{0}", L"The paired Bluetooth device was not found: {0}" },
    { L"ble.deviceNotFoundByMac", L"未找到已配对的蓝牙设备（MAC {0}），请在设置中刷新后重新选择。",
      L"The paired Bluetooth device ({0}) was not found. Refresh the list in Settings and pick it again." },
    { L"ble.nusServiceMissing", L"设备未提供 NUS 服务，请确认固件已启用 BLE 模式。",
      L"The device does not expose the NUS service. Check that the firmware runs in BLE mode." },
    { L"ble.nusServiceMissingFor", L"{0} 未提供 NUS 服务，请确认固件已启用 BLE 模式。",
      L"{0} does not expose the NUS service. Check that the firmware runs in BLE mode." },
    { L"ble.nusCharacteristicMissing", L"设备 NUS 特征不完整，请确认固件版本。",
      L"The device's NUS characteristics are incomplete. Check the firmware version." },
    { L"ble.notConnected", L"蓝牙未连接。", L"Bluetooth is not connected." },
    { L"ble.timeout", L"蓝牙操作超时（{0} 秒）。", L"The Bluetooth operation timed out after {0}s." },
    { L"ble.gattUnavailable", L"系统未提供 BLE GATT 接口，无法使用蓝牙通信。",
      L"Windows does not expose the BLE GATT API, so Bluetooth cannot be used." },
    { L"ble.openFailed", L"打开 BLE 设备失败（Win32 {0}）。", L"Failed to open the BLE device (Win32 {0})." },
    { L"ble.writeFailed", L"蓝牙写入失败（HRESULT {0}）。", L"Bluetooth write failed (HRESULT {0})." },
    { L"com.openTimeout", L"串口打开超时（{0}），设备可能未就绪。", L"Opening {0} timed out; the device may not be ready." },
    { L"com.noHandshake", L"链路未就绪（{0} 握手无应答）。", L"The link is not ready (no handshake response on {0})." },
    { L"com.notConnected", L"串口未连接。", L"The serial port is not connected." },
    { L"curve.empty", L"曲线不能为空。", L"The curve cannot be empty." },
    { L"curve.notIncreasing", L"曲线温度点必须严格递增。", L"Curve temperature points must increase strictly." },
    { L"runtimeCurve.empty", L"转速曲线不能为空。", L"The RPM curve cannot be empty." },
    { L"runtimeCurve.notIncreasing", L"转速曲线点必须严格递增。", L"RPM curve points must increase strictly." },
    { L"autostart.noProcessPath", L"无法获取进程路径。", L"Cannot resolve the process path." },
    { L"autostart.taskFailed", L"创建自启任务失败（schtasks 退出码 {0}）。",
      L"Failed to create the startup task (schtasks exit code {0})." },
};

const std::unordered_map<std::wstring, const ErrEntry*>& ErrorTable() {
    static const auto table = [] {
        std::unordered_map<std::wstring, const ErrEntry*> map;
        for (const auto& entry : kErrors) map.emplace(entry.code, &entry);
        return map;
    }();
    return table;
}

void Substitute(std::wstring& text, const wchar_t* token, const std::wstring& value) {
    const size_t length = wcslen(token);
    for (size_t pos = text.find(token); pos != std::wstring::npos; pos = text.find(token, pos + value.size()))
        text.replace(pos, length, value);
}

} // namespace

std::wstring ErrorText(const std::wstring& code, const std::wstring& arg1, const std::wstring& arg2,
                       const std::wstring& fallback) {
    const auto& table = ErrorTable();
    const auto it = table.find(code);
    if (it == table.end()) return fallback;
    std::wstring text = g_lang == Lang::En ? it->second->en : it->second->zh;
    Substitute(text, L"{0}", arg1);
    Substitute(text, L"{1}", arg2);
    return text;
}

} // namespace fc
