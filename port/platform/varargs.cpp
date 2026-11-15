// SPDX-License-Identifier: AGPL-3.0-or-later
// FormatSlots (port/varargs.h): the game's format language, as its formatter parses it
// (sly1-decomp src/main/text_format.cpp, ParseFormatSpec, states 0 to 5, and
// FormatConversionArg).
#include "port/varargs.h"

namespace port {

static bool IsFlag(char c) { return c == '-' || c == '+' || c == ' ' || c == '#' || c == '0'; }
static bool IsDigit(char c) { return c >= '0' && c <= '9'; }

int FormatSlots(const char* fmt, std::va_list va, Slot* slots, int max)
{
    int n = 0;
    auto put = [&](Slot s) {
        if (n < max)
            slots[n++] = s;
    };
    for (const char* p = fmt; *p;) {
        if (*p != '%') {
            p++;
            continue;
        }
        p++;
        if (*p == '%') {          // %%: no argument
            p++;
            continue;
        }
        while (IsFlag(*p))
            p++;
        if (*p == '*') {          // width from an argument
            put(SlotInt(va_arg(va, int)));
            p++;
        } else {
            if (*p == '-' || *p == '+')
                p++;
            while (IsDigit(*p))
                p++;
        }
        if (*p == '.') {
            p++;
            if (*p == '*') {      // precision from an argument
                put(SlotInt(va_arg(va, int)));
                p++;
            } else {
                if (*p == '-' || *p == '+')
                    p++;
                while (IsDigit(*p))
                    p++;
            }
        }
        int length = 0;           // 0 int, 1 short, 2 a 64-bit integer
        bool vector = false;
        switch (*p) {
        case 'h': length = 1; p++; break;
        case 'l': length = 2; p++; if (*p == 'l') p++; break;
        case 'L': case 'q': length = 2; p++; break;
        case 'v': case 'V': case 'm': case 'M': vector = true; p++; break;
        default: break;
        }
        char conv = *p;
        if (!conv)
            break;
        p++;
        if (conv >= 'A' && conv <= 'Z')
            conv = char(conv + 0x20);   // the formatter lowercases the conversion (upper case output)
        switch (conv) {
        case 'c':
            put(SlotInt(va_arg(va, int)));
            break;
        case 's':
        case 'n':
            put(SlotPtr(va_arg(va, const void*)));
            break;
        case 'p':
            put(SlotPtr(va_arg(va, const void*)));
            break;
        case 'b': case 'd': case 'i': case 'o': case 'u': case 'x':
            if (length == 2)
                put(SlotLong(va_arg(va, long long)));
            else
                put(SlotInt(va_arg(va, int)));
            break;
        case 'e': case 'f': case 'g':
            if (vector)
                put(SlotPtr(va_arg(va, const void*)));
            else
                put(SlotDouble(va_arg(va, double)));
            break;
        default:                  // an unknown conversion is copied as text: no argument
            break;
        }
    }
    return n;
}

}  // namespace port
