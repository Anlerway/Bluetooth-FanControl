// FanControl BLE bridge — C ABI，供 .NET 侧 P/Invoke。
// 为什么用 C++/WinRT 而不是 C# 的 WinRT 投影：投影程序集 Microsoft.Windows.SDK.NET.dll 有 24 MB，
// 而本机（Windows 11 + 该蓝牙适配器）的 Win32 bluetoothleapis GATT 写入恒返回 ERROR_INVALID_FUNCTION，
// 只有 WinRT 通道能写。C++/WinRT 是纯头文件投影，运行时就是系统自带的 WinRT，DLL 只有约 200 KB。
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// 返回码，0 表示成功。C# 侧据此映射到现有错误码（ble.*）。
enum FcBleStatus
{
    FCBLE_OK = 0,
    FCBLE_ERR_UNKNOWN = 1,
    FCBLE_ERR_NOT_CONFIGURED = 2,
    FCBLE_ERR_DEVICE_NOT_FOUND = 3,
    FCBLE_ERR_SERVICE_MISSING = 4,
    FCBLE_ERR_CHARACTERISTIC_MISSING = 5,
    FCBLE_ERR_WRITE_FAILED = 6,
    FCBLE_ERR_NOT_CONNECTED = 7,
    FCBLE_ERR_TIMEOUT = 8,
    FCBLE_ERR_BUFFER_TOO_SMALL = 9
};

typedef void(__stdcall* FcBleNotifyCallback)(const uint8_t* data, int32_t length, void* user);

// 枚举已配对的 BLE 设备，输出 "名称\tMAC\n" 文本（UTF-16）。
// 返回所需字符数（含结尾 0）；若 capChars 不足则返回所需长度且不写内容。
__declspec(dllexport) int32_t __stdcall fcble_scan(wchar_t* buffer, int32_t capChars);

// 连接。macAddress 形如 "489D31E38902"（12 位十六进制，可空）；为空时按 deviceName 匹配。
__declspec(dllexport) int32_t __stdcall fcble_connect(const wchar_t* macAddress, const wchar_t* deviceName, int32_t timeoutMs);

__declspec(dllexport) int32_t __stdcall fcble_disconnect(void);

__declspec(dllexport) int32_t __stdcall fcble_is_connected(void);

__declspec(dllexport) int32_t __stdcall fcble_write(const uint8_t* data, int32_t length, int32_t timeoutMs);

// 订阅 TX 通知；传 nullptr 取消订阅。回调在 WinRT 线程池线程上调用。
__declspec(dllexport) int32_t __stdcall fcble_set_notify(FcBleNotifyCallback callback, void* user);

// 取最近一次失败的说明文本（UTF-16），返回所需字符数（含结尾 0）。
__declspec(dllexport) int32_t __stdcall fcble_last_error(wchar_t* buffer, int32_t capChars);

#ifdef __cplusplus
}
#endif
