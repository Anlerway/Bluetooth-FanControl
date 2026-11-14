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
    { L"后台已连接", L"Backend connected" },
    { L"演示数据（后台未连接）", L"Demo data (backend offline)" },
    { L"深色", L"Dark" }, { L"跟随系统", L"System" },
    { L"浅色", L"Light" },
    { L"后台 v1.4.0 · %s", L"Backend v1.4.0 · %s" },
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
    { L"登录后自动启动后台守护进程", L"Start the background service at sign-in" },
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
    { L"后端", L"Backend" },
    { L"配置文件", L"Config file" },
    { L"打开日志目录", L"Open log folder" },
    { L"检查更新", L"Check for updates" },
    { L"FanControl 守护进程（命名管道）", L"FanControl background service (named pipe)" },
    { L"未连接（演示数据）", L"Not connected (demo data)" },
    { L"1.4.0（C++ UI）", L"1.4.0 (C++ UI)" },
    { L"实时温度、转速与趋势", L"Live temperatures, RPM and trends" },
    { L"外观、硬件来源与控制行为", L"Appearance, hardware sources and control" },
    { L"温度到风扇转速的映射", L"Mapping from temperature to fan speed" },
    { L"版本与运行信息", L"Version and runtime info" },
    { L"等待后台连接", L"Waiting for backend" },
    { L"未配对设备", L"No device paired" },
    { L"开机自启由安装器写入（待接线）", L"Startup entry is written by the installer (not wired yet)" },
    { L"已关闭开机自启（待接线）", L"Startup disabled (not wired yet)" },
    { L"启动后台失败（需要管理员权限）", L"Failed to start the backend (administrator rights required)" },
    { L"命令已发送：", L"Command sent: " },
    { L"当前已是最新版本", L"You are on the latest version" },
    { L"模拟数据", L"Simulated" },
    { L"EC 直读", L"EC direct" },
    { L"摄氏度 °C", L"Celsius °C" },
    { L"华氏度 °F", L"Fahrenheit °F" },
    { L"English", L"English" },
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

} // namespace fc
