#include "theme.h"

#include <winreg.h>

namespace fc {

D2D1_COLOR_F Col(unsigned rgb, float alpha) {
    return D2D1::ColorF(((rgb >> 16) & 0xFF) / 255.0f, ((rgb >> 8) & 0xFF) / 255.0f, (rgb & 0xFF) / 255.0f, alpha);
}

Theme MakeTheme(bool dark) {
    Theme t{};
    if (dark) {
        t.windowBg     = Col(0x202020);
        t.cardBg       = Col(0x2C2C2C);
        t.cardBorder   = Col(0x3A3A3A);
        t.divider      = Col(0x383838);
        t.textPrimary  = Col(0xFFFFFF);
        t.textSecondary = Col(0xB9B9B9);
        t.textTertiary = Col(0x8F8F8F);
        t.accent       = Col(0x60CDFF);
        t.accentText   = Col(0x000000);
        t.accentHover  = Col(0x8BDCFF);
        t.controlBg    = Col(0x333333);
        t.controlBorder = Col(0x4A4A4A);
        t.controlHover = Col(0x3A3A3A);
        t.track        = Col(0x3F3F3F);
        t.danger       = Col(0xFF99A4);
        t.dangerHover  = Col(0xFFB4BC);
        t.dangerText   = Col(0x000000);
        t.ok           = Col(0x6CCB5F);
    } else {
        t.windowBg     = Col(0xF3F3F3);
        t.cardBg       = Col(0xFBFBFB);
        t.cardBorder   = Col(0xE9E9E9);
        t.divider      = Col(0xEDEDED);
        t.textPrimary  = Col(0x1B1B1B);
        t.textSecondary = Col(0x5D5D5D);
        t.textTertiary = Col(0x8A8A8A);
        t.accent       = Col(0x005FB8);
        t.accentText   = Col(0xFFFFFF);
        t.accentHover  = Col(0x1A74C8);
        t.controlBg    = Col(0xFFFFFF);
        t.controlBorder = Col(0xD9D9D9);
        t.controlHover = Col(0xF5F5F5);
        t.track        = Col(0xE2E2E2);
        t.danger       = Col(0xC42B1C);
        t.dangerHover  = Col(0xA51F12);
        t.dangerText   = Col(0xFFFFFF);
        t.ok           = Col(0x0F7B0F);
    }
    return t;
}

bool SystemUsesLightTheme() {
    DWORD value = 1;
    DWORD size = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                     L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &value, &size) == ERROR_SUCCESS) {
        return value != 0;
    }
    return false;
}

bool Fonts::Create(IDWriteFactory* dw) {
    if (!dw) return false;
    auto make = [&](const wchar_t* family, DWRITE_FONT_WEIGHT weight, float size, IDWriteTextFormat** out) {
        if (FAILED(dw->CreateTextFormat(family, nullptr, weight, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                        size, L"", out))) {
            return false;
        }
        (*out)->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        return true;
    };
    return make(L"Segoe UI Variable Display", DWRITE_FONT_WEIGHT_SEMI_BOLD, 24.0f, &display24) &&
           make(L"Segoe UI Variable Display", DWRITE_FONT_WEIGHT_SEMI_BOLD, 20.0f, &title20) &&
           make(L"Segoe UI Variable Text", DWRITE_FONT_WEIGHT_SEMI_BOLD, 16.0f, &h16) &&
           make(L"Segoe UI Variable Text", DWRITE_FONT_WEIGHT_NORMAL, 14.0f, &body14) &&
           make(L"Segoe UI Variable Text", DWRITE_FONT_WEIGHT_SEMI_BOLD, 14.0f, &strong14) &&
           make(L"Segoe UI Variable Text", DWRITE_FONT_WEIGHT_NORMAL, 12.0f, &small12) &&
           make(L"Segoe Fluent Icons", DWRITE_FONT_WEIGHT_NORMAL, 16.0f, &glyph);
}

void Fonts::Release() {
    IDWriteTextFormat* Fonts::*members[] = {&Fonts::display24, &Fonts::title20, &Fonts::h16, &Fonts::body14,
                                            &Fonts::strong14, &Fonts::small12, &Fonts::glyph};
    for (IDWriteTextFormat* Fonts::*member : members) {
        IDWriteTextFormat*& f = this->*member;
        if (f) {
            f->Release();
            f = nullptr;
        }
    }
}

}  // namespace fc
