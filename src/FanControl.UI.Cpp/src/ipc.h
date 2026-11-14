#pragma once

#include <string>
#include <utility>
#include <vector>

namespace fc {

struct CurvePoint {
    double x = 0;
    double y = 0;
};

// 守护进程下发的配置快照（字段名与服务端 StateJson 一一对应）
struct Config {
    std::wstring temperatureSource = L"LibreHardwareMonitor";
    std::wstring gpuTemperatureSource = L"NvidiaSmiAdl";
    std::wstring gpuSelection = L"Auto";
    std::wstring fanSpeedSource = L"AtkAcpi";
    std::wstring fanControlMode = L"CpuTemp";
    std::wstring communicationType = L"Ble";
    std::wstring comPort = L"COM3";
    int comBaudRate = 115200;
    std::wstring bleDeviceMac;
    std::wstring bleDeviceName;
    int pollIntervalMilliseconds = 1000;
    int blePollIntervalSeconds = 2;
    int bleReconnectIntervalSeconds = 5;
    bool autoReconnectBle = true;
    double pwmSmoothingWindowSeconds = 3.0;
    bool notifyOnBleDisconnect = true;
    bool notifyOnTemperatureError = true;
    std::wstring language = L"zh-CN";
    std::wstring temperatureUnit = L"Celsius";
    std::wstring theme = L"System";
    double manualPwmPercent = 50;
    bool smoothingEnabled = true;
    std::vector<CurvePoint> curve;
    std::vector<CurvePoint> rpmCurve;
};

// 实时状态快照
struct State {
    double cpu = 0;
    double gpu = 0;
    double rpm = 0;
    double pwm = 0;
    double rawPwm = 0;
    std::wstring mode;
    std::wstring connectionState = L"Waiting";
    std::wstring communicationType = L"Ble";
    std::wstring deviceName;
    std::wstring lastError;
    // 错误码 + 参数：界面按 code 翻译，lastError 只是后台给的中文兜底
    std::wstring errorCode;
    std::wstring errorArg1;
    std::wstring errorArg2;
    // 0 提示 / 1 异常 / 2 错误 / 3 验证错误
    int errorSeverity = 2;
    // 通信链路（连接/写入/校时）最近的失败，与温度采样错误分开上报
    std::wstring commError;
    std::wstring commErrorCode;
    std::wstring commErrorArg1;
    std::wstring commErrorArg2;
    int commErrorSeverity = 2;
    bool connected = false;
    bool autostart = false;
    // SystemConfig 侧（与 AppConfig 分开存，界面上一组「数据目录 / 日志」就是它）
    bool logEnabled = true;
    std::wstring userDataLocation = L"AppData";
    // 自定义数据目录（userDataLocation == "Custom" 时有效）
    std::wstring userDataPath;
    // 首次运行向导：宿主那边还没有配置就为 true，界面弹一次欢迎面板
    bool firstRun = false;
    long long timestamp = 0;
};

// 运行日志页的一条记录：时间是宿主给的 HH:mm:ss.fff，级别 0 提示 / 1 异常 / 2 错误 / 3 验证错误
struct LogEntry {
    long long seq = 0;
    std::wstring time;
    int level = 0;
    std::wstring tag;
    std::wstring category;
    std::wstring message;
};

// 弹窗列表的一项：label 显示，value 是回写给后台的取值
struct DialogItem {
    std::wstring label;
    std::wstring value;
};

struct Message {
    std::wstring event;
    bool hasConfig = false;
    bool hasState = false;
    Config config;
    State state;
    std::wstring error;
    std::wstring errorCode;
    std::wstring errorArg1;
    std::wstring errorArg2;
    int severity = 2;
    // scanBle / listPorts / listGpus 的候选列表
    std::vector<DialogItem> items;
    bool hasItems = false;
    bool autostart = false;
    bool hasAutostart = false;
    // 关于页「检查更新」结果：state = checking / current / newer / none / failed
    std::wstring updateState;
    std::wstring updateTag;
    std::wstring updateUrl;
    std::wstring updateDetail;
    // 运行日志页：event == "logs" 时带回来的增量条目
    std::vector<LogEntry> logs;
    bool hasLogs = false;
    long long logLastSeq = 0;
};

// 界面是 FanControl.exe 进程里的一个 DLL（单一进程），没有管道：
// 命令直接回调到 C# 端，状态由 C# 端序列化成同一套 JSON 推回来。
using CommandSink = void (__stdcall*)(const char* utf8Json);

/// 注册命令回调（进程启动时注册一次，之后不变）。
void SetCommandSink(CommandSink sink);
bool HasCommandSink();

/// 发一条命令给后台（单进程下就是一次函数调用）。
bool Send(const std::string& json);

/// 解析后台推来的 JSON 帧（与 1.4.0 管道帧的载荷同形）。
bool ParseMessage(const std::string& text, Message& message);

bool SetValue(const std::string& key, const std::string& valueJson);
bool SetCurve(const std::vector<CurvePoint>& points, bool rpmCurve);
bool RequestReconnect();
bool RequestState();
bool ScanBleDevices();
bool ListPorts();
bool ListGpus();
bool SetAutostart(bool enabled);
/// 关于页「检查更新」：宿主去查 GitHub releases，结果以 update 事件推回
bool CheckUpdate();
/// SystemConfig（日志开关、数据目录）走单独命令，后台会落盘 system.json
bool SetSystem(const std::string& key, const std::string& valueJson);

/// 运行日志页：拉取 seq 之后的条目（since = 0 表示从头）
bool GetLogs(long long since);
/// 清空宿主内存里的日志缓冲（不影响已落盘的日志文件）
bool ClearLogs();
/// 在资源管理器里打开宿主真实的日志目录
bool OpenLogFolder();

/// 数据存放位置切换（location = "AppData" / "ExeDirectory" / "Custom"，path 只在 Custom 时有意义）
bool SetDataLocation(const std::wstring& location, const std::wstring& path);
/// 首次运行向导收尾：语言 + 数据位置一次性落盘
bool CompleteSetup(const std::wstring& language, const std::wstring& location, const std::wstring& path);

/// UTF-8 → UTF-16
std::wstring Utf8ToWide(const std::string& text);
/// UTF-16 → UTF-8
std::string WideToUtf8(const std::wstring& text);

}  // namespace fc