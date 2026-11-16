// 异常诊断离线测试：把 diag.cpp 编进来（UE 读写换成本地受保护读），在临时目录里真的制造访问冲突 / C++ 异常，
// 检查：受保护调用接住并返回 false、报告 JSON（异常码、访问地址、栈、现场段、事件）与标准 minidump 写出、
// 探测读失败不记录、未受保护的首次机会访问冲突被记录且仍交还原处理。
// 编译：cl /std:c++20 /utf-8 /EHa /W4 diag.test.cxx（需要 bcrypt.lib，见下方 pragma）
#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>

#pragma comment(lib, "bcrypt.lib")

#include "../host/log.h"
#include "diag.cpp"

namespace pbz::ue {
static bool Copy(const void* p, void* out, size_t n) {
    __try { memcpy(out, p, n); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool Read(const void* p, void* out, size_t n) { ++diag::t_probe; const bool ok = Copy(p, out, n); --diag::t_probe; return ok; }
void NameRaw(uint32_t idx, char* out, size_t cap) { _snprintf_s(out, cap, _TRUNCATE, "N%u", idx); }
}  // namespace pbz::ue

static int g_checks = 0;
#define CHECK(c) do { ++g_checks; if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); std::exit(1); } } while (0)

namespace fs = std::filesystem;
static std::string ReadAll(const fs::path& p) { std::ifstream f(p, std::ios::binary); std::stringstream s; s << f.rdbuf(); return s.str(); }

__declspec(noinline) static void Crash(volatile int* p) { *p = 42; }
static int FirstChance() {
    __try { Crash(reinterpret_cast<volatile int*>(0x20)); return 0; } __except (EXCEPTION_EXECUTE_HANDLER) { return 1; }
}

int main() {
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    const fs::path base = fs::path(tmp) / L"pbz-diag-test";
    std::error_code ec;
    fs::remove_all(base, ec);
    fs::create_directories(base);
    SetEnvironmentVariableW(L"LOCALAPPDATA", base.c_str());
    pbz::log::Open();
    pbz::diag::Install();
    pbz::diag::MarkGameThread();
    pbz::diag::Section("features", "combat.one_hit[user] ");
    pbz::diag::Section("tower", "run=abc floor=2/3 phase=reward");
    int fake = 0;
    pbz::diag::Event("pe.observe", &fake, nullptr);

    // 探测读失败：不算异常
    int v = 0;
    CHECK(!pbz::ue::Read(reinterpret_cast<void*>(0x10), &v, 4));
    CHECK(pbz::diag::Summary().find("\"total\":0") != std::string::npos);

    // 受保护调用里的访问冲突：接住、返回 false、写报告与 minidump
    const bool ok = pbz::diag::Guarded("测试：写空指针", [] { Crash(reinterpret_cast<volatile int*>(0x18)); });
    CHECK(!ok);
    // C++ 异常同样接住
    CHECK(!pbz::diag::Guarded("测试：抛异常", [] { throw std::runtime_error("x"); }));
    // 正常调用返回 true
    int ran = 0;
    CHECK(pbz::diag::Guarded("测试：正常", [&] { ran = 1; }) && ran == 1);
    // 未受保护的首次机会访问冲突：记录后交还原处理（这里由外层 __except 接住）
    CHECK(FirstChance() == 1);

    const auto sum = pbz::diag::Summary();
    CHECK(sum.find("\"total\":3") != std::string::npos);
    int json = 0, dmp = 0;
    std::string av;
    for (auto& e : fs::directory_iterator(base / L"PBZLab" / L"crash")) {
        if (e.path().extension() == L".json") { ++json; auto t = ReadAll(e.path()); if (t.find("测试：写空指针") != std::string::npos) av = t; }
        if (e.path().extension() == L".dmp" && fs::file_size(e.path()) > 4096) {
            ++dmp;
            CHECK(ReadAll(e.path()).rfind("MDMP", 0) == 0);  // 标准 minidump 头
        }
    }
    CHECK(json == 3 && dmp == 3);  // 被吞 2 份 + 首次机会 1 份，各带 minidump（未超上限）
    CHECK(av.find("\"code\":\"0xC0000005\"") != std::string::npos);
    CHECK(av.find("\"access\":\"write\",\"access_address\":\"0x18\"") != std::string::npos);
    CHECK(av.find("\"handled\":true") != std::string::npos && av.find("\"game_thread\":true") != std::string::npos);
    CHECK(av.find("\"where\":\"测试：写空指针\"") != std::string::npos);
    CHECK(av.find("combat.one_hit[user]") != std::string::npos && av.find("phase=reward") != std::string::npos);
    CHECK(av.find("\"tag\":\"pe.observe\"") != std::string::npos);
    CHECK(av.find("\"module\":\"diag_test.exe\"") != std::string::npos);  // 栈顶帧落在本程序（可符号化：模块 + 偏移）
    CHECK(sum.find("\"source\":\"first_chance\"") != std::string::npos);
    CHECK(ReadAll(base / L"PBZLab" / L"core.log").find("异常记录 #1") != std::string::npos);

    pbz::diag::Uninstall();
    fs::remove_all(base, ec);
    std::printf("CORE_DIAG_OK %d checks (temp dirs, no game process)\n", g_checks);
    return 0;
}
