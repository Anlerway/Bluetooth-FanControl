#include "ipc.h"

#include <windows.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace fc {
namespace {

constexpr wchar_t kPipeName[] = L"\\\\.\\pipe\\FanControl.Host.v1";

// ---------------------------------------------------------------- JSON 解析

struct JValue {
    enum Kind { Null, Bool, Num, Str, Arr, Obj } kind = Null;
    bool b = false;
    double num = 0;
    std::string str;
    std::vector<JValue> arr;
    std::vector<std::pair<std::string, JValue>> obj;

    const JValue* Get(const char* key) const {
        for (const auto& kv : obj) {
            if (kv.first == key) return &kv.second;
        }
        return nullptr;
    }
};

void AppendUtf8(std::string& out, unsigned cp) {
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

class JsonParser {
public:
    explicit JsonParser(const std::string& text) : s_(text) {}

    bool Parse(JValue& out) {
        SkipWs();
        if (!ParseValue(out, 0)) return false;
        SkipWs();
        return pos_ >= s_.size();
    }

private:
    void SkipWs() {
        while (pos_ < s_.size()) {
            const char c = s_[pos_];
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
                ++pos_;
            } else {
                break;
            }
        }
    }

    bool Literal(const char* text) {
        const size_t n = std::strlen(text);
        if (s_.compare(pos_, n, text) != 0) return false;
        pos_ += n;
        return true;
    }

    bool ParseValue(JValue& out, int depth) {
        if (depth > 64 || pos_ >= s_.size()) return false;
        switch (s_[pos_]) {
            case '{': return ParseObject(out, depth);
            case '[': return ParseArray(out, depth);
            case '"':
                out.kind = JValue::Str;
                return ParseString(out.str);
            case 't':
                if (!Literal("true")) return false;
                out.kind = JValue::Bool;
                out.b = true;
                return true;
            case 'f':
                if (!Literal("false")) return false;
                out.kind = JValue::Bool;
                out.b = false;
                return true;
            case 'n':
                if (!Literal("null")) return false;
                out.kind = JValue::Null;
                return true;
            default: return ParseNumber(out);
        }
    }

    bool ParseObject(JValue& out, int depth) {
        out.kind = JValue::Obj;
        ++pos_;  // {
        SkipWs();
        if (pos_ < s_.size() && s_[pos_] == '}') {
            ++pos_;
            return true;
        }
        while (pos_ < s_.size()) {
            SkipWs();
            std::string key;
            if (!ParseString(key)) return false;
            SkipWs();
            if (pos_ >= s_.size() || s_[pos_] != ':') return false;
            ++pos_;
            SkipWs();
            JValue value;
            if (!ParseValue(value, depth + 1)) return false;
            out.obj.emplace_back(std::move(key), std::move(value));
            SkipWs();
            if (pos_ >= s_.size()) return false;
            if (s_[pos_] == ',') {
                ++pos_;
                continue;
            }
            if (s_[pos_] == '}') {
                ++pos_;
                return true;
            }
            return false;
        }
        return false;
    }

    bool ParseArray(JValue& out, int depth) {
        out.kind = JValue::Arr;
        ++pos_;  // [
        SkipWs();
        if (pos_ < s_.size() && s_[pos_] == ']') {
            ++pos_;
            return true;
        }
        while (pos_ < s_.size()) {
            SkipWs();
            JValue value;
            if (!ParseValue(value, depth + 1)) return false;
            out.arr.push_back(std::move(value));
            SkipWs();
            if (pos_ >= s_.size()) return false;
            if (s_[pos_] == ',') {
                ++pos_;
                continue;
            }
            if (s_[pos_] == ']') {
                ++pos_;
                return true;
            }
            return false;
        }
        return false;
    }

    bool ParseString(std::string& out) {
        if (pos_ >= s_.size() || s_[pos_] != '"') return false;
        ++pos_;
        out.clear();
        while (pos_ < s_.size()) {
            const char c = s_[pos_++];
            if (c == '"') return true;
            if (c != '\\') {
                out.push_back(c);
                continue;
            }
            if (pos_ >= s_.size()) return false;
            const char esc = s_[pos_++];
            switch (esc) {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case 'u': {
                    if (pos_ + 4 > s_.size()) return false;
                    unsigned cp = static_cast<unsigned>(std::strtoul(s_.substr(pos_, 4).c_str(), nullptr, 16));
                    pos_ += 4;
                    if (cp >= 0xD800 && cp <= 0xDBFF && pos_ + 6 <= s_.size() && s_[pos_] == '\\' &&
                        s_[pos_ + 1] == 'u') {
                        const unsigned low =
                            static_cast<unsigned>(std::strtoul(s_.substr(pos_ + 2, 4).c_str(), nullptr, 16));
                        if (low >= 0xDC00 && low <= 0xDFFF) {
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                            pos_ += 6;
                        }
                    }
                    AppendUtf8(out, cp);
                    break;
                }
                default: return false;
            }
        }
        return false;
    }

    bool ParseNumber(JValue& out) {
        const size_t start = pos_;
        if (pos_ < s_.size() && (s_[pos_] == '-' || s_[pos_] == '+')) ++pos_;
        while (pos_ < s_.size()) {
            const char c = s_[pos_];
            if ((c >= '0' && c <= '9') || c == '.' || c == 'e' || c == 'E' || c == '+' || c == '-') {
                ++pos_;
            } else {
                break;
            }
        }
        if (pos_ == start) return false;
        out.kind = JValue::Num;
        out.num = std::strtod(s_.substr(start, pos_ - start).c_str(), nullptr);
        return true;
    }

    const std::string& s_;
    size_t pos_ = 0;
};

std::wstring Str(const JValue* v) { return v && v->kind == JValue::Str ? Utf8ToWide(v->str) : std::wstring(); }

double Num(const JValue* v, double fallback = 0) {
    if (!v) return fallback;
    if (v->kind == JValue::Num) return v->num;
    if (v->kind == JValue::Bool) return v->b ? 1 : 0;
    return fallback;
}

int Int(const JValue* v, int fallback = 0) { return static_cast<int>(Num(v, fallback)); }

bool Bool(const JValue* v, bool fallback = false) {
    if (!v) return fallback;
    if (v->kind == JValue::Bool) return v->b;
    if (v->kind == JValue::Num) return v->num != 0;
    return fallback;
}

std::vector<CurvePoint> Points(const JValue* v) {
    std::vector<CurvePoint> points;
    if (!v || v->kind != JValue::Arr) return points;
    for (const auto& item : v->arr) {
        if (item.kind != JValue::Arr || item.arr.size() < 2) continue;
        points.push_back(CurvePoint{Num(&item.arr[0]), Num(&item.arr[1])});
    }
    return points;
}

void FillConfig(const JValue& node, Config& config) {
    if (node.kind != JValue::Obj) return;
    config.temperatureSource = Str(node.Get("temperatureSource"));
    config.gpuTemperatureSource = Str(node.Get("gpuTemperatureSource"));
    config.gpuSelection = Str(node.Get("gpuSelection"));
    config.fanSpeedSource = Str(node.Get("fanSpeedSource"));
    config.fanControlMode = Str(node.Get("fanControlMode"));
    config.communicationType = Str(node.Get("communicationType"));
    config.comPort = Str(node.Get("comPort"));
    config.comBaudRate = Int(node.Get("comBaudRate"), config.comBaudRate);
    config.bleDeviceMac = Str(node.Get("bleDeviceMac"));
    config.bleDeviceName = Str(node.Get("bleDeviceName"));
    config.pollIntervalMilliseconds = Int(node.Get("pollIntervalMilliseconds"), config.pollIntervalMilliseconds);
    config.blePollIntervalSeconds = Int(node.Get("blePollIntervalSeconds"), config.blePollIntervalSeconds);
    config.bleReconnectIntervalSeconds =
        Int(node.Get("bleReconnectIntervalSeconds"), config.bleReconnectIntervalSeconds);
    config.autoReconnectBle = Bool(node.Get("autoReconnectBle"), config.autoReconnectBle);
    config.pwmSmoothingWindowSeconds = Num(node.Get("pwmSmoothingWindowSeconds"), config.pwmSmoothingWindowSeconds);
    config.notifyOnBleDisconnect = Bool(node.Get("notifyOnBleDisconnect"), config.notifyOnBleDisconnect);
    config.notifyOnTemperatureError = Bool(node.Get("notifyOnTemperatureError"), config.notifyOnTemperatureError);
    config.language = Str(node.Get("language"));
    config.temperatureUnit = Str(node.Get("temperatureUnit"));
    config.theme = Str(node.Get("theme"));
    config.manualPwmPercent = Num(node.Get("manualPwmPercent"), config.manualPwmPercent);
    config.smoothingEnabled = Bool(node.Get("smoothingEnabled"), config.smoothingEnabled);
    config.curve = Points(node.Get("curve"));
    config.rpmCurve = Points(node.Get("rpmCurve"));
}

void FillState(const JValue& node, State& state) {
    if (node.kind != JValue::Obj) return;
    state.cpu = Num(node.Get("cpu"));
    state.gpu = Num(node.Get("gpu"));
    state.rpm = Num(node.Get("rpm"));
    state.pwm = Num(node.Get("pwm"));
    state.rawPwm = Num(node.Get("rawPwm"));
    state.mode = Str(node.Get("mode"));
    state.connectionState = Str(node.Get("connectionState"));
    state.communicationType = Str(node.Get("communicationType"));
    state.deviceName = Str(node.Get("deviceName"));
    state.lastError = Str(node.Get("lastError"));
    state.errorCode = Str(node.Get("errorCode"));
    state.errorArg1 = Str(node.Get("errorArg1"));
    state.errorArg2 = Str(node.Get("errorArg2"));
    state.connected = Bool(node.Get("connected"));
    state.timestamp = static_cast<long long>(Num(node.Get("timestamp")));
}

// 管道句柄以 FILE_FLAG_OVERLAPPED 打开：读写都带超时，界面线程永远不会被
// 一个空闲/半死的后台连接卡死（同步句柄上 WriteFile 可能无限阻塞）。
bool OverlappedIo(HANDLE pipe, HANDLE ioEvent, bool write, void* buffer, DWORD size, DWORD timeoutMs,
                  DWORD* transferred) {
    if (ioEvent == nullptr) return false;
    OVERLAPPED overlapped = {};
    overlapped.hEvent = ioEvent;
    ResetEvent(ioEvent);

    const BOOL started = write ? WriteFile(pipe, buffer, size, nullptr, &overlapped)
                               : ReadFile(pipe, buffer, size, nullptr, &overlapped);
    if (!started) {
        const DWORD error = GetLastError();
        if (error != ERROR_IO_PENDING) return false;
    }

    if (WaitForSingleObject(ioEvent, timeoutMs) != WAIT_OBJECT_0) {
        CancelIoEx(pipe, &overlapped);
        return false;
    }

    DWORD done = 0;
    if (!GetOverlappedResult(pipe, &overlapped, &done, FALSE) || done == 0) return false;
    *transferred = done;
    return true;
}

bool ReadExact(HANDLE pipe, HANDLE ioEvent, void* buffer, DWORD size, DWORD timeoutMs) {
    auto* cursor = static_cast<char*>(buffer);
    DWORD remaining = size;
    while (remaining > 0) {
        DWORD read = 0;
        if (!OverlappedIo(pipe, ioEvent, false, cursor, remaining, timeoutMs, &read)) return false;
        cursor += read;
        remaining -= read;
    }
    return true;
}

bool WriteExact(HANDLE pipe, HANDLE ioEvent, const void* buffer, DWORD size, DWORD timeoutMs) {
    auto* cursor = static_cast<const char*>(buffer);
    DWORD remaining = size;
    while (remaining > 0) {
        DWORD written = 0;
        if (!OverlappedIo(pipe, ioEvent, true, const_cast<char*>(cursor), remaining, timeoutMs, &written)) return false;
        cursor += written;
        remaining -= written;
    }
    return true;
}

}  // namespace

