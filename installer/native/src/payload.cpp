#include "payload.h"

#include <cstring>

#include "lzma2.h"
#include "sysutil.h"

namespace fc {
namespace {

constexpr char   kFooterMagic[8] = {'F', 'C', 'P', 'A', 'Y', 'L', 'D', '1'};
constexpr char   kArchMagic[8]   = {'F', 'C', 'A', 'R', 'C', 'H', '0', '1'};
constexpr size_t kFooterSize     = 64;
constexpr uint32_t kMaxFiles     = 200000;

uint32_t ReadU32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

uint64_t ReadU64(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; --i) v = (v << 8) | p[i];
    return v;
}

bool ReadAt(HANDLE file, uint64_t offset, void* buffer, size_t length) {
    LARGE_INTEGER li;
    li.QuadPart = static_cast<LONGLONG>(offset);
    if (!SetFilePointerEx(file, li, nullptr, FILE_BEGIN)) return false;
    auto* p = static_cast<uint8_t*>(buffer);
    size_t done = 0;
    while (done < length) {
        DWORD got = 0;
        DWORD want = static_cast<DWORD>(length - done > 1u << 24 ? 1u << 24 : length - done);
        if (!ReadFile(file, p + done, want, &got, nullptr) || got == 0) return false;
        done += got;
    }
    return true;
}

[[noreturn]] void Fail(std::string* error, const char* what) {
    if (error) *error = what;
    throw 1;
}

bool ValidEntryName(const std::string& name) {
    if (name.empty() || name.size() > 4096) return false;
    if (name[0] == '/' || name[0] == '\\') return false;
    if (name.find('\\') != std::string::npos) return false;   // 归档内统一用 '/'
    if (name.find(':') != std::string::npos) return false;    // 禁止盘符/ADS
    size_t start = 0;
    while (start <= name.size()) {
        size_t end = name.find('/', start);
        if (end == std::string::npos) end = name.size();
        std::string part = name.substr(start, end - start);
        if (part.empty() || part == "." || part == "..") return false;
        if (end == name.size()) break;
        start = end + 1;
    }
    return true;
}

std::wstring EntryToPath(const std::wstring& dir, const std::string& name) {
    std::wstring rel = Utf8ToWide(name);
    for (wchar_t& ch : rel) {
        if (ch == L'/') ch = L'\\';
    }
    return Join(dir, rel);
}

}  // namespace

bool PayloadSet::Load(const std::wstring& exePath, PayloadSet* out, std::string* error) {
    if (!out) return false;
    *out = PayloadSet{};

    HANDLE file = CreateFileW(exePath.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        if (error) *error = "payload: cannot open exe";
        return false;
    }
    LARGE_INTEGER size;
    if (!GetFileSizeEx(file, &size) || size.QuadPart < static_cast<LONGLONG>(kFooterSize)) {
        CloseHandle(file);
        out->exeSize = size.QuadPart > 0 ? static_cast<uint64_t>(size.QuadPart) : 0;
        return true;   // 无负载
    }
    uint8_t footer[kFooterSize];
    if (!ReadAt(file, static_cast<uint64_t>(size.QuadPart) - kFooterSize, footer, kFooterSize)) {
        CloseHandle(file);
        if (error) *error = "payload: cannot read footer";
        return false;
    }
    CloseHandle(file);

    if (std::memcmp(footer, kFooterMagic, 8) != 0) {
        out->exeSize = static_cast<uint64_t>(size.QuadPart);
        return true;   // 未追加负载
    }

    const uint32_t version = ReadU32(footer + 8);
    const uint32_t flags   = ReadU32(footer + 12);
    if (version != 2) {
        if (error) *error = "payload: unsupported footer version";
        return false;
    }

    const uint64_t fileSize = static_cast<uint64_t>(size.QuadPart);
    const uint64_t limit    = fileSize - kFooterSize;
    uint64_t firstOffset    = fileSize;

    for (int i = 0; i < 2; ++i) {
        PayloadSegment& seg = out->seg[i];
        seg.present  = (flags & (1u << i)) != 0;
        seg.dictProp = static_cast<uint8_t>((flags >> (8 + 8 * i)) & 0xFF);
        if (!seg.present) continue;
        const uint8_t* base = footer + 16 + i * 24;
        seg.offset   = ReadU64(base);
        seg.compSize = ReadU64(base + 8);
        seg.rawSize  = ReadU64(base + 16);
        if (seg.offset < kFooterSize || seg.compSize == 0 || seg.rawSize == 0 ||
            seg.offset > limit || seg.compSize > limit - seg.offset) {
            if (error) *error = "payload: footer points outside file";
            return false;
        }
        if (seg.offset < firstOffset) firstOffset = seg.offset;
    }
    out->exeSize = firstOffset;
    return true;
}

