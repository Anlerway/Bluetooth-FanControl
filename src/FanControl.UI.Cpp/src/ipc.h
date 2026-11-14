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

/// 与守护进程的命名管道客户端：一帧一个 JSON（4 字节小端长度前缀 + UTF-8）。
class IpcClient {
public:
    IpcClient() = default;
    ~IpcClient();

    IpcClient(const IpcClient&) = delete;
    IpcClient& operator=(const IpcClient&) = delete;

    /// 在 timeoutMs 内反复尝试连接，成功返回 true。
    bool Connect(int timeoutMs);
    void Close();
    bool IsOpen() const { return pipe_ != nullptr; }

    /// 读取一帧；timeoutMs < 0 表示一直阻塞到有数据。
    bool ReadMessage(int timeoutMs, Message& message);
    bool Send(const std::string& json);

    bool SetValue(const std::string& key, const std::string& valueJson);
    bool SetCurve(const std::vector<CurvePoint>& points, bool rpmCurve);
    bool RequestReconnect();
    bool RequestState();
    bool ScanBleDevices();
    bool ListPorts();
    bool ListGpus();
    bool SetAutostart(bool enabled);

private:
    void* pipe_ = nullptr;
    // 读线程和界面线程会同时做重叠 IO；共用一个事件会让两边的等待互相串信，
    // 所以读写各用一个事件。
    void* readEvent_ = nullptr;
    void* writeEvent_ = nullptr;
};

/// UTF-8 → UTF-16
std::wstring Utf8ToWide(const std::string& text);
/// UTF-16 → UTF-8
std::string WideToUtf8(const std::wstring& text);

}  // namespace fc