std::wstring Utf8ToWide(const std::string& text) {
    if (text.empty()) return std::wstring();
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0);
    if (size <= 0) return std::wstring();
    std::wstring wide(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), wide.data(), size);
    return wide;
}

std::string WideToUtf8(const std::wstring& text) {
    if (text.empty()) return std::string();
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0, nullptr,
                                        nullptr);
    if (size <= 0) return std::string();
    std::string narrow(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), narrow.data(), size, nullptr, nullptr);
    return narrow;
}

IpcClient::~IpcClient() { Close(); }

bool IpcClient::Connect(int timeoutMs) {
    Close();
    const ULONGLONG deadline = GetTickCount64() + static_cast<ULONGLONG>(timeoutMs < 0 ? 0 : timeoutMs);
    for (;;) {
        HANDLE pipe = CreateFileW(kPipeName, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                                  FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, nullptr);
        if (pipe != INVALID_HANDLE_VALUE) {
            pipe_ = pipe;
            ioEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            return true;
        }
        if (timeoutMs >= 0 && GetTickCount64() >= deadline) return false;
        Sleep(100);
    }
}

void IpcClient::Close() {
    if (pipe_ != nullptr) {
        CancelIoEx(static_cast<HANDLE>(pipe_), nullptr);
        CloseHandle(static_cast<HANDLE>(pipe_));
        pipe_ = nullptr;
    }
    if (ioEvent_ != nullptr) {
        CloseHandle(static_cast<HANDLE>(ioEvent_));
        ioEvent_ = nullptr;
    }
}

