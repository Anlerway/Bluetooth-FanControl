#include "i18n.h"

namespace fc {
namespace {

struct Entry {
    const wchar_t* zh;
    const wchar_t* en;
};

constexpr Entry kTable[] = {
#define FC_ROW(id, zh, en) {zh, en},
    FC_STRINGS(FC_ROW)
#undef FC_ROW
};

static_assert(sizeof(kTable) / sizeof(kTable[0]) == static_cast<size_t>(Str::Count),
              "字符串表与 Str 枚举数量不一致");

bool g_chinese = true;

}  // namespace

void SetUiLanguage(bool chinese) { g_chinese = chinese; }

bool IsChineseUi() { return g_chinese; }

const wchar_t* T(Str id) {
    const size_t index = static_cast<size_t>(id);
    if (index >= sizeof(kTable) / sizeof(kTable[0])) return L"";
    return g_chinese ? kTable[index].zh : kTable[index].en;
}

}  // namespace fc
