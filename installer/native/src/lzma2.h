#pragma once
#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

namespace fc {

// 解码一整段 raw LZMA2 流（xz 容器去掉头部/尾部校验后的纯 LZMA2 数据）。
// src/srcLen      : 压缩数据
// dictProp        : LZMA2 属性字节，字典大小 = (2 | (dictProp & 1)) << (dictProp / 2 + 11)
// out             : 解压结果（函数内部按 expectedSize 预留）
// expectedSize    : 期望解压字节数；0 表示未知（此时按流结束控制字符判断）
// 返回 true 且 out.size() == expectedSize 视为成功；失败返回 false，*error 填原因
bool Lzma2Decode(const uint8_t* src, size_t srcLen, uint8_t dictProp,
                 std::vector<uint8_t>& out, uint64_t expectedSize, std::string* error);

}  // namespace fc