bool IpcClient::ReadMessage(int timeoutMs, Message& message) {
    if (pipe_ == nullptr) return false;
    HANDLE pipe = static_cast<HANDLE>(pipe_);

    HANDLE ioEvent = static_cast<HANDLE>(ioEvent_);
    const DWORD headerTimeout = timeoutMs < 0 ? INFINITE : static_cast<DWORD>(timeoutMs);

    unsigned char header[4] = {};
    if (!ReadExact(pipe, ioEvent, header, 4, headerTimeout)) return false;
    const unsigned length =
        static_cast<unsigned>(header[0]) | (static_cast<unsigned>(header[1]) << 8) |
        (static_cast<unsigned>(header[2]) << 16) | (static_cast<unsigned>(header[3]) << 24);
    if (length == 0 || length > (1u << 20)) return false;
    std::string payload(length, '\0');
    if (!ReadExact(pipe, ioEvent, payload.data(), length, 4000)) return false;

    message = Message();
    JValue root;
    if (!JsonParser(payload).Parse(root) || root.kind != JValue::Obj) return false;
    message.event = Str(root.Get("event"));
    if (const JValue* error = root.Get("message")) message.error = Str(error);
    if (const JValue* code = root.Get("code")) message.errorCode = Str(code);
    if (const JValue* arg1 = root.Get("arg1")) message.errorArg1 = Str(arg1);
    if (const JValue* arg2 = root.Get("arg2")) message.errorArg2 = Str(arg2);
    if (const JValue* config = root.Get("config")) {
        FillConfig(*config, message.config);
        message.hasConfig = true;
    }
    if (const JValue* state = root.Get("state")) {
        FillState(*state, message.state);
        message.hasState = true;
    }
    return true;
}

