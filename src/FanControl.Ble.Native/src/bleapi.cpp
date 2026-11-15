#include "bleapi.h"

#include <windows.h>

#include <atomic>
#include <cwctype>
#include <mutex>
#include <string>
#include <vector>

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.Devices.Enumeration.h>
#include <winrt/Windows.Devices.Bluetooth.h>
#include <winrt/Windows.Devices.Bluetooth.GenericAttributeProfile.h>

using namespace winrt;
using namespace Windows::Devices::Bluetooth;
using namespace Windows::Devices::Bluetooth::GenericAttributeProfile;
using namespace Windows::Devices::Enumeration;
using namespace Windows::Storage::Streams;

namespace
{
constexpr GUID kNusService = { 0x6e400001, 0xb5a3, 0xf393, { 0xe0, 0xa9, 0xe5, 0x0e, 0x24, 0xdc, 0xca, 0x9e } };
constexpr GUID kNusRx = { 0x6e400002, 0xb5a3, 0xf393, { 0xe0, 0xa9, 0xe5, 0x0e, 0x24, 0xdc, 0xca, 0x9e } };
constexpr GUID kNusTx = { 0x6e400003, 0xb5a3, 0xf393, { 0xe0, 0xa9, 0xe5, 0x0e, 0x24, 0xdc, 0xca, 0x9e } };

constexpr int32_t kOpenTimeoutMs = 10000;
constexpr int32_t kDiscoverTimeoutMs = 10000;
constexpr int32_t kSubscribeTimeoutMs = 5000;
constexpr int32_t kScanTimeoutMs = 5000;

std::mutex g_mutex;              // 串行化 connect/write/disconnect
BluetoothLEDevice g_device{ nullptr };
GattCharacteristic g_rx{ nullptr };
GattCharacteristic g_tx{ nullptr };
event_token g_txToken{};
FcBleNotifyCallback g_notify = nullptr;
void* g_notifyUser = nullptr;
std::wstring g_lastError;

void SetErrorLocked(std::wstring const& text)
{
    g_lastError = text;
}

std::wstring Describe(hresult_error const& error)
{
    wchar_t code[16] = {};
    swprintf_s(code, L"0x%08X ", static_cast<unsigned>(error.code()));
    return std::wstring(code) + error.message().c_str();
}

void EnsureApartment()
{
    try
    {
        winrt::init_apartment(apartment_type::multi_threaded);
    }
    catch (...)
    {
        // 已初始化（MTA）时重复调用会抛，忽略
    }
}

template <typename TOp, typename TResult>
bool Await(TOp const& op, int32_t timeoutMs, TResult& result, int32_t& code)
{
    handle ev{ CreateEventW(nullptr, TRUE, FALSE, nullptr) };
    if (!ev)
    {
        code = FCBLE_ERR_UNKNOWN;
        return false;
    }

    op.Completed([h = ev.get()](auto&&, auto&&) { ::SetEvent(h); });
    const DWORD waited = ::WaitForSingleObject(ev.get(), static_cast<DWORD>(timeoutMs));
    if (waited != WAIT_OBJECT_0)
    {
        try { op.Cancel(); } catch (...) {}
        try { op.Completed(nullptr); } catch (...) {}
        code = FCBLE_ERR_TIMEOUT;
        return false;
    }

    try { op.Completed(nullptr); } catch (...) {}
    if (op.Status() != Windows::Foundation::AsyncStatus::Completed)
    {
        code = FCBLE_ERR_TIMEOUT;
        return false;
    }

    result = op.GetResults();
    code = FCBLE_OK;
    return true;
}

bool ParseMac(std::wstring const& text, uint64_t& address)
{
    std::wstring hex;
    for (wchar_t ch : text)
    {
        if (iswxdigit(ch) != 0)
        {
            hex.push_back(ch);
        }
        else if (ch == L':' || ch == L'-' || ch == L' ' || ch == L'\t')
        {
            continue;
        }
        else
        {
            return false;
        }
    }

    if (hex.size() != 12)
    {
        return false;
    }

    address = wcstoull(hex.c_str(), nullptr, 16);
    return true;
}

std::wstring FormatMac(uint64_t address)
{
    wchar_t text[16] = {};
    swprintf_s(text, L"%012llX", static_cast<unsigned long long>(address));
    return text;
}

bool EqualsNoCase(std::wstring const& left, std::wstring const& right)
{
    if (left.size() != right.size())
    {
        return false;
    }

    for (size_t index = 0; index < left.size(); ++index)
    {
        if (towlower(left[index]) != towlower(right[index]))
        {
            return false;
        }
    }

    return true;
}

struct DiscoveredDevice
{
    std::wstring Name;
    std::wstring Mac;
    std::wstring Id;
};

std::vector<DiscoveredDevice> EnumerateDevices(hstring const& selector, int32_t timeoutMs)
{
    std::vector<DiscoveredDevice> found;

    auto operation = DeviceInformation::FindAllAsync(selector);
    DeviceInformationCollection collection{ nullptr };
    int32_t code = FCBLE_OK;
    if (!Await(operation, timeoutMs, collection, code))
    {
        return found;
    }

    for (auto const& info : collection)
    {
        DiscoveredDevice entry;
        entry.Name = info.Name().c_str();
        entry.Id = info.Id().c_str();

        uint64_t address = 0;
        if (ParseMac(entry.Id, address) && address != 0)
        {
            entry.Mac = FormatMac(address);
        }
        else
        {
            try
            {
                auto open = BluetoothLEDevice::FromIdAsync(info.Id());
                BluetoothLEDevice device{ nullptr };
                int32_t openCode = FCBLE_OK;
                if (Await(open, timeoutMs, device, openCode) && device)
                {
                    if (device.BluetoothAddress() != 0)
                    {
                        entry.Mac = FormatMac(device.BluetoothAddress());
                    }

                    if (entry.Name.empty())
                    {
                        entry.Name = device.Name().c_str();
                    }

                    device.Close();
                }
            }
            catch (...)
            {
                // 打不开这台设备就跳过 MAC：名称仍可用于显示
            }
        }

        if (entry.Name.empty() && entry.Mac.empty())
        {
            continue;
        }

        found.push_back(entry);
    }

    return found;
}

void StopNotifyLocked()
{
    if (g_txToken.value != 0)
    {
        try { g_tx.ValueChanged(g_txToken); } catch (...) {}
        g_txToken = {};
    }
}

void CloseLocked()
{
    StopNotifyLocked();
    g_rx = nullptr;
    g_tx = nullptr;
    if (g_device)
    {
        try { g_device.Close(); } catch (...) {}
        g_device = nullptr;
    }
}

GattCharacteristic FindCharacteristic(GattDeviceService const& service, GUID const& uuid, int32_t& code)
{
    auto operation = service.GetCharacteristicsForUuidAsync(guid(uuid), BluetoothCacheMode::Uncached);
    GattCharacteristicsResult result{ nullptr };
    if (!Await(operation, kDiscoverTimeoutMs, result, code))
    {
        return nullptr;
    }

    if (result.Status() != GattCommunicationStatus::Success || result.Characteristics().Size() == 0)
    {
        code = FCBLE_ERR_CHARACTERISTIC_MISSING;
        return nullptr;
    }

    return result.Characteristics().GetAt(0);
}

int32_t ConnectLocked(std::wstring const& mac, std::wstring const& name, int32_t timeoutMs)
{
    CloseLocked();

    BluetoothLEDevice device{ nullptr };
    uint64_t address = 0;
    int32_t code = FCBLE_OK;

    if (!mac.empty() && ParseMac(mac, address))
    {
        auto operation = BluetoothLEDevice::FromBluetoothAddressAsync(address);
        if (!Await(operation, timeoutMs, device, code) || !device)
        {
            SetErrorLocked(L"FromBluetoothAddressAsync failed for " + mac);
            return code == FCBLE_OK ? FCBLE_ERR_DEVICE_NOT_FOUND : code;
        }
    }
    else
    {
        auto selector = BluetoothLEDevice::GetDeviceSelector();
        auto candidates = EnumerateDevices(selector, kScanTimeoutMs);
        std::wstring target = name;
        for (auto const& candidate : candidates)
        {
            if (!target.empty() && !EqualsNoCase(candidate.Name, target))
            {
                continue;
            }

            if (target.empty() && candidate.Mac.empty())
            {
                continue;
            }

            if (candidate.Mac.empty())
            {
                continue;
            }

            uint64_t candidateAddress = 0;
            if (!ParseMac(candidate.Mac, candidateAddress))
            {
                continue;
            }

            auto operation = BluetoothLEDevice::FromBluetoothAddressAsync(candidateAddress);
            if (Await(operation, timeoutMs, device, code) && device)
            {
                break;
            }

            device = nullptr;
        }

        if (!device)
        {
            SetErrorLocked(target.empty() ? L"no BLE device matched" : L"device not found: " + target);
            return FCBLE_ERR_DEVICE_NOT_FOUND;
        }
    }

    // Uncached 会真实向设备取一次 GATT 表：WinRT 在这里会隐式建立连接
    auto servicesOperation = device.GetGattServicesForUuidAsync(guid(kNusService), BluetoothCacheMode::Uncached);
    GattDeviceServicesResult services{ nullptr };
    if (!Await(servicesOperation, kDiscoverTimeoutMs, services, code))
    {
        SetErrorLocked(L"GetGattServicesForUuidAsync timed out");
        try { device.Close(); } catch (...) {}
        return code;
    }

    if (services.Status() != GattCommunicationStatus::Success || services.Services().Size() == 0)
    {
        SetErrorLocked(L"NUS service missing");
        try { device.Close(); } catch (...) {}
        return FCBLE_ERR_SERVICE_MISSING;
    }

    auto service = services.Services().GetAt(0);
    for (uint32_t index = 1; index < services.Services().Size(); ++index)
    {
        try { services.Services().GetAt(index).Close(); } catch (...) {}
    }

    auto rx = FindCharacteristic(service, kNusRx, code);
    if (!rx)
    {
        SetErrorLocked(L"NUS RX characteristic missing");
        try { service.Close(); } catch (...) {}
        try { device.Close(); } catch (...) {}
        return code == FCBLE_OK ? FCBLE_ERR_CHARACTERISTIC_MISSING : code;
    }

    auto tx = FindCharacteristic(service, kNusTx, code);
    code = FCBLE_OK;

    // 订阅 TX 通知：尽力而为，失败不影响写入通道
    if (tx && g_notify)
    {
        try
        {
            auto cccd = tx.WriteClientCharacteristicConfigurationDescriptorAsync(
                GattClientCharacteristicConfigurationDescriptorValue::Notify);
            GattCommunicationStatus status = GattCommunicationStatus::Unreachable;
            int32_t cccdCode = FCBLE_OK;
            if (Await(cccd, kSubscribeTimeoutMs, status, cccdCode) && status == GattCommunicationStatus::Success)
            {
                g_txToken = tx.ValueChanged([](GattCharacteristic const&, GattValueChangedEventArgs const& args) {
                    FcBleNotifyCallback callback = g_notify;
                    void* user = g_notifyUser;
                    if (!callback)
                    {
                        return;
                    }

                    try
                    {
                        auto reader = DataReader::FromBuffer(args.CharacteristicValue());
                        const uint32_t length = reader.UnconsumedBufferLength();
                        std::vector<uint8_t> payload(length);
                        if (length > 0)
                        {
                            reader.ReadBytes(payload);
                        }

                        callback(payload.data(), static_cast<int32_t>(length), user);
                    }
                    catch (...)
                    {
                    }
                });
            }
        }
        catch (...)
        {
        }
    }

    g_device = device;
    g_rx = rx;
    g_tx = tx;
    return FCBLE_OK;
}
} // namespace

