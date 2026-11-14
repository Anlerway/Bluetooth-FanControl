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

std::wstring StrOr(const JValue* v, const std::wstring& fallback) {
    if (!v || v->kind != JValue::Str) return fallback;
    return Utf8ToWide(v->str);
}

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
    if (const JValue* severity = node.Get("errorSeverity")) state.errorSeverity = static_cast<int>(Num(severity));
    state.commError = Str(node.Get("commError"));
    state.commErrorCode = Str(node.Get("commErrorCode"));
    state.commErrorArg1 = Str(node.Get("commErrorArg1"));
    state.commErrorArg2 = Str(node.Get("commErrorArg2"));
    if (const JValue* severity = node.Get("commErrorSeverity")) state.commErrorSeverity = static_cast<int>(Num(severity));
    state.connected = Bool(node.Get("connected"));
    state.autostart = Bool(node.Get("autostart"));
    state.logEnabled = Bool(node.Get("logEnabled"), state.logEnabled);
    state.userDataLocation = StrOr(node.Get("userDataLocation"), state.userDataLocation);
    state.userDataPath = Str(node.Get("userDataPath"));
    state.firstRun = Bool(node.Get("firstRun"), state.firstRun);
    state.timestamp = static_cast<long long>(Num(node.Get("timestamp")));
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

namespace {

CommandSink g_commandSink = nullptr;

// JSON 字符串字面量：自定义目录带反斜杠，必须转义后再拼进命令
std::string JsonQuoted(const std::wstring& text) {
    const std::string utf8 = WideToUtf8(text);
    std::string out = "\"";
    for (const char c : utf8) {
        if (c == '\\' || c == '"') {
            out.push_back('\\');
            out.push_back(c);
        } else if (c == '\n') {
            out += "\\n";
        } else if (c == '\r') {
            out += "\\r";
        } else {
            out.push_back(c);
        }
    }
    out.push_back('"');
    return out;
}

}  // namespace

void SetCommandSink(CommandSink sink) { g_commandSink = sink; }

bool HasCommandSink() { return g_commandSink != nullptr; }

bool Send(const std::string& json) {
    CommandSink sink = g_commandSink;
    if (sink == nullptr) return false;
    sink(json.c_str());
    return true;
}

bool ParseMessage(const std::string& text, Message& message) {
    message = Message();
    JValue root;
    if (!JsonParser(text).Parse(root) || root.kind != JValue::Obj) return false;
    message.event = Str(root.Get("event"));
    if (const JValue* error = root.Get("message")) message.error = Str(error);
    if (const JValue* code = root.Get("code")) message.errorCode = Str(code);
    if (const JValue* arg1 = root.Get("arg1")) message.errorArg1 = Str(arg1);
    if (const JValue* arg2 = root.Get("arg2")) message.errorArg2 = Str(arg2);
    if (const JValue* severity = root.Get("severity")) message.severity = static_cast<int>(Num(severity));
    if (const JValue* config = root.Get("config")) {
        FillConfig(*config, message.config);
        message.hasConfig = true;
    }
    if (const JValue* state = root.Get("state")) {
        FillState(*state, message.state);
        message.hasState = true;
    }
    if (const JValue* autostart = root.Get("autostart")) {
        message.autostart = Bool(autostart);
        message.hasAutostart = true;
    }
    if (message.event == L"logs") {
        if (const JValue* entries = root.Get("entries"); entries && entries->kind == JValue::Arr) {
            for (const auto& item : entries->arr) {
                if (item.kind != JValue::Obj) continue;
                LogEntry entry;
                entry.seq = static_cast<long long>(Num(item.Get("seq")));
                entry.time = Str(item.Get("time"));
                entry.level = Int(item.Get("level"));
                entry.tag = Str(item.Get("tag"));
                entry.category = Str(item.Get("category"));
                entry.message = Str(item.Get("message"));
                message.logs.push_back(entry);
            }
            message.hasLogs = true;
        }
        message.logLastSeq = static_cast<long long>(Num(root.Get("lastSeq")));
    }
    if (message.event == L"update") {
        if (const JValue* state = root.Get("state")) message.updateState = Str(state);
        if (const JValue* tag = root.Get("tag")) message.updateTag = Str(tag);
        if (const JValue* url = root.Get("url")) message.updateUrl = Str(url);
        if (const JValue* detail = root.Get("detail")) message.updateDetail = Str(detail);
    }

    // 设备/端口/GPU 候选列表：三种事件形状不同，这里统一折成 label/value
    if (message.event == L"bleDevices") {
        if (const JValue* devices = root.Get("devices"); devices && devices->kind == JValue::Arr) {
            for (const auto& item : devices->arr) {
                if (item.kind != JValue::Obj) continue;
                DialogItem entry;
                entry.value = Str(item.Get("mac"));
                entry.label = Str(item.Get("name"));
                if (entry.label.empty()) entry.label = entry.value;
                if (entry.value.empty()) continue;
                message.items.push_back(entry);
            }
            message.hasItems = true;
        }
    } else if (message.event == L"ports" || message.event == L"gpus") {
        const char* key = message.event == L"ports" ? "ports" : "gpus";
        if (const JValue* list = root.Get(key); list && list->kind == JValue::Arr) {
            for (const auto& item : list->arr) {
                if (item.kind != JValue::Str) continue;
                DialogItem entry;
                entry.label = Utf8ToWide(item.str);
                entry.value = entry.label;
                message.items.push_back(entry);
            }
            message.hasItems = true;
        }
    }
    return true;
}

bool SetValue(const std::string& key, const std::string& valueJson) {
    std::string json = "{\"cmd\":\"set\",\"key\":\"" + key + "\",\"value\":" + valueJson + "}";
    return Send(json);
}

bool SetCurve(const std::vector<CurvePoint>& points, bool rpmCurve) {
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

bool RequestReconnect() { return Send("{\"cmd\":\"reconnect\"}"); }

bool RequestState() { return Send("{\"cmd\":\"getState\"}"); }

bool ScanBleDevices() { return Send("{\"cmd\":\"scanBle\"}"); }

bool ListPorts() { return Send("{\"cmd\":\"listPorts\"}"); }

bool ListGpus() { return Send("{\"cmd\":\"listGpus\"}"); }

bool SetSystem(const std::string& key, const std::string& valueJson) {
    std::string json = "{\"cmd\":\"setSystem\",\"key\":\"" + key + "\",\"value\":" + valueJson + "}";
    return Send(json);
}

bool CheckUpdate() { return Send("{\"cmd\":\"checkUpdate\"}"); }

bool GetLogs(long long since) {
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "{\"cmd\":\"getLogs\",\"since\":%lld}", since);
    return Send(buffer);
}

bool ClearLogs() { return Send("{\"cmd\":\"clearLogs\"}"); }

bool OpenLogFolder() { return Send("{\"cmd\":\"openLogs\"}"); }

bool SetDataLocation(const std::wstring& location, const std::wstring& path) {
    return Send("{\"cmd\":\"setDataLocation\",\"location\":" + JsonQuoted(location) +
                ",\"path\":" + JsonQuoted(path) + "}");
}

bool CompleteSetup(const std::wstring& language, const std::wstring& location, const std::wstring& path) {
    return Send("{\"cmd\":\"completeSetup\",\"language\":" + JsonQuoted(language) +
                ",\"location\":" + JsonQuoted(location) + ",\"path\":" + JsonQuoted(path) + "}");
}

bool SetAutostart(bool enabled) {
    return Send(std::string("{\"cmd\":\"setAutostart\",\"value\":") + (enabled ? "true" : "false") + "}");
}

}  // namespace fc
