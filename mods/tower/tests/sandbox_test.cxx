// 影塔存档沙盒（文件层）离线测试：把 tower_sandbox.cpp 编进来，钩子换成直接调用改道函数，在临时目录里真读真写。
// 覆盖游戏可能用到的每条写入通道（写、新建、删、改名、替换、三种复制、CreateFile2、按句柄改名），以及"基准没放回不许关沙盒"。
#define PBZ_MODULE
#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>

#include "../src/tower_sandbox.cpp"

namespace pbz::log { void Write(const std::string&) {} }
namespace pbz::hooks {
bool Create(const char*, void* target, void*, void** original) { *original = target; return true; }
bool Enable(const char*, bool) { return true; }
}

namespace towersb {
bool Set(bool on, std::string* why, bool force);
std::string State();
int Session(); bool Tainted(); void BeginReal(); void EndReal();
void SetRootsForTest(const std::wstring& root, const std::wstring& box);
}

static int g_checks = 0;
#define CHECK(c) do { ++g_checks; if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); std::exit(1); } } while (0)

namespace fs = std::filesystem;
static std::string ReadAll(const fs::path& p) { std::ifstream f(p, std::ios::binary); std::stringstream s; s << f.rdbuf(); return s.str(); }
static void WriteAll(const fs::path& p, const std::string& s) { fs::create_directories(p.parent_path()); std::ofstream(p, std::ios::binary | std::ios::trunc) << s; }
static void WriteVia(const fs::path& p, const std::string& s, DWORD disp = CREATE_ALWAYS) {
    HANDLE h = Create(p.c_str(), GENERIC_WRITE, 0, nullptr, disp, FILE_ATTRIBUTE_NORMAL, nullptr);
    CHECK(h != INVALID_HANDLE_VALUE);
    DWORD n = 0; WriteFile(h, s.data(), static_cast<DWORD>(s.size()), &n, nullptr); CloseHandle(h);
}
static std::string ReadVia(const fs::path& p) {
    HANDLE h = Create(p.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return "<none>";
    char buf[256]; DWORD n = 0; ReadFile(h, buf, sizeof buf, &n, nullptr); CloseHandle(h);
    return std::string(buf, n);
}

int main() {
    wchar_t tmp[MAX_PATH]; GetTempPathW(MAX_PATH, tmp);
    const fs::path base = fs::path(tmp) / L"pbz-sandbox-test";
    fs::remove_all(base);
    const fs::path real = base / L"Saved" / L"SaveGames", box = base / L"Box" / L"SaveGames";
    fs::create_directories(real); fs::create_directories(box);
    towersb::SetRootsForTest(real.wstring() + L"\\", box.wstring() + L"\\");
    const fs::path main = real / L"Steam" / L"1" / L"PBZSaveGame.sav", snap = real / L"Steam" / L"1" / L"1RealTime" / L"a.sav";
    WriteAll(main, "REAL"); WriteAll(snap, "REALSNAP");
    std::string why;

    CHECK(towersb::Set(true, &why, false) && towersb::Set(false, &why, false));  // 第一次开会挂钩（夹具里只记下原函数）
    // 关着：原样放行
    CHECK(ReadVia(main) == "REAL");
    CHECK(towersb::Set(true, &why, false));

    // 写已有存档：进沙盒，原档不变；读回读到沙盒的
    WriteVia(main, "TEST1");
    CHECK(ReadAll(main) == "REAL" && ReadAll(box / L"Steam" / L"1" / L"PBZSaveGame.sav") == "TEST1" && ReadVia(main) == "TEST1");
    // 只读打开、沙盒里没有：读原档
    CHECK(ReadVia(snap) == "REALSNAP");
    // 新建快照
    const fs::path snap2 = real / L"Steam" / L"1" / L"1RealTime" / L"b.sav";
    WriteVia(snap2, "NEWSNAP", CREATE_NEW);
    CHECK(!fs::exists(snap2) && ReadVia(snap2) == "NEWSNAP");
    // 删除：原档不删
    CHECK(Delete(snap.c_str()) && fs::exists(snap));
    // 改名（临时文件 → 主存档，游戏写存档的常见做法）
    const fs::path tmpSave = real / L"Steam" / L"1" / L"PBZSaveGame.tmp";
    WriteVia(tmpSave, "TEST2");
    CHECK(Move(tmpSave.c_str(), main.c_str(), MOVEFILE_REPLACE_EXISTING) && ReadAll(main) == "REAL" && ReadVia(main) == "TEST2");
    CHECK(MoveP(snap2.c_str(), (real / L"Steam" / L"1" / L"c.sav").c_str(), nullptr, nullptr, 0) && !fs::exists(real / L"Steam" / L"1" / L"c.sav"));
    // ReplaceFileW
    WriteVia(tmpSave, "TEST3");
    CHECK(Replace(main.c_str(), tmpSave.c_str(), nullptr, 0, nullptr, nullptr) && ReadAll(main) == "REAL" && ReadVia(main) == "TEST3");
    // 三种复制：目标在存档目录 → 进沙盒（快照轮换常用复制）
    const fs::path outside = base / L"elsewhere.sav";
    WriteAll(outside, "OUTSIDE");
    const fs::path cp1 = real / L"Steam" / L"1" / L"2Hourly" / L"h1.sav", cp2 = real / L"Steam" / L"1" / L"2Hourly" / L"h2.sav", cp3 = real / L"Steam" / L"1" / L"2Hourly" / L"h3.sav";
    CHECK(Copy(main.c_str(), cp1.c_str(), FALSE) && !fs::exists(cp1) && ReadVia(cp1) == "TEST3");  // 源也从沙盒取
    CHECK(CopyEx(outside.c_str(), cp2.c_str(), nullptr, nullptr, nullptr, 0) && !fs::exists(cp2) && ReadVia(cp2) == "OUTSIDE");
    CHECK(SUCCEEDED(Copy2(main.c_str(), cp3.c_str(), nullptr)) && !fs::exists(cp3) && ReadVia(cp3) == "TEST3");
    // 从存档目录复制出去：读沙盒里的，原档不动
    const fs::path out2 = base / L"export.sav";
    CHECK(Copy(main.c_str(), out2.c_str(), FALSE) && ReadAll(out2) == "TEST3" && ReadAll(main) == "REAL");
    // CreateFile2 写
    {
        HANDLE h = Create2(main.c_str(), GENERIC_WRITE, 0, CREATE_ALWAYS, nullptr);
        CHECK(h != INVALID_HANDLE_VALUE);
        DWORD n = 0; WriteFile(h, "TEST4", 5, &n, nullptr); CloseHandle(h);
        CHECK(ReadAll(main) == "REAL" && ReadVia(main) == "TEST4");
        HANDLE r = Create2(main.c_str(), GENERIC_READ, FILE_SHARE_READ, OPEN_EXISTING, nullptr);
        char b[16]; DWORD m = 0; ReadFile(r, b, sizeof b, &m, nullptr); CloseHandle(r);
        CHECK(std::string(b, m) == "TEST4");
    }
    // 按句柄改名：沙盒里打开的临时文件改名到真目录 → 必须还落在沙盒（否则测试档被搬进真档）
    {
        WriteVia(tmpSave, "TEST5");
        HANDLE h = Create(tmpSave.c_str(), DELETE | GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        CHECK(h != INVALID_HANDLE_VALUE);
        const std::wstring target = main.wstring();
        std::vector<uint8_t> buf(sizeof(FILE_RENAME_INFO) + target.size() * sizeof(wchar_t), 0);
        auto* ri = reinterpret_cast<FILE_RENAME_INFO*>(buf.data());
        ri->ReplaceIfExists = TRUE;
        ri->FileNameLength = static_cast<DWORD>(target.size() * sizeof(wchar_t));
        std::memcpy(ri->FileName, target.c_str(), ri->FileNameLength);
        CHECK(SetInfo(h, FileRenameInfo, ri, static_cast<DWORD>(buf.size())));
        CloseHandle(h);
        CHECK(ReadAll(main) == "REAL" && ReadVia(main) == "TEST5");
    }
    // 存档目录外的文件不受影响
    WriteVia(base / L"other.txt", "X");
    CHECK(ReadAll(base / L"other.txt") == "X");

    // 绕开改道读真档（备份真档用）：沙盒开着也读到磁盘上的原档
    towersb::BeginReal();
    CHECK(ReadVia(main) == "REAL");
    towersb::EndReal();
    CHECK(ReadVia(main) == "TEST5");

    // 基准没放回（内存是塔内状态）：拒绝关沙盒；强制可以关；放回后可以关
    towersb::SetDirty(true);
    CHECK(!towersb::Set(false, &why, false) && towersb::Active() && !why.empty());
    towersb::SetDirty(false);
    CHECK(towersb::Set(false, &why, false) && !towersb::Active());
    // 关了：原样放行（读真档）
    CHECK(ReadVia(main) == "REAL");
    // 新一轮：会话号 +1、上一轮影子档归档、从空影子目录开始（不会读到上一轮塔内写的档）、一开始不脏
    const int s1 = towersb::Session();
    CHECK(towersb::Set(true, &why, false) && towersb::Session() == s1 + 1);
    CHECK(ReadVia(main) == "REAL" && !fs::exists(box / L"Steam" / L"1" / L"PBZSaveGame.sav") && !towersb::Dirty());
    {
        int archived = 0;
        for (auto& e : fs::recursive_directory_iterator(base / L"sandbox-archive")) if (e.is_regular_file() && e.path().filename() == L"PBZSaveGame.sav") ++archived;
        CHECK(archived == 1);
    }
    // 同一轮里再开：不换会话、不清影子
    WriteVia(main, "TEST6");
    CHECK(towersb::Set(true, &why, false) && towersb::Session() == s1 + 1 && ReadVia(main) == "TEST6");
    // 脏着强制关：记为"内存仍是塔内状态"，下一轮一开始就算脏
    towersb::SetDirty(true);
    CHECK(towersb::Set(false, &why, true) && !towersb::Active() && towersb::Tainted());
    CHECK(towersb::Set(true, &why, false) && towersb::Dirty());
    CHECK(towersb::Set(false, &why, true));

    // 全程：真目录只剩原来的两个文件，内容不变
    int files = 0;
    for (auto& e : fs::recursive_directory_iterator(real)) if (e.is_regular_file()) ++files;
    CHECK(files == 2 && ReadAll(main) == "REAL" && ReadAll(snap) == "REALSNAP");
    fs::remove_all(base);
    std::printf("TOWER_SANDBOX_OK %d checks (temp dirs, no game process)\n", g_checks);
    return 0;
}
