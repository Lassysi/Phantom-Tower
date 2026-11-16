#pragma once
#include <array>
#include <string>
#include <string_view>
#include <vector>

namespace tower::menutext {
inline std::vector<std::string> Lines(std::string_view s) {
    std::vector<std::string> out;
    for (size_t p=0;p<s.size();) { const auto e=s.find('\n',p);out.emplace_back(s.substr(p,e==s.npos?s.size()-p:e-p));if(e==s.npos)break;p=e+1; }
    return out;
}
// 当前旧DLL的准备页适配：不改变按钮或任何游戏状态。
inline std::string CompactReady(std::string text) {
    const auto head=text.find('\n');
    if(head==text.npos || !text.substr(0,head).starts_with("影塔·百战 · 第 ") || text.substr(0,head).find(" · 信息")!=text.npos) return text;
    const auto start=text.find("\n第 ");
    if(start==text.npos || text.find("魂玉 ",start)==text.npos || text.find("本局加成",start)==text.npos) return text;
    std::string floor,jade,hp,qi,attack,sha,temper,cultivate,soul,prep;
    std::vector<std::string> bonus;
    for(const auto& line:Lines(std::string_view(text).substr(start+1))) {
        if(line.starts_with("第 ")) floor=line;
        else if(line.starts_with("魂玉 "))jade=line;
        else if(line.starts_with("生命 "))hp=line;
        else if(line.starts_with("杀气 "))qi=line;
        else if(line.starts_with("攻击 "))attack=line;
        else if(line.starts_with("杀气攻击 "))sha=line;
        else if(line.starts_with("淬炼点 "))temper=line;
        else if(line.starts_with("修为点 "))cultivate=line;
        else if(line.starts_with("铸魂令 "))soul=line;
        else if(line.starts_with("下层预装"))prep=line;
        else if(line.starts_with("　") && line.find("+0%")==line.npos) bonus.push_back(line.substr(std::string("　").size()));
    }
    if(floor.empty() || jade.empty() || hp.empty() || qi.empty() || attack.empty() || sha.empty())return text;
    std::string out=text.substr(0,head)+"\n\n"+floor+"　"+jade+"\n"+hp+"　"+qi+"\n"+attack+"　"+sha;
    if(!temper.empty())out+="\n"+temper+"　"+cultivate+"　"+soul;
    if(!bonus.empty()){out+="\n\n";for(size_t i=0;i<bonus.size();++i)out+=(i?"　":"")+bonus[i];}
    if(!prep.empty())out+="\n"+prep;
    return out;
}

// 结算首屏只保留结果与失败Boss，避免长快照/逐层时间挤掉退出按钮。
// 同时支持源码的body与旧DLL中已包含标题的RichText；不碰其他原生对话框。
inline std::string CompactSettlement(const std::string& text) {
    const auto lines=Lines(text);
    if(lines.empty()) return text;
    const bool titled=lines.front().starts_with("影塔·百战 · ") &&
        (lines.front().find("止步第 ")!=std::string::npos || lines.front().find("通关")!=std::string::npos);
    const bool summit=lines.front().starts_with("第 100 层：");  // 隐藏层登顶：保留那一句
    if(!titled && !summit && !lines.front().starts_with("结果：")) return text;
    std::string result,boss;
    for(const auto& line:lines) {
        if(line.starts_with("结果：")) result=line;
        else if(line.starts_with("失败 Boss：")) boss=line;
    }
    if(result.empty()) return text;
    std::string out=titled||summit?lines.front()+"\n\n":std::string();
    out+=result;
    if(!boss.empty())out+="\n"+boss;
    out+="\n\n点击「退出影塔」恢复原进度。";
    return out;
}
// 准备页属性网格（用户 2026-11-11：名称与数值分列对齐）。前一半行在左、后一半在右；
// 每行在第一个空格处拆成名称 / 数值，没有空格的整行当名称。返回 左名、左值、右名、右值 四列文本
// 属性行 =「名称 数值」：名称至少两个汉字（UTF-8 ≥ 6 字节），整行不含中文逗号 / 句号。
// 其余（如「第 16 层已经领过奖励，这次只有魂玉 +46」）是提示句，不能拆进两列表格（用户 2026-11-12：拆进去把表格撑到黑边）
inline bool IsStatLine(const std::string& l) {
    const auto sp = l.find(' ');
    if (sp == std::string::npos || sp < 6 || sp > 30) return false;
    return l.find("，") == std::string::npos && l.find("。") == std::string::npos;
}
inline std::array<std::string, 4> StatsGrid(const std::vector<std::string>& lines) {
    std::array<std::string, 4> out;
    const size_t split = (lines.size() + 1) / 2;
    for (size_t i = 0; i < lines.size(); ++i) {
        const size_t base = i < split ? 0 : 2;
        const auto& l = lines[i];
        const auto sp = l.find(' ');
        const std::string name = sp == std::string::npos ? l : l.substr(0, sp);
        std::string value = sp == std::string::npos ? std::string() : l.substr(sp + 1);
        while (!value.empty() && value.front() == ' ') value.erase(value.begin());
        if (i != 0 && i != split) { out[base] += "\n"; out[base + 1] += "\n"; }  // 半边第一行前不加换行（名称可能为空也保持行对齐）
        out[base] += name; out[base + 1] += value;
    }
    return out;
}
} // namespace tower::menutext