extern "C"
{
int32_t __stdcall fcble_scan(wchar_t* buffer, int32_t capChars)
{
    EnsureApartment();

    std::wstring text;
    try
    {
        auto selector = BluetoothLEDevice::GetDeviceSelector();
        auto devices = EnumerateDevices(selector, kScanTimeoutMs);
        for (auto const& device : devices)
        {
            text += device.Name;
            text += L'\t';
            text += device.Mac;
            text += L'\n';
        }
    }
    catch (hresult_error const& error)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        SetErrorLocked(Describe(error));
        return -static_cast<int32_t>(FCBLE_ERR_UNKNOWN);
    }
    catch (...)
    {
        return -static_cast<int32_t>(FCBLE_ERR_UNKNOWN);
    }

    const int32_t needed = static_cast<int32_t>(text.size() + 1);
    if (!buffer || capChars < needed)
    {
        return needed;
    }

    memcpy(buffer, text.c_str(), static_cast<size_t>(needed) * sizeof(wchar_t));
    return needed;
}

int32_t __stdcall fcble_connect(const wchar_t* macAddress, const wchar_t* deviceName, int32_t timeoutMs)
{
    EnsureApartment();

    const std::wstring mac = macAddress ? macAddress : L"";
    const std::wstring name = deviceName ? deviceName : L"";
    if (mac.empty() && name.empty())
    {
        return FCBLE_ERR_NOT_CONFIGURED;
    }

    try
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        return ConnectLocked(mac, name, timeoutMs > 0 ? timeoutMs : kOpenTimeoutMs);
    }
    catch (hresult_error const& error)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        SetErrorLocked(Describe(error));
        return FCBLE_ERR_UNKNOWN;
    }
    catch (...)
    {
        return FCBLE_ERR_UNKNOWN;
    }
}

