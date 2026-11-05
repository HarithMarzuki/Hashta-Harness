// UTF-8 <-> UTF-16 helpers. Every string that crosses the wire is UTF-8.
#pragma once

#include <windows.h>

#include <string>

inline std::wstring Widen(const std::string& utf8) {
    if (utf8.empty()) return L"";
    const int needed = MultiByteToWideChar(CP_UTF8, 0, utf8.data(),
                                           (int)utf8.size(), nullptr, 0);
    std::wstring out((size_t)needed, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), (int)utf8.size(), out.data(),
                        needed);
    return out;
}

inline std::string Narrow(const std::wstring& wide) {
    if (wide.empty()) return {};
    const int needed = WideCharToMultiByte(CP_UTF8, 0, wide.data(),
                                           (int)wide.size(), nullptr, 0,
                                           nullptr, nullptr);
    std::string out((size_t)needed, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), (int)wide.size(), out.data(),
                        needed, nullptr, nullptr);
    return out;
}