const PayloadSegment* PayloadSet::Select(bool preferSlim) const {
    const PayloadSegment* full = seg[0].present ? &seg[0] : nullptr;
    const PayloadSegment* slim = seg[1].present ? &seg[1] : nullptr;
    if (preferSlim) return slim ? slim : full;
    return full ? full : slim;
}

bool DecodeArchive(const std::wstring& exePath, const PayloadSegment& seg, std::vector<uint8_t>* raw,
                   std::string* error) {
    if (!raw) return false;
    raw->clear();
    if (!seg.present) {
        if (error) *error = "payload: segment missing";
        return false;
    }

    HANDLE file = CreateFileW(exePath.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        if (error) *error = "payload: cannot open exe";
        return false;
    }
    std::vector<uint8_t> compressed;
    compressed.resize(static_cast<size_t>(seg.compSize));
    const bool ok = ReadAt(file, seg.offset, compressed.data(), compressed.size());
    CloseHandle(file);
    if (!ok) {
        if (error) *error = "payload: cannot read compressed stream";
        return false;
    }

    if (!Lzma2Decode(compressed.data(), compressed.size(), seg.dictProp, *raw, seg.rawSize, error)) return false;
    if (raw->size() != seg.rawSize) {
        if (error) *error = "payload: decoded size mismatch";
        return false;
    }
    return true;
}

bool ListArchive(const std::vector<uint8_t>& raw, std::vector<ArchiveEntry>* entries, size_t* dataOffset,
                 std::string* error) {
    if (!entries || !dataOffset) return false;
    entries->clear();
    if (raw.size() < 16 || std::memcmp(raw.data(), kArchMagic, 8) != 0) {
        if (error) *error = "archive: bad magic";
        return false;
    }
    const uint32_t count = ReadU32(raw.data() + 8);
    if (count > kMaxFiles) {
        if (error) *error = "archive: too many files";
        return false;
    }
    size_t pos = 16;
    uint64_t dataBytes = 0;
    entries->reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        if (pos + 2 > raw.size()) {
            if (error) *error = "archive: truncated index";
            return false;
        }
        const uint16_t nameLen = static_cast<uint16_t>(raw[pos] | (raw[pos + 1] << 8));
        pos += 2;
        if (nameLen == 0 || pos + nameLen + 12 > raw.size()) {
            if (error) *error = "archive: truncated entry";
            return false;
        }
        ArchiveEntry entry;
        entry.name.assign(reinterpret_cast<const char*>(raw.data() + pos), nameLen);
        pos += nameLen;
        entry.size = ReadU64(raw.data() + pos);
        entry.crc  = ReadU32(raw.data() + pos + 8);
        pos += 12;
        if (!ValidEntryName(entry.name)) {
            if (error) *error = "archive: unsafe entry name";
            return false;
        }
        dataBytes += entry.size;
        entries->push_back(std::move(entry));
    }
    if (dataBytes > raw.size() - pos) {
        if (error) *error = "archive: data truncated";
        return false;
    }
    *dataOffset = pos;
    return true;
}

bool ExtractPayload(const std::wstring& exePath, const PayloadSegment& seg, const std::wstring& destDir,
                    const ExtractCallbacks& cb, std::string* error, int* filesWritten) {
    if (filesWritten) *filesWritten = 0;

    std::vector<uint8_t> raw;
    if (!DecodeArchive(exePath, seg, &raw, error)) return false;

    std::vector<ArchiveEntry> entries;
    size_t pos = 0;
    if (!ListArchive(raw, &entries, &pos, error)) return false;

    const uint64_t total = seg.rawSize;
    const int count = static_cast<int>(entries.size());
    uint64_t done = pos;

    for (int i = 0; i < count; ++i) {
        const ArchiveEntry& entry = entries[i];
        const uint8_t* data = raw.data() + pos;
        const uint32_t crc = Crc32(data, static_cast<size_t>(entry.size));
        if (crc != entry.crc) {
            if (error) *error = "archive: crc mismatch on " + entry.name;
            return false;
        }
        const std::wstring target = EntryToPath(destDir, entry.name);
        if (!EnsureDirForFile(target)) {
            if (error) *error = "install: cannot create directory for " + entry.name;
            return false;
        }
        // 只读文件（如已打包的 exe/dll）若已存在需要先清属性
        const DWORD attrs = GetFileAttributesW(target.c_str());
        if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_READONLY)) {
            SetFileAttributesW(target.c_str(), attrs & ~FILE_ATTRIBUTE_READONLY);
        }
        if (!WriteAllBytes(target, data, static_cast<size_t>(entry.size))) {
            if (error) *error = "install: cannot write " + entry.name;
            return false;
        }
        pos += static_cast<size_t>(entry.size);
        done = pos;
        if (cb.onFile && !cb.onFile(target, i + 1, count, done, total)) {
            if (error) *error = "install: cancelled";
            return false;
        }
    }
    if (filesWritten) *filesWritten = count;
    return true;
}

}  // namespace fc
