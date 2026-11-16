// 1.1.1（用户 2026-11-07：很多玩家"第 N 层出了问题，已跳过"，有人装了外观 / 语音等其他 Mod）：
// 扫描游戏目录里"原版游戏和本 Mod 都没有"的东西——额外的资源包（.pak / .utoc / .ucas，含 ~mods / LogicMods 子目录）、
// PBZ.exe 旁边多出来的 DLL（dinput8 / dxgi / version / UE4SS 之类的加载器）和常见 Mod 加载器目录。只读文件名，不读内容。
// 结果写日志，并附在坏层跳过的提示里，玩家截图时能直接看到可能冲突的 Mod。纯逻辑（给目录就能测），不依赖游戏进程。
#pragma once
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

namespace tower::mods {

inline std::string Lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}
// 原版资源包：ProjectPBZ-Windows（含 _sN 分片）、global、pakchunk*（过场影片分包）
inline bool VanillaPak(const std::string& lowerName) {
    return lowerName.rfind("projectpbz-windows", 0) == 0 || lowerName.rfind("global.", 0) == 0 || lowerName.rfind("pakchunk", 0) == 0;
}
// PBZ.exe 旁边原版就有的 DLL，以及本 Mod 的代理
inline bool KnownExeDll(const std::string& lowerName) {
    return lowerName == "steam_api64.dll" || lowerName == "tbbmalloc.dll" || lowerName == "avrt.dll";
}
inline std::string Utf8(const std::filesystem::path& p) {
    const auto u = p.u8string();
    return std::string(u.begin(), u.end());
}

// exeDir = PBZ.exe 所在目录（<游戏根>\platform\steam）；paksDir = <游戏根>\ProjectPBZ\Content\Paks。
// 返回给玩家看的条目（文件 / 目录名，资源包带子目录前缀），最多 maxItems 个
inline std::vector<std::string> Foreign(const std::filesystem::path& exeDir, const std::filesystem::path& paksDir, size_t maxItems = 32) {
    namespace fs = std::filesystem;
    std::vector<std::string> out;
    std::error_code ec;
    auto add = [&](std::string s) { if (out.size() < maxItems && std::find(out.begin(), out.end(), s) == out.end()) out.push_back(std::move(s)); };
    if (fs::is_directory(paksDir, ec)) {
        for (fs::recursive_directory_iterator it(paksDir, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec)) {
            if (it.depth() > 3) { it.disable_recursion_pending(); continue; }
            if (!it->is_regular_file(ec)) continue;
            const auto name = Lower(Utf8(it->path().filename()));
            const auto ext = Lower(Utf8(it->path().extension()));
            if (ext != ".pak" && ext != ".utoc" && ext != ".ucas") continue;
            if (it.depth() == 0 && VanillaPak(name)) continue;
            if (ext != ".pak" && ext != ".utoc") continue;  // 同名的 .ucas 不重复列
            auto rel = Utf8(fs::relative(it->path(), paksDir, ec));
            if (ec) { rel = Utf8(it->path().filename()); ec.clear(); }
            add("Paks\\" + rel);
        }
        ec.clear();
    }
    if (fs::is_directory(exeDir, ec)) {
        for (fs::directory_iterator it(exeDir, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec)) {
            const auto name = Lower(Utf8(it->path().filename()));
            if (it->is_directory(ec)) {
                if (name == "ue4ss" || name == "mods" || name == "reshade-shaders" || name == "plugins" || name == "scripts") add(Utf8(it->path().filename()) + "\\");
                continue;
            }
            if (Lower(Utf8(it->path().extension())) == ".dll" && !KnownExeDll(name)) add(Utf8(it->path().filename()));
            if (name == "dxgi.ini" || name == "reshade.ini" || name == "ue4ss-settings.ini") add(Utf8(it->path().filename()));
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

// 坏层提示里的一句话：最多列 3 个，其余写"等 N 个"
inline std::string Hint(const std::vector<std::string>& found) {
    if (found.empty()) return {};
    std::string s = "检测到其他 Mod：";
    for (size_t i = 0; i < found.size() && i < 3; ++i) s += (i ? "、" : "") + found[i];
    if (found.size() > 3) s += "等 " + std::to_string(found.size()) + " 个";
    return s + "，可能冲突；可先移出这些 Mod 再试";
}

}  // namespace tower::mods
