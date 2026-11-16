// 1.3.5 自定义打开键：hotkey.txt 解析与占用键防护（不碰真实文件）
#include <cstdio>
#include "../src/tower_hotkey.h"
int main() {
    using namespace tower::hotkey;
    int checks = 0, failed = 0;
    auto check = [&](bool ok, const char* why) { ++checks; if (!ok) { ++failed; std::printf("FAIL %s\n", why); } };
    check(Parse("vk=117\nname=F6").vk == VK_F6 && Parse("vk=117\nname=F6").name == "F6", "decimal vk + display name");
    check(Parse("vk=0x75\r\n").vk == VK_F6, "hex vk with CRLF");
    check(Parse("\xEF\xBB\xBFvk=36\n").vk == VK_HOME, "UTF-8 BOM before the first line");
    check(Parse("vk=87").vk == 0, "W (move) refused");
    check(Parse("vk=1").vk == 0, "left mouse (light attack) refused");
    check(Parse("vk=162").vk == 0 && Parse("vk=17").vk == 0, "Ctrl refused");
    check(Parse("vk=81").vk == 0 && Parse("vk=49").vk == 0 && Parse("vk=50").vk == 0, "Q / 1 / 2 refused");
    check(Parse("vk=45").vk == 0, "Insert itself is not a separate custom key");
    check(Parse("vk=300").vk == 0 && Parse("vk=abc").vk == 0 && Parse("").vk == 0, "out of range / garbage / empty -> Insert only");
    check(Parse("vk=5").vk == VK_XBUTTON1 && Parse("vk=6").vk == VK_XBUTTON2, "mouse side buttons allowed");
    check(Usable(VK_HOME) && Usable('G') && Usable(VK_F1) && Usable(VK_OEM_3), "free keys usable");
    std::printf("TOWER_HOTKEY_%s %d checks (no game process)\n", failed ? "FAILED" : "OK", checks);
    return failed ? 1 : 0;
}