int32_t __stdcall fcble_disconnect(void)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    CloseLocked();
    return FCBLE_OK;
}

int32_t __stdcall fcble_is_connected(void)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    return (g_rx && g_device) ? 1 : 0;
}

int32_t __stdcall fcble_write(const uint8_t* data, int32_t length, int32_t timeoutMs)
{
    if (!data || length <= 0)
    {
        return FCBLE_ERR_WRITE_FAILED;
    }

    try
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (!g_rx)
        {
            return FCBLE_ERR_NOT_CONNECTED;
        }

        DataWriter writer;
        writer.WriteBytes(array_view<uint8_t const>(data, static_cast<uint32_t>(length)));
        auto buffer = writer.DetachBuffer();

        auto operation = g_rx.WriteValueAsync(buffer, GattWriteOption::WriteWithResponse);
        GattCommunicationStatus status = GattCommunicationStatus::Unreachable;
        int32_t code = FCBLE_OK;
        if (!Await(operation, timeoutMs > 0 ? timeoutMs : 5000, status, code))
        {
            return code;
        }

        if (status != GattCommunicationStatus::Success)
        {
            wchar_t text[64] = {};
            swprintf_s(text, L"WriteValueAsync status=%d", static_cast<int>(status));
            SetErrorLocked(text);
            return FCBLE_ERR_WRITE_FAILED;
        }

        return FCBLE_OK;
    }
    catch (hresult_error const& error)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        SetErrorLocked(Describe(error));
        return FCBLE_ERR_WRITE_FAILED;
    }
    catch (...)
    {
        return FCBLE_ERR_WRITE_FAILED;
    }
}

int32_t __stdcall fcble_set_notify(FcBleNotifyCallback callback, void* user)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    g_notify = callback;
    g_notifyUser = user;
    return FCBLE_OK;
}

int32_t __stdcall fcble_last_error(wchar_t* buffer, int32_t capChars)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    const int32_t needed = static_cast<int32_t>(g_lastError.size() + 1);
    if (!buffer || capChars < needed)
    {
        return needed;
    }

    memcpy(buffer, g_lastError.c_str(), static_cast<size_t>(needed) * sizeof(wchar_t));
    return needed;
}
}
