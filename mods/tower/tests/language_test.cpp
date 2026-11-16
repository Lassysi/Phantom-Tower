#include "../src/tower_language.h"
#include "../src/tower_l10n_text.h"
#include "../src/tower_l10n_layout.h"
#include "../src/boss_pool.generated.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>

int main(int argc, char** argv) {
    using namespace tower::language;
    assert(DefaultForWindowsLanguage(0x0804) == Language::Chinese);
    assert(DefaultForWindowsLanguage(0x0404) == Language::Chinese);
    assert(DefaultForWindowsLanguage(0x0409) == Language::English);
    assert(DefaultForWindowsLanguage(0x0411) == Language::English);
    assert(Parse("") == Language::Chinese);
    assert(Parse("zh\n") == Language::Chinese);
    assert(Parse(" en\r\n") == Language::English);
    assert(Parse("english") == Language::Chinese);
    assert(Parse("en\nzh") == Language::Chinese);
    assert(Parse(Encode(Language::Chinese)) == Language::Chinese);
    assert(Parse(Encode(Language::English)) == Language::English);
    assert(Display("返回", Language::Chinese) == "返回");
    assert(Display("返回", Language::English) == "Back");
    assert(Display("未知文本", Language::English) == "未知文本");
    assert(Display("返回准备", Language::English) == "Back to Preparation");
    assert(Display("铸魂令", Language::English) == "Soul Tokens");
    assert(Display("驿站　魂玉 100", Language::English) == "Rest Stop · Soul Jade 100");
    assert(Display("MOD 背包　未准备", Language::English) == "Mod Inventory · None Prepared");
    assert(Display("复活符 ×2（待生效）　撤销", Language::English) == "Revival Talisman ×2 (Prepared) · Remove");
    assert(Display("药葫芦", Language::English) == "Healing Gourd");
    assert(Display("火流星", Language::English) == "FIRE METEOR");
    assert(Display("复活符", Language::English) == "Revival Talisman");
    assert(Display("本层问题", Language::English) == "Floor Issue");
    assert(Display("确认保存并退出", Language::English) == "Confirm Save and Exit");
    assert(Display("确认购买", Language::English) == "Confirm Purchase");
    assert(Display("商品介绍（1/3）", Language::English) == "Item Details (1/3)");
    assert(Display("Boss关卡进度 14/15，还需 1 关\n可用轮回点 0", Language::English) ==
           "Boss stages: 14/15; 1 to go\nAvailable cycle points: 0");
    assert(Display("Boss关卡进度 45，轮回点目标已达成", Language::English) ==
           "Boss stages: 45; cycle point milestones reached");
    assert(Display("透视眼", Language::English) == "Foresight");
    assert(Display("透视眼 ×1　自动生效", Language::English) == "Foresight ×1 · Activates Automatically");
    assert(Display("透视眼　432 魂玉", Language::English) == "Foresight · 432 Soul Jade");
    assert(Display("下一场 Boss：拳痴：铁狂", Language::English) == "Next boss: " + Display("拳痴：铁狂", Language::English));
    assert(Display("第 26 层（困难）", Language::English) == "Floor 26 (Hard)");
    assert(Display("生命 935/935\n杀气 400/400", Language::English) == "HP 935/935\nSha-Chi 400/400");
    assert(Display("复活符×2：倒下后满状态复活一次", Language::English) == "Revival Talisman ×2: Revives you once at full strength.");
    assert(Display("影塔·百战　第 26 层（困难）", Language::English) == "Phantom Tower · Floor 26 (Hard)");
    assert(Display("影塔·百战　第 26 层（困难）", Language::Chinese) == "影塔·百战　第 26 层（困难）");
    assert(Display("无关内容", Language::English) == "无关内容");
    assert(Display("　生命 100/200", Language::English) == "  HP 100/200");
    assert(Display("本层收获：魂玉 +100\n本层用时 01:23.4", Language::English) ==
           "Floor reward: Soul Jade +100\nFloor time: 01:23.4");
    assert(Display("淬锋（凡）", Language::English) == "Honed Edge (Common)");
    assert(Display("魂玉 +100", Language::English) == "Soul Jade +100");
    assert(Display("主兵器：药葫芦、复活符", Language::English) == "Main Weapons: Healing Gourd, Revival Talisman");
    assert(Display("额外挑战：禁药　未达成", Language::English) == "Bonus Challenge: No Medicine · Failed");
    assert(Display("主兵器：黑伤 1 级、黑伤双刃 1 级", Language::English) ==
           "Main Weapons: SANGUINE Lv. 1, SANGUINE TWIN Lv. 1");
    assert(Display("本局一直生效：攻击 / 杀气攻击 +8%，本次 2 层，最多 3 层", Language::English) ==
           "Lasts for this run: Attack and Sha-Chi attack +8%. Gain 2 stack(s), up to 3.");
    assert(Display("喝药后随机获得以下一种效果：\n1．使自身与附近敌人陷入燃烧状态。\n2．使自身与附近敌人陷入中毒状态。\n3．凝聚混沌火球，并自动攻击附近的敌人。\n4．即刻回复全部生命值和50%杀气值", Language::English)
               .starts_with("After using Medicine, randomly gain one of the following effects:"));
    for (const auto& boss : tower::kBossPool) assert(Display(boss.name, Language::English) != boss.name);
    assert(RowsPerPage(false, 10) == 10 && RowsPerPage(true, 10) == 5);
    assert(Pages(0, 5) == 1 && Pages(5, 5) == 1 && Pages(6, 5) == 2 && Pages(11, 5) == 3);
    assert(RowIndex(0, 4, 5) == 4 && RowIndex(1, 0, 5) == 5 && RowIndex(2, 0, 5) == 10);
    for (const bool english : {false, true}) {
        const int per = RowsPerPage(english, 10);
        for (int total = 0; total <= 31; ++total) {
            int next = 0;
            for (int page = 0; page < Pages(total, per); ++page)
                for (int row = 0; row < per && RowIndex(page, row, per) < total; ++row)
                    assert(RowIndex(page, row, per) == next++);
            assert(next == total);
        }
    }
    for (size_t i = 0; i < kInterface.size(); ++i) {
        assert(!kInterface[i].first.empty() && !kInterface[i].second.empty());
        assert(Display(kInterface[i].first, Language::English) == kInterface[i].second);
        for (size_t j = i + 1; j < kInterface.size(); ++j) assert(kInterface[i].first != kInterface[j].first);
    }
    for (size_t i = 0; i < kPatterns.size(); ++i) {
        assert(!kPatterns[i].first.empty() && !kPatterns[i].second.empty());
        for (size_t j = i + 1; j < kPatterns.size(); ++j) assert(kPatterns[i].first != kPatterns[j].first);
        std::string source(kPatterns[i].first), expected(kPatterns[i].second);
        size_t index = 0;
        for (size_t at = 0; (at = source.find("{}", at)) != std::string::npos; ++index) {
            const std::string token = "FIELD" + std::to_string(index);
            source.replace(at, 2, token);
            at += token.size();
            const auto marker = "{" + std::to_string(index) + "}";
            size_t position = 0;
            while ((position = expected.find(marker, position)) != std::string::npos) {
                expected.replace(position, marker.size(), token);
                position += token.size();
            }
        }
        assert(index > 0 && expected.find('{') == std::string::npos);
        if (Display(source, Language::English) != expected) {
            std::fprintf(stderr, "Pattern %zu failed\n", i);
            return 1;
        }
    }
    if (argc == 2 && std::strcmp(argv[1], "write-en") == 0) {
        assert(Current() == Language::Chinese);
        assert(Set(Language::English));
        assert(Current() == Language::English);
    } else if (argc == 2 && std::strcmp(argv[1], "read-en") == 0) {
        assert(Current() == Language::English);
        assert(Set(Language::Chinese));
        assert(Current() == Language::Chinese);
    } else if (argc == 2 && std::strcmp(argv[1], "read-zh") == 0) {
        assert(Current() == Language::Chinese);
    } else if (argc == 2 && std::strcmp(argv[1], "write-fails") == 0) {
        assert(Current() == Language::Chinese);
        assert(!Set(Language::English));
        assert(Current() == Language::Chinese);
    }
}
