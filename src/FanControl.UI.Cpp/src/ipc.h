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
    // 通信链路（连接/写入/校时）最近的失败，与温度采样错误分开上报
    std::wstring commError;
    std::wstring commErrorCode;
    std::wstring commErrorArg1;
    std::wstring commErrorArg2;
    bool connected = false;
    bool autostart = false;
    // SystemConfig 侧（与 AppConfig 分开存，界面上一组「数据目录 / 日志」就是它）
    bool logEnabled = true;
    std::wstring userDataLocation = L"AppData";
    long long timestamp = 0;
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
    // scanBle / listPorts / listGpus 的候选列表
    std::vector<DialogItem> items;
    bool hasItems = false;
    bool autostart = false;
    bool hasAutostart = false;
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
/// SystemConfig（日志开关、数据目录）走单独命令，后台会落盘 system.json
bool SetSystem(const std::string& key, const std::string& valueJson);

/// UTF-8 → UTF-16
std::wstring Utf8ToWide(const std::string& text);
/// UTF-16 → UTF-8
std::string WideToUtf8(const std::wstring& text);

}  // namespace fc