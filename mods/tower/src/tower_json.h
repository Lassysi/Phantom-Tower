// 最小 JSON（游戏存档 JSON 读改写用）：保留对象键顺序、数字按原文保存（整数 / 小数写回不变形），
// 只实现游戏存档里出现的语法。纯逻辑，离线可测。
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace tower::json {

struct Value {
    enum Kind { Null, Bool, Number, String, Array, Object } kind = Null;
    bool b = false;
    std::string text;  // Number：原文；String：已解码的 UTF-8
    std::vector<Value> items;
    std::vector<std::pair<std::string, Value>> fields;

    static Value Str(std::string s) { Value v; v.kind = String; v.text = std::move(s); return v; }
    static Value Num(long long n) { Value v; v.kind = Number; v.text = std::to_string(n); return v; }
    static Value Boolean(bool x) { Value v; v.kind = Bool; v.b = x; return v; }
    static Value Obj() { Value v; v.kind = Object; return v; }
    static Value Arr() { Value v; v.kind = Array; return v; }

    Value* Find(std::string_view k) {
        for (auto& [key, v] : fields) if (key == k) return &v;
        return nullptr;
    }
    const Value* Find(std::string_view k) const {
        for (auto& [key, v] : fields) if (key == k) return &v;
        return nullptr;
    }
    Value& Set(std::string_view k, Value v) {  // 已有就替换（保持原位置），没有就追加
        if (auto* x = Find(k)) { *x = std::move(v); return *x; }
        fields.emplace_back(std::string(k), std::move(v));
        return fields.back().second;
    }
    void Erase(std::string_view k) { std::erase_if(fields, [&](auto& f) { return f.first == k; }); }
    long long Int(long long def = 0) const {
        if (kind == Bool) return b;
        if (kind != Number || text.empty()) return def;
        long long v = 0; bool neg = text[0] == '-';
        for (size_t i = neg; i < text.size() && text[i] >= '0' && text[i] <= '9'; ++i) v = v * 10 + (text[i] - '0');
        return neg ? -v : v;
    }
};

namespace detail {
struct Parser {
    std::string_view s;
    size_t i = 0;
    bool ok = true;
    void Ws() { while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) ++i; }
    bool Eat(char c) { Ws(); if (i < s.size() && s[i] == c) { ++i; return true; } return false; }
    static void Utf8(std::string& o, uint32_t cp) {
        if (cp < 0x80) o += static_cast<char>(cp);
        else if (cp < 0x800) { o += static_cast<char>(0xC0 | (cp >> 6)); o += static_cast<char>(0x80 | (cp & 0x3F)); }
        else if (cp < 0x10000) { o += static_cast<char>(0xE0 | (cp >> 12)); o += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)); o += static_cast<char>(0x80 | (cp & 0x3F)); }
        else { o += static_cast<char>(0xF0 | (cp >> 18)); o += static_cast<char>(0x80 | ((cp >> 12) & 0x3F)); o += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)); o += static_cast<char>(0x80 | (cp & 0x3F)); }
    }
    uint32_t Hex4() {
        uint32_t v = 0;
        for (int k = 0; k < 4; ++k, ++i) {
            if (i >= s.size()) { ok = false; return 0; }
            const char c = s[i];
            v = v * 16 + (c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : (ok = false, 0));
        }
        return v;
    }
    std::string Str() {
        std::string o;
        if (!Eat('"')) { ok = false; return o; }
        while (i < s.size() && s[i] != '"') {
            if (s[i] != '\\') { o += s[i++]; continue; }
            if (++i >= s.size()) { ok = false; break; }
            const char e = s[i++];
            switch (e) {
                case 'n': o += '\n'; break; case 't': o += '\t'; break; case 'r': o += '\r'; break;
                case 'b': o += '\b'; break; case 'f': o += '\f'; break;
                case 'u': {
                    uint32_t cp = Hex4();
                    if (cp >= 0xD800 && cp < 0xDC00 && i + 1 < s.size() && s[i] == '\\' && s[i + 1] == 'u') { i += 2; cp = 0x10000 + ((cp - 0xD800) << 10) + (Hex4() - 0xDC00); }
                    Utf8(o, cp);
                    break;
                }
                default: o += e;
            }
        }
        if (i >= s.size()) ok = false; else ++i;
        return o;
    }
    Value Parse(int depth = 0) {
        Value v;
        Ws();
        if (i >= s.size() || depth > 200) { ok = false; return v; }
        const char c = s[i];
        if (c == '{') {
            ++i; v.kind = Value::Object;
            if (Eat('}')) return v;
            do { Ws(); std::string k = Str(); if (!Eat(':')) { ok = false; return v; } v.fields.emplace_back(std::move(k), Parse(depth + 1)); } while (ok && Eat(','));
            if (!Eat('}')) ok = false;
        } else if (c == '[') {
            ++i; v.kind = Value::Array;
            if (Eat(']')) return v;
            do v.items.push_back(Parse(depth + 1)); while (ok && Eat(','));
            if (!Eat(']')) ok = false;
        } else if (c == '"') { v.kind = Value::String; v.text = Str(); }
        else if (s.substr(i, 4) == "true") { v.kind = Value::Bool; v.b = true; i += 4; }
        else if (s.substr(i, 5) == "false") { v.kind = Value::Bool; i += 5; }
        else if (s.substr(i, 4) == "null") { i += 4; }
        else {
            const size_t a = i;
            while (i < s.size() && (s[i] == '-' || s[i] == '+' || s[i] == '.' || s[i] == 'e' || s[i] == 'E' || (s[i] >= '0' && s[i] <= '9'))) ++i;
            if (a == i) { ok = false; return v; }
            v.kind = Value::Number; v.text = std::string(s.substr(a, i - a));
        }
        return v;
    }
};
inline void Esc(std::string& o, const std::string& t) {
    o += '"';
    for (unsigned char c : t) {
        switch (c) {
            case '"': o += "\\\""; break; case '\\': o += "\\\\"; break;
            case '\n': o += "\\n"; break; case '\r': o += "\\r"; break; case '\t': o += "\\t"; break;
            default:
                if (c < 0x20) { static const char* h = "0123456789abcdef"; o += "\\u00"; o += h[c >> 4]; o += h[c & 15]; }
                else o += static_cast<char>(c);
        }
    }
    o += '"';
}
inline void Write(std::string& o, const Value& v) {
    switch (v.kind) {
        case Value::Null: o += "null"; break;
        case Value::Bool: o += v.b ? "true" : "false"; break;
        case Value::Number: o += v.text; break;
        case Value::String: Esc(o, v.text); break;
        case Value::Array:
            o += '[';
            for (size_t k = 0; k < v.items.size(); ++k) { if (k) o += ','; Write(o, v.items[k]); }
            o += ']';
            break;
        case Value::Object:
            o += '{';
            for (size_t k = 0; k < v.fields.size(); ++k) { if (k) o += ','; Esc(o, v.fields[k].first); o += ':'; Write(o, v.fields[k].second); }
            o += '}';
            break;
    }
}
}  // namespace detail

inline bool Parse(std::string_view text, Value* out) {
    detail::Parser p{text};
    Value v = p.Parse();
    p.Ws();
    if (!p.ok || p.i != text.size()) return false;
    *out = std::move(v);
    return true;
}
inline std::string Dump(const Value& v) { std::string o; detail::Write(o, v); return o; }

}  // namespace tower::json
