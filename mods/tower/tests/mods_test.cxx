// 其他 Mod 检测（tower_mods.h）：在临时目录里摆一个原版布局 + 各类常见 Mod，核对只报多出来的东西
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include "../src/tower_mods.h"

static int g_checks = 0, g_fail = 0;
static void Check(bool ok, const char* what) { ++g_checks; if (!ok) { ++g_fail; std::printf("FAIL %s\n", what); } }
static void Touch(const std::filesystem::path& p) { std::filesystem::create_directories(p.parent_path()); std::ofstream(p) << "x"; }
static bool Has(const std::vector<std::string>& v, const std::string& s) { return std::find(v.begin(), v.end(), s) != v.end(); }

int main() {
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() / "pbztower_mods_test";
    fs::remove_all(root);
    const fs::path exe = root / "platform" / "steam", paks = root / "ProjectPBZ" / "Content" / "Paks";
    // 原版 + 本 Mod
    for (auto n : {"PBZ.exe", "steam_api64.dll", "tbbmalloc.dll", "avrt.dll", "PBZTower/PBZTower.dll", "D3D12/D3D12Core.dll"}) Touch(exe / n);
    for (auto n : {"ProjectPBZ-Windows.pak", "ProjectPBZ-Windows.utoc", "ProjectPBZ-Windows_s12.ucas", "global.utoc", "global.ucas", "pakchunkMovies-Windows.pak"}) Touch(paks / n);
    Check(tower::mods::Foreign(exe, paks).empty(), "vanilla layout + this mod: nothing reported");
    Check(tower::mods::Hint({}).empty(), "no hint without foreign mods");
    // 外观 / 语音资源包、UE4SS、ReShade
    Touch(paks / "~mods" / "HunOutfit_P.pak"); Touch(paks / "~mods" / "HunOutfit_P.utoc"); Touch(paks / "~mods" / "HunOutfit_P.ucas");
    Touch(paks / "VoiceCN_P.pak");
    Touch(exe / "dwmapi.dll"); Touch(exe / "ue4ss" / "UE4SS.dll"); Touch(exe / "dxgi.dll"); Touch(exe / "ReShade.ini");
    const auto f = tower::mods::Foreign(exe, paks);
    Check(Has(f, "Paks\\~mods\\HunOutfit_P.pak") && Has(f, "Paks\\~mods\\HunOutfit_P.utoc") && Has(f, "Paks\\VoiceCN_P.pak"), "pak mods listed (ucas not repeated)");
    Check(!Has(f, "Paks\\~mods\\HunOutfit_P.ucas"), "ucas twin not listed");
    Check(Has(f, "dwmapi.dll") && Has(f, "dxgi.dll") && Has(f, "ue4ss\\") && Has(f, "ReShade.ini"), "loader dlls / folders listed");
    Check(!Has(f, "avrt.dll") && !Has(f, "steam_api64.dll"), "own proxy and vanilla dlls not listed");
    const auto h = tower::mods::Hint(f);
    Check(h.find("检测到其他 Mod") == 0 && h.find("等 ") != std::string::npos, "hint lists first 3 and counts the rest");
    fs::remove_all(root);
    if (g_fail) { std::printf("TOWER_MODS_FAILED %d/%d\n", g_fail, g_checks); return 1; }
    std::printf("TOWER_MODS_OK %d checks (temp dirs, no game process)\n", g_checks);
    return 0;
}
