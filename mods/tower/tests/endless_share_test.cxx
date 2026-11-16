// 3.5.0 无尽档格式：生成 / 解析往返、篡改校验、CRLF + BOM、起始层与难度校验；局档新字段（无尽存档 / 来源）存读
#include <cstdio>
#include <string>
#include "../src/tower_endless_share.h"
#include "../src/tower_logic.h"

static int g_fail = 0, g_checks = 0;
static void Check(bool ok, const char* what) { ++g_checks; if (!ok) { ++g_fail; std::printf("FAIL %s\n", what); } }

int main() {
    namespace es = tower::endlessshare;
    es::Share s; s.author = "小狐"; s.shown = 120; s.difficulty = 0; s.mod = "3.5.0";
    s.runText = "id=abc\nseed=7\nfloor=70\nendless=1\n"; s.growthText = "g.points=1,2,3,4,0,1\ng.weapons=Tang:0:5:0:4\n";
    const std::string text = es::Build(s);
    es::Share back; std::string why;
    Check(es::Parse(text, &back, &why) && back.author == "小狐" && back.shown == 120 && back.difficulty == 0 && back.runText == s.runText && back.growthText == s.growthText, "build / parse round trip");
    std::string crlf; for (char c : text) { if (c == '\n') crlf += "\r\n"; else crlf += c; }
    Check(es::Parse("\xEF\xBB\xBF" + crlf, &back, &why), "CRLF + BOM still parse");
    std::string tampered = text; tampered.replace(tampered.find("floor=70"), 8, "floor=99");
    Check(!es::Parse(tampered, &back, &why) && why.find("校验") != std::string::npos, "edited content fails the checksum");
    es::Share bad = s; bad.shown = 110;
    Check(!es::Parse(es::Build(bad), &back, &why), "start floor must be 100 + 20k");
    bad = s; bad.difficulty = 3;
    Check(!es::Parse(es::Build(bad), &back, &why), "difficulty must be 0 / 1");
    Check(!es::Parse("hello\n", &back, &why), "foreign file rejected");
    es::Share noEnd = s; std::string cut = es::Build(noEnd); cut.erase(cut.find("[end]"));
    Check(!es::Parse(cut, &back, &why), "truncated file rejected");
    es::Share crlfIn = s; crlfIn.runText = "id=abc\r\nseed=7\r\n\r\nfloor=70\r\nendless=1";
    Check(es::Parse(es::Build(crlfIn), &back, &why) && back.runText == "id=abc\nseed=7\nfloor=70\nendless=1\n", "source text with CRLF / blank lines normalised before the checksum");

    // 局档：无尽存档 / 来源字段存读；3.5.0 之前已在无尽层的局自动认作无尽存档
    tower::Run r; r.Start(9, 50); r.endless = true; r.floor = 50; r.endlessSave = true; r.source = "小狐|120";
    tower::Run rb; Check(rb.Load(r.Save()) && rb.endlessSave && rb.source == "小狐|120", "endless save + source persist");
    tower::Run old; old.Start(9, 50); old.endless = true; old.floor = 60; std::string t = old.Save();
    t.replace(t.find("eslot=0"), 7, ""); tower::Run ob;
    Check(ob.Load(t) && ob.endlessSave, "pre-3.5.0 run already past floor 100 counts as the endless save");
    tower::Run at50; at50.Start(9, 50); at50.endless = true; at50.floor = 50; tower::Run a5;
    Check(a5.Load(at50.Save()) && !a5.endlessSave, "a run sitting at floor 99 is not an endless save until the player confirms");

    if (g_fail) std::printf("TOWER_ENDLESS_SHARE_FAILED %d/%d\n", g_fail, g_checks);
    else std::printf("TOWER_ENDLESS_SHARE_OK %d checks\n", g_checks);
    return g_fail ? 1 : 0;
}