bool IpcClient::Send(const std::string& json) {
    if (pipe_ == nullptr) return false;
    HANDLE pipe = static_cast<HANDLE>(pipe_);
    const unsigned length = static_cast<unsigned>(json.size());
    unsigned char header[4] = {static_cast<unsigned char>(length & 0xFF),
                               static_cast<unsigned char>((length >> 8) & 0xFF),
                               static_cast<unsigned char>((length >> 16) & 0xFF),
                               static_cast<unsigned char>((length >> 24) & 0xFF)};
    HANDLE ioEvent = static_cast<HANDLE>(ioEvent_);
    if (!WriteExact(pipe, ioEvent, header, 4, 2000)) return false;
    return WriteExact(pipe, ioEvent, json.data(), length, 2000);
}

bool IpcClient::SetValue(const std::string& key, const std::string& valueJson) {
    std::string json = "{\"cmd\":\"set\",\"key\":\"" + key + "\",\"value\":" + valueJson + "}";
    return Send(json);
}

bool IpcClient::SetCurve(const std::vector<CurvePoint>& points, bool rpmCurve) {
    char buffer[64];
    std::string json = rpmCurve ? "{\"cmd\":\"setRpmCurve\",\"points\":[" : "{\"cmd\":\"setCurve\",\"points\":[";
    bool first = true;
    for (const auto& point : points) {
        std::snprintf(buffer, sizeof(buffer), "%s[%.4f,%.4f]", first ? "" : ",", point.x, point.y);
        json += buffer;
        first = false;
    }
    json += "]}";
    return Send(json);
}

bool IpcClient::RequestReconnect() { return Send("{\"cmd\":\"reconnect\"}"); }

bool IpcClient::RequestState() { return Send("{\"cmd\":\"getState\"}"); }

}  // namespace fc
