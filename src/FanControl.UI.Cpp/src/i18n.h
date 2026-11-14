#pragma once

// 极简运行时双语表：key 是中文原文，英文在表里。
// 未登记的字符串原样返回，所以漏翻只会显示中文，不会显示占位符。

#include <string>

namespace fc {

enum class Lang { Zh, En };

void SetLang(Lang lang);
Lang GetLang();
bool IsEn();

// 传入中文原文，返回当前语言下的文本（静态字符串，生命周期为整个进程）。
const wchar_t* T(const wchar_t* zh);

// 后台错误码 → 当前语言文案（{0}/{1} 用 arg1/arg2 替换）。
// code 未登记时返回 fallback，也就是后台随帧带来的中文兜底文案。
std::wstring ErrorText(const std::wstring& code, const std::wstring& arg1, const std::wstring& arg2,
                       const std::wstring& fallback);

} // namespace fc
