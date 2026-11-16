// Portable seed contract. This is a new-run recipe, never a progress save.
#pragma once
#include <charconv>
#include <cstdint>
#include <map>
#include <sstream>
#include <string>
#include <string_view>

namespace tower::seedshare {
inline constexpr std::string_view kVersion = "3.5.0";  // 只作记录，导入不再要求一致
inline constexpr int kPlan = 14;
struct Seed { uint64_t value = 0; int difficulty = 0; int shuffle = 0; };  // shuffle 0 = 没写（按当前新局规则）
inline bool Parse(std::string_view text, Seed* result) {
    if (!result || text.empty() || text.size() > 512) return false;
    std::istringstream in{std::string(text)};
    std::map<std::string, std::string> fields;
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const auto p = line.find('=');
        if (p == std::string::npos || p == 0 || !fields.emplace(line.substr(0, p), line.substr(p + 1)).second) return false;
    }
    // 3.1.0（玩家反馈：种子导不出 / 导入无效）：原来要求 mod 版本等于写死的 2.2.0，3.0.0 安装器写的是 3.0.0，导入永远被拒。
    // 现在只要求方案版本（plan）一致；mod 只作记录。格式 2 多一个 shuffle（Boss 排序方案），导出局用什么顺序，导入就用什么顺序
    const bool v2 = fields["format"] == "pbztower-seed-2";
    if ((v2 ? fields.size() != 6 : fields.size() != 5) || (!v2 && fields["format"] != "pbztower-seed-1") || fields["mod"].empty() || fields["mod"].size() > 32 ||
        fields["plan"] != std::to_string(kPlan) || (fields["difficulty"] != "easy" && fields["difficulty"] != "hard")) return false;
    int shuffle = 0;
    if (v2) { const auto& s = fields["shuffle"]; if (s != "1" && s != "2" && s != "3" && s != "4") return false; shuffle = s[0] - '0'; }
    const auto& v = fields["seed"];
    if (v.empty() || v.size() > 20 || v.find_first_not_of("0123456789") != std::string::npos) return false;
    uint64_t n = 0; const auto parsed = std::from_chars(v.data(), v.data() + v.size(), n);
    if (parsed.ec != std::errc{} || parsed.ptr != v.data() + v.size() || !n) return false;
    *result = {n, fields["difficulty"] == "easy" ? 1 : 0, shuffle};
    return true;
}
}
