// 3.5.0 无尽档（用户 2026-11-14）：把无尽存档点（显示 100 / 120 / 140 …）的整局状态做成可分享的文件。
// 导入后从那一层开局：武器、海克斯、属性、魂玉、MOD 道具、难度照原作者；种子、局号重新生成，之后的 Boss / 海克斯候选 / 驿站各自随机。
// 格式（UTF-8 文本，安装器 PBZTowerSetup 用同一套规则读写）：
//   pbztower-endless-1
//   author=<昵称，可空>
//   shown=<起始显示层>
//   difficulty=<0 困难 / 1 简单>
//   mod=<导出时的 Mod 版本>
//   check=<16 位十六进制 FNV-1a 64，算在 "run 文本 + \n--\n + growth 文本" 上>
//   [run]
//   ...run.txt 原文...
//   [growth]
//   ...growth.txt 原文...
//   [end]
#pragma once
#include <cstdint>
#include <format>
#include <string>
#include <string_view>

namespace tower::endlessshare {
inline constexpr std::string_view kMagic = "pbztower-endless-1";
struct Share { std::string author, mod, runText, growthText; int shown = 0, difficulty = 0; };

inline uint64_t Fnv(std::string_view s) {
    uint64_t h = 1469598103934665603ull;
    for (unsigned char c : s) { h ^= c; h *= 1099511628211ull; }
    return h;
}
inline std::string Check(const std::string& run, const std::string& growth) { return std::format("{:016x}", Fnv(run + "\n--\n" + growth)); }

// 段落文本统一成「去掉 \r、去掉空行、每行以 \n 结尾」，导出和导入算校验的是同一份文本
inline std::string Normalize(std::string_view t) {
    std::string out, line;
    for (char c : t) {
        if (c == '\r') continue;
        if (c == '\n') { if (!line.empty() && line != "[run]" && line != "[growth]" && line != "[end]") out += line + "\n"; line.clear(); continue; }
        line += c;
    }
    if (!line.empty() && line != "[run]" && line != "[growth]" && line != "[end]") out += line + "\n";
    return out;
}
inline std::string Build(const Share& s) {
    std::string author = s.author;
    for (auto& c : author) if (c == '\n' || c == '\r') c = ' ';
    const std::string run = Normalize(s.runText), growth = Normalize(s.growthText);
    std::string out = std::string(kMagic) + "\n";
    out += "author=" + author.substr(0, 40) + "\n";
    out += std::format("shown={}\ndifficulty={}\nmod={}\ncheck={}\n", s.shown, s.difficulty, s.mod, Check(run, growth));
    out += "[run]\n" + run + "[growth]\n" + growth + "[end]\n";
    return out;
}

// 只校验文件结构和校验值；局档 / 成长档的内容由各自的 Load 再严格校验
inline bool Parse(std::string text, Share* out, std::string* why) {
    auto fail = [&](const char* w) { if (why) *why = w; return false; };
    if (text.size() > 512 * 1024) return fail("无尽档文件过大");
    if (text.rfind("\xEF\xBB\xBF", 0) == 0) text.erase(0, 3);
    std::string norm; norm.reserve(text.size());
    for (char c : text) if (c != '\r') norm += c;
    size_t p = 0;
    auto next = [&](std::string* line) { if (p >= norm.size()) return false; size_t e = norm.find('\n', p); if (e == std::string::npos) e = norm.size(); *line = norm.substr(p, e - p); p = e + 1; return true; };
    std::string line, check;
    if (!next(&line) || line != kMagic) return fail("不是影塔无尽档，或格式版本不对");
    Share s;
    int section = 0;  // 0 头 1 run 2 growth 3 结束
    bool seenShown = false, seenDiff = false;
    while (next(&line)) {
        if (line == "[run]") { if (section != 0) return fail("无尽档段落顺序不对"); section = 1; continue; }
        if (line == "[growth]") { if (section != 1) return fail("无尽档段落顺序不对"); section = 2; continue; }
        if (line == "[end]") { if (section != 2) return fail("无尽档段落顺序不对"); section = 3; break; }
        if (section == 1) { if (!line.empty()) s.runText += line + "\n"; continue; }
        if (section == 2) { if (!line.empty()) s.growthText += line + "\n"; continue; }
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        try {
            if (k == "author") s.author = v.substr(0, 40);
            else if (k == "shown") { s.shown = std::stoi(v); seenShown = true; }
            else if (k == "difficulty") { s.difficulty = std::stoi(v); seenDiff = true; }
            else if (k == "mod") s.mod = v.substr(0, 20);
            else if (k == "check") check = v;
        } catch (...) { return fail("无尽档头部数值无效"); }
    }
    if (section != 3) return fail("无尽档不完整");
    if (!seenShown || s.shown < 100 || s.shown % 20 != 0 || s.shown > 100000) return fail("无尽档的起始层无效");
    if (!seenDiff || (s.difficulty != 0 && s.difficulty != 1)) return fail("无尽档的难度无效");
    if (s.runText.empty() || s.growthText.empty()) return fail("无尽档缺少局档或成长档");
    if (check != Check(s.runText, s.growthText)) return fail("无尽档校验失败（文件被改动或损坏）");
    *out = std::move(s);
    return true;
}
}  // namespace tower::endlessshare
