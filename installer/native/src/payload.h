#pragma once
// 安装器负载：解析追加在 setup.exe 尾部的 footer，解压 raw LZMA2 归档流并落盘。
// 容器格式见 installer/pack.py 顶部注释。
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace fc {

struct PayloadSegment {
    bool     present  = false;
    uint64_t offset   = 0;   // 压缩流在 exe 中的起始偏移
    uint64_t compSize = 0;
    uint64_t rawSize  = 0;
    uint8_t  dictProp = 0;
};

struct PayloadSet {
    uint64_t       exeSize = 0;        // 不含负载的 exe 长度
    PayloadSegment seg[2];             // 0 = full(selfcontained)，1 = slim(framework)

    // 读取 exe 尾部 footer。没有负载时返回 true 且 present 全为 false。
    static bool Load(const std::wstring& exePath, PayloadSet* out, std::string* error);

    // 选择要安装的负载：优先 slim，缺失时退回 full。
    const PayloadSegment* Select(bool preferSlim) const;
};

struct ArchiveEntry {
    std::string name;   // UTF-8，'/' 分隔
    uint64_t    size = 0;
    uint32_t    crc  = 0;
};

struct ExtractCallbacks {
    // 每个文件写完回调一次；返回 false 中止安装。
    std::function<bool(const std::wstring& relName, int index, int count, uint64_t doneBytes, uint64_t totalBytes)> onFile;
};

// 只解压不写盘（自测/调试用）。raw 输出整段归档流。
bool DecodeArchive(const std::wstring& exePath, const PayloadSegment& seg, std::vector<uint8_t>* raw, std::string* error);

// 解析归档头；dataOffset 为文件数据起始位置。
bool ListArchive(const std::vector<uint8_t>& raw, std::vector<ArchiveEntry>* entries, size_t* dataOffset, std::string* error);

// 解压并写入 destDir（目录不存在会自动创建），逐个文件校验 CRC32。
bool ExtractPayload(const std::wstring& exePath, const PayloadSegment& seg, const std::wstring& destDir,
                    const ExtractCallbacks& cb, std::string* error, int* filesWritten = nullptr);

}  // namespace fc
