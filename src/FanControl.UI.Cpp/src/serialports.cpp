#include "serialports.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <algorithm>
#include <cwchar>
#include <mutex>

#pragma comment(lib, "advapi32.lib")

namespace fc {
namespace {

constexpr wchar_t kSerialComm[] = L"HARDWARE\\DEVICEMAP\\SERIALCOMM";

std::vector<std::wstring> Enumerate() {
    std::vector<std::wstring> ports;
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kSerialComm, 0, KEY_READ, &key) == ERROR_SUCCESS) {
        for (DWORD index = 0;; ++index) {
            wchar_t valueName[512] = {};
            DWORD nameLen = 511;
            wchar_t data[128] = {};
            DWORD dataLen = sizeof(data) - sizeof(wchar_t);
            DWORD type = 0;
            const LONG rc = RegEnumValueW(key, index, valueName, &nameLen, nullptr, &type,
                                          reinterpret_cast<LPBYTE>(data), &dataLen);
            if (rc != ERROR_SUCCESS) break;
            if (type != REG_SZ || dataLen < sizeof(wchar_t) || data[0] == L'\0') continue;
            ports.emplace_back(data);
        }
        RegCloseKey(key);
    }
    std::sort(ports.begin(), ports.end(), [](const std::wstring& a, const std::wstring& b) {
        return _wcsicmp(a.c_str(), b.c_str()) < 0;
    });
    ports.erase(std::unique(ports.begin(), ports.end()), ports.end());
    return ports;
}

}  // namespace

const std::vector<std::wstring>& SerialPorts() {
    static const std::vector<std::wstring> ports = Enumerate();
    return ports;
}

}  // namespace fc
