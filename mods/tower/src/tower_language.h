#pragma once

#include <string_view>

// Language is a UI preference only. Never embed it in a run or game save.
namespace tower::language {
enum class Language { Chinese, English };

// Windows primary language ID: 0x04 is Chinese (all regional variants).
// Other UI languages use English; an explicit saved choice always takes priority.
inline Language DefaultForWindowsLanguage(unsigned short id) {
    return (id & 0x03ffu) == 0x04u ? Language::Chinese : Language::English;
}

inline Language Parse(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t' || text.front() == '\r' || text.front() == '\n')) text.remove_prefix(1);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r' || text.back() == '\n')) text.remove_suffix(1);
    return text == "en" ? Language::English : Language::Chinese;
}
inline std::string_view Encode(Language value) { return value == Language::English ? "en\n" : "zh\n"; }

// Reads once per process. A missing/unreadable preference uses the Windows UI language.
Language Current();
// Updates the in-memory preference only after the on-disk preference is saved.
bool Set(Language value);
}  // namespace tower::language
