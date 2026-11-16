// 独立塔档离线测试：存档 JSON 读写、塔内存档生成、进出塔状态机、Boss 准入、2+2+2 起手。
// 不碰游戏；本机有 %LOCALAPPDATA%\PBZTower\baseline.json 时额外用真实基准跑一遍（只检查结构，不打印内容）。
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <set>
#include <sstream>
#include <string>

#include "../src/boss_pool.generated.h"
#include "../src/tower_admission.h"
#include "../src/tower_logic.h"
#include "../src/tower_plan.h"
#include "../src/tower_trip.h"

static int g_checks = 0, g_failed = 0;
static void Check(bool ok, const char* what) {
    ++g_checks;
    if (!ok) { ++g_failed; std::printf("FAIL %s\n", what); }
}

using tower::json::Value;
namespace gr = tower::growth;

static const char* kBaseline = R"({"PBZGameSystem":{"CurrentLevelName":"PBZ_Fujizhilin_WP","PlayerTransform":{"Location":{"X":-1435.25,"Y":165305.0,"Z":-1184.1}}},)"
    R"("Inventory":{"InventoryInfos":{"Page_XC_01":{"CurrentAmount":1},"Item_001":{"CurrentAmount":27},"Item_Piece_002":{"CurrentAmount":3},"Weapon_Material_01":{"CurrentAmount":30},"Ac_Material_1":{"CurrentAmount":2},"Medi_Material_2":{"CurrentAmount":4},"Return_Weapon_1":{"CurrentAmount":1},"K_01002":{"CurrentAmount":1}},)"
    R"("AccessoryIDs":["AC_Counter","AC_PerfectFrame","None","None","None","None"],"AccessoryInfos":{"AC_Counter":{"CurrentLevel":0,"DirtyTime":"t"},"AC_PerfectFrame":{"CurrentLevel":1,"DirtyTime":"t"},"AC_Loot":{"CurrentLevel":0,"GrantedByEntitlement":true}},)"
    R"("TopMedicine":"Medicine_Recover","BottomMedicine":"RongQi","MedicineInfos":{"Hulu":{"CurrentLevel":0,"CurrentTime":2,"MaxTime":2,"DirtyTime":"t"},"RongQi":{"CurrentLevel":0,"CurrentTime":1,"MaxTime":1}},)"
    R"("CurrentWeaponIndex":1,"CurrentMainWeaponID":"Tang","MainWeapons":["ZhuGuangBaoQi","Tang"],)"
    R"("MainWeaponInfos":{"Tang":{"DateTime":"d","AlreadySnapSoul":0,"AcquiredCycle":0},"ZhanMaBlade":{"DateTime":"d","WeaponLevel":10,"AlreadySnapSoul":0}},)"
    R"("CurrentShaqiWeaponIndex":0,"ShaqiWeapons":["TanLang","ThunderSpear"],"ForcedLoadoutActive":false,"ShaChiWeaponInfo":{"UnnamedBow":{"AlreadySnapSoul":false},"ThunderSpear":{"ShaChiWeaponLevel":30,"AlreadySnapSoul":false}},"HasTriggeredFirstTwoMedicines":true},)"
    R"("Player":{"Dust":134796,"Name":"影"},"AbilityNode_RenewalHun":{"AbilityNodes":{"HeiShi1":1,"HeiShi2":1,"ShaChiHuPo1":1,"MemoryFlower1":0}},"BossRecord":{"X":[1,2.5,-3e2]}})";

// 假游戏端：memory = 游戏内存里的存档串（回标题 → 继续时载入它）；live = 世界里实际是什么内容。
// 13f7e98 实机反例：跨图钟传送会把 live（塔内状态）存回 memory 并换一个新背包，但不经过标题
struct FakeIo : tower::trip::IIo {
    std::string memory = "BASE", live = "BASE", baseline;
    bool sandbox = false, ach = false, failBackup = false, failReplace = false, title = false, sawTitle = false, failTitlePut = false;
    int bag = 0, armedBag = 0;
    std::vector<std::string> calls;
    bool Backup(std::string* w) override { calls.push_back("backup"); if (failBackup) *w = "x"; return !failBackup; }
    bool SandboxOn(bool snap, std::string*) override { calls.push_back(snap ? "sandbox+base" : "sandbox"); sandbox = true; return true; }
    bool Baseline(std::string* j) override { *j = baseline; return !baseline.empty(); }
    bool Replace(const std::string& j, bool isBase, std::string* w) override {
        calls.push_back(isBase ? "put_base" : "put_tower");
        if (failReplace || (title && failTitlePut)) { *w = "x"; failTitlePut = false; return false; }
        memory = j;
        armedBag = bag;
        sawTitle = title;  // 在标题上重放：保留"到过标题"
        return true;
    }
    bool AtTitle() override { if (title) sawTitle = true; return title; }
    bool Reloaded() override { return !title && sawTitle && bag != armedBag; }
    bool Verify(const std::string& expect, std::string* diff) override { if (live == expect) return true; *diff = "content differs"; return false; }
    bool SandboxOff(std::string*) override { calls.push_back("off"); sandbox = false; return true; }
    std::string tmpl;  // 世界模板（空 = 没有）
    bool WorldTemplate(std::string* j) override { if (tmpl.empty()) return false; *j = tmpl; return true; }
    void Achievements(bool b) override { ach = b; }
    // 模拟玩家 / 游戏动作
    void Teleport() { memory = live; ++bag; }      // 跨图钟传送：游戏把当前状态存进内存再载入（新背包），不经过标题
    void ToTitle() { title = true; }               // 回到标题：塔内世界卸载
    void Continue() { title = false; live = memory; ++bag; }  // 继续：载入内存里的存档
};

// 自动切档的假游戏端：busy = 原生加载 / 传送中；ToTitle / Continue 模拟原生返回标题、继续（可设成没反应）
struct AutoIo : FakeIo {
    bool busy = false, deadTitle = false, deadContinue = false, menu = true, corrupt = false;
    int toTitle = 0, cont = 0;
    uint64_t now = 0;
    bool Calm() override { return !busy; }
    bool ToTitle(std::string*) override { ++toTitle; if (!deadTitle) FakeIo::ToTitle(); return true; }
    bool TitleReady() override { return menu; }
    bool Continue(std::string*) override { ++cont; if (!deadContinue) FakeIo::Continue(); if (corrupt) live = "X"; return true; }
    uint64_t Now() override { return now; }
};
// fa164d1 反例的假原生存档：锁开着时的存档排队（Suspended），放开锁的那一刻补存；空闲自动存在 dirtyAt 时刻再存一次。
// 写盘时沙盒开着 → 落进沙盒（activity++）；沙盒关着 → 写真档（realWrites++，就是验收失败）
struct SettleIo : AutoIo {
    bool lock = false, queued = false, dirty = false;
    uint64_t activity = 0, fireAt = 0, idleSaveAt = 0;
    int realWrites = 0;
    bool SandboxOn(bool snap, std::string* w) override { lock = true; return FakeIo::SandboxOn(snap, w); }
    bool SettleSaves() override { return true; }
    bool ReleaseLock(std::string*) override { lock = false; if (queued) fireAt = now + 1280; return true; }
    uint64_t SandboxActivity() override { Pump(); return activity; }
    bool SaveClean(std::string* why) override { Pump(); if (dirty) *why = "dirty"; return !dirty; }
    bool SandboxOff(std::string* w) override { lock = false; return FakeIo::SandboxOff(w); }
    void Pump() {  // 原生存档按时间推进
        auto write = [&] { if (sandbox) ++activity; else ++realWrites; dirty = false; };
        if (fireAt && now >= fireAt) { fireAt = 0; queued = false; write(); }
        if (idleSaveAt && now >= idleSaveAt && !lock) { idleSaveAt = 0; write(); }
    }
    void LoadedAtBell() { queued = true; dirty = true; idleSaveAt = now + 4000; }  // 载入到钟旁：锁里排队一次，之后空闲再存一次
};

// 推进：每次 500ms（同引擎的 TripTick 间隔）
static void Run(AutoIo& io, tower::trip::Trip& t, int ticks, bool* entered = nullptr) {
    for (int i = 0; i < ticks; ++i) { io.now += 500; if (t.Tick(io) && entered) *entered = true; }
}

static std::string Text(const Value* v) { return v && v->kind == Value::String ? v->text : std::string("?"); }

int main() {
    // ---- JSON：读写往返、数字原文、Unicode 转义
    {
        Value v;
        Check(tower::json::Parse(kBaseline, &v), "json: baseline parses");
        Value again;
        Check(tower::json::Parse(tower::json::Dump(v), &again) && tower::json::Dump(again) == tower::json::Dump(v), "json: dump/parse round trip is stable");
        Check(tower::json::Dump(v).find("-1435.25") != std::string::npos && tower::json::Dump(v).find("-3e2") != std::string::npos, "json: numbers keep their original text");
        Check(Text(v.Find("Player")->Find("Name")) == "\xe5\xbd\xb1", "json: \\u escapes decode to UTF-8");
        Value bad;
        Check(!tower::json::Parse("{\"a\":1,", &bad) && !tower::json::Parse("{\"a\":1} x", &bad), "json: truncated / trailing garbage rejected");
    }

    // ---- 装备槽空槽语义（a7c1c2d 首次进塔阻断：原生空槽读回 ""，存档写 "None"）
    {
        using tower::ids::SameSlots;
        Check(SameSlots({"UnnamedBow", "None"}, {"UnnamedBow", ""}) && SameSlots({"UnnamedBow", ""}, {"unnamedbow", "NONE"}), "slots: native empty FName reads as \"\" and matches the saved \"None\"");
        Check(!SameSlots({"UnnamedBow", "None"}, {"UnnamedBow"}) && !SameSlots({"UnnamedBow", "None"}, {"UnnamedBow", "BrutalAxe"}) &&
              !SameSlots({"UnnamedBow", "None"}, {"None", "UnnamedBow"}) && !SameSlots({"Tang", "Double"}, {"Tang", ""}),
              "slots: slot count, order and real weapons are still checked exactly");
    }

    // ---- 重放复位：原生 Flow 节点的"已执行"记录（6d105f2 第 4 层追命不进二阶段）
    {
        Value save;
        tower::json::Parse(R"({"EventPipeline":{"ExecuteNodeGuid":{"2B132618485DE089BF7126A02A912116":true,)"
                           R"("PBZ_Xigu_WP-BP_FlowActor_C_UAID_E89C25A75434EA4202_1100302205_22D84278492E633ADF6615916728790C":true,)"
                           R"("PBZ_Xigu_WP-BP_FlowActor_C_UAID_X_1_AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA":true,"short":true},"GlobalTags":["xg.击杀.追命","keep.me"]}})", &save);
        const int nodes = tower::save::ResetReplayNodes(save), tags = tower::save::ResetReplayTags(save);
        const Value* done = save.Find("EventPipeline")->Find("ExecuteNodeGuid");
        Check(nodes == 2 && done->fields.size() == 2 && done->Find("short") && done->Find("PBZ_Xigu_WP-BP_FlowActor_C_UAID_X_1_AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"),
              "replay reset: executed records of contract flow nodes removed (bare and actor-prefixed keys), unrelated kept");
        Check(tags == 1 && save.Find("EventPipeline")->Find("GlobalTags")->items.size() == 1, "replay reset: kill tags still removed alongside");
        bool all = true;
        for (auto& c : tower::replay::kContracts) {
            int n = 0;
            for (auto& x : tower::replay::kFlowNodes) n += x.contract == c.id;
            all = all && (n > 0 || c.spawnerKick);  // 原生刷怪点直刷（赤影骨刃）没有出场 / 死亡 Flow
        }
        Check(all, "replay reset: every replay contract has its native flow node list");
    }

    // ---- 起手 = 原游戏新档（用户 2026-11-03）
    const auto kit = gr::Profile::Kit();
    {
        std::set<std::string> mains, shaqi;
        for (auto& w : kit.weapons) (gr::kWeapons[w.index].kind == 0 ? mains : shaqi).insert(std::string(gr::kWeapons[w.index].id));
        Check(mains == std::set<std::string>{"Tang", "Double", "ZhanMaBlade"}, "kit: all three starting main weapons are owned");
        Check(shaqi == std::set<std::string>{"UnnamedBow", "BrutalAxe"}, "kit: bow and shield-breaking axe are owned");
        Check(kit.medicines.size() == 2 && gr::kMedicines[kit.medicines[0]].id == "Hulu" && gr::kMedicines[kit.medicines[1]].id == "NingFengDan", "kit: 药葫芦 + 凝锋丹");
        bool lv1 = true;
        for (auto& w : kit.weapons) lv1 = lv1 && w.level == 1 && !w.snapped;
        int nodesOn = 0;
        for (int n = 0; n < gr::kNodeCount; ++n) nodesOn += kit.nodes[n];
        Check(lv1 && nodesOn == 0 && kit.medLevel <= 1 && kit.accessories.empty(), "kit: everything level 1, no abilities, no accessories");
        auto t = kit; t.TrialKit();
        Check(t.medicines.size() == 2 || t.medicines.size() == 3, "kit: trial extras do not duplicate a medicine");
    }

    // ---- 塔内存档生成
    {
        Value base;
        tower::json::Parse(kBaseline, &base);
        auto p = kit;
        p.weapons[0].level = 7;   // 黑伤 7 级
        p.nodes[gr::NodeIndex("HeiShi1")] = 1;
        p.medLevel = 3;
        p.AddAccessory(gr::AccessoryIndex("AC_Counter"));
        const auto eq = tower::save::DefaultEquip(p);
        {  // 继续上一局按保存时的装备还原（用户 2026-11-05）；塔档里没有的那组退回默认
            auto q = p;
            q.AddWeapon(gr::WeaponIndex("BrutalAxe", 1));
            const auto saved = tower::save::SavedEquip(q, "Double,Tang|BrutalAxe,None|None,None|AC_Counter");
            Check(saved.main.size() == 2 && saved.main[0] == "Double" && saved.main[1] == "Tang" && saved.shaqi[0] == "BrutalAxe" && saved.accessories.size() == 1, "save: resume restores the saved loadout");
            const auto bad = tower::save::SavedEquip(q, "NotOwned,Tang|BrutalAxe,None|None,None|");
            Check(bad.main == tower::save::DefaultEquip(q).main && bad.shaqi[0] == "BrutalAxe", "save: an unowned saved weapon falls back to the default for that group only");
            Check(tower::save::SavedEquip(q, "").main == tower::save::DefaultEquip(q).main, "save: no saved loadout = default");
        }
        const auto r = tower::save::Build(base, p, eq, "2026.10.31-18.16.05");
        Check(r.ok, "save: tower save builds");
        const Value* inv = r.save.Find("Inventory");
        auto names = [](const Value* a) { std::vector<std::string> v; if (a) for (auto& x : a->items) v.push_back(x.text); return v; };
        Check(names(inv->Find("MainWeapons")) == eq.main && eq.main.size() == 2 && names(inv->Find("ShaqiWeapons")) == eq.shaqi &&
              eq.main == std::vector<std::string>{"Tang", "ZhanMaBlade"} && eq.shaqi == std::vector<std::string>{"UnnamedBow", "BrutalAxe"}, "save: equip requested two mains plus bow and axe");
        Check(Text(inv->Find("CurrentMainWeaponID")) == eq.main[0] && inv->Find("CurrentWeaponIndex")->Int() == 0, "save: current weapon is the first tower main weapon");
        {   // 3.3.2：回收了正在装备的兵器 → 空槽在前，当前手持 0 不能指向空槽
            auto gap = eq; gap.shaqi = {"None", "BrutalAxe"}; gap.main = {"None", "Tang"};
            const auto g = tower::save::Build(base, p, gap, "2026.10.31-18.16.05");
            const Value* gi = g.ok ? g.save.Find("Inventory") : nullptr;
            Check(gi && names(gi->Find("ShaqiWeapons")) == std::vector<std::string>{"BrutalAxe", "None"} && names(gi->Find("MainWeapons")) == std::vector<std::string>{"Tang", "None"} &&
                  Text(gi->Find("CurrentMainWeaponID")) == "Tang", "3.3.2 save: filled slots first so the current slot index points to a weapon");
        }
        const Value* mains = inv->Find("MainWeaponInfos");
        Check(mains->fields.size() == 3 && mains->Find("Tang") && mains->Find("Double") && mains->Find("ZhanMaBlade"), "save: all three mains owned including unequipped dual blades");
        Check(mains->Find("Tang")->Find("WeaponLevel")->Int() == 7 && !mains->Find("Double")->Find("WeaponLevel"), "save: main weapon level written (level 1 omitted like the game)");
        Check(Text(mains->Find("Tang")->Find("DateTime")) == "d" && Text(mains->Find("Double")->Find("DateTime")) == "2026.10.31-18.16.05", "save: existing entries keep their fields, new ones get the stamp");
        const Value* shaqi = inv->Find("ShaChiWeaponInfo");
        Check(shaqi->fields.size() == 2 && shaqi->Find("UnnamedBow") && shaqi->Find("BrutalAxe") && !shaqi->Find("ThunderSpear"), "save: bow and axe owned without unrelated shadow weapons");
        const Value* meds = inv->Find("MedicineInfos");
        Check(meds->fields.size() == 2 && meds->Find("Hulu")->Find("CurrentLevel")->Int() == 2 && !meds->Find("RongQi"), "save: medicines are the tower two, level stored from 0");
        Check(meds->Find("Hulu")->Find("MaxTime")->Int() == p.MedUses(gr::MedicineIndex("Hulu")) && meds->Find("Hulu")->Find("CurrentTime")->Int() == meds->Find("Hulu")->Find("MaxTime")->Int(), "save: medicine charges full for the tower level");
        Check(Text(inv->Find("BottomMedicine")) == "Hulu" && Text(inv->Find("TopMedicine")) == eq.meds[1], "save: both medicine slots filled with the tower medicines");
        const Value* accs = inv->Find("AccessoryInfos");
        Check(accs->fields.size() == 2 && accs->Find("AC_Counter") && !accs->Find("AC_PerfectFrame") && inv->Find("AccessoryIDs")->items.size() == 6 && inv->Find("AccessoryIDs")->items[0].text == "None",
              "save: only tower accessories (+ entitlement items), six empty slots");
        Check(accs->Find("AC_Loot") && accs->Find("AC_Loot")->Find("GrantedByEntitlement")->b, "save: 聚宝盆 entitlement entry kept as the game re-grants it (no entitlement change, not a tower reward)");
        const Value* items = inv->Find("InventoryInfos");
        Check(items->Find("Page_XC_01") && items->Find("K_01002") && !items->Find("Weapon_Material_01") && !items->Find("Ac_Material_1") && !items->Find("Medi_Material_2") && !items->Find("Return_Weapon_1"),
              "save: upgrade materials removed, story / key items kept");
        Check(!items->Find("Item_001") && !items->Find("Item_Piece_002"), "save: permanent HP / shachi boost items removed (stats start at native new-game values)");
        Check(r.save.Find("Player")->Find("Dust")->Int() == 0, "save: dust is zero (tower upgrades use points)");
        const Value* nodes = r.save.Find("AbilityNode_RenewalHun")->Find("AbilityNodes");
        Check(nodes->Find("HeiShi1")->Int() == 1 && nodes->Find("HeiShi2")->Int() == 0 && nodes->Find("ShaChiHuPo1")->Int() == 0, "save: ability nodes follow the tower profile");
        Check(tower::json::Dump(*r.save.Find("BossRecord")) == tower::json::Dump(*base.Find("BossRecord")) &&
              tower::json::Dump(*r.save.Find("PBZGameSystem")) == tower::json::Dump(*base.Find("PBZGameSystem")), "save: world / story sections untouched");
        Value broken = base;
        broken.Erase("Inventory");
        Check(!tower::save::Build(broken, p, eq, "s").ok, "save: refuses a baseline without Inventory");
    }

    // ---- 进出塔状态机（含 13f7e98 实机反例）
    {
        auto enter = [&](FakeIo& io, tower::trip::Trip& t) {
            io.baseline = kBaseline; io.memory = kBaseline; io.live = kBaseline;
            const bool ok = t.Enter(io, kit, tower::save::DefaultEquip(kit), "s");
            io.ToTitle(); t.Tick(io); io.Continue();
            for (int i = 0; i < 3; ++i) t.Tick(io);
            return ok;
        };
        FakeIo io;
        tower::trip::Trip t;
        io.baseline = kBaseline; io.memory = kBaseline; io.live = kBaseline;
        Check(t.Enter(io, kit, tower::save::DefaultEquip(kit), "s") && t.phase() == tower::trip::Phase::Entering, "trip: enter puts the tower save in memory");
        Check(io.calls.size() >= 3 && io.calls[0] == "backup" && io.calls[1] == "sandbox+base" && io.calls.back() == "put_tower" && io.ach && io.sandbox, "trip: order backup -> sandbox+baseline -> tower save");
        Value m;
        Check(tower::json::Parse(io.memory, &m) && m.Find("Player")->Find("Dust")->Int() == 0, "trip: memory holds a valid tower save");
        io.Teleport();  // 没回标题、只是跨图：不算进塔
        t.Tick(io);
        Check(t.phase() == tower::trip::Phase::Entering, "trip counterexample: cross-map teleport (new bag, no title) is not a reload");
        io.failTitlePut = true;  // 到标题时重放失败：载入的是被传送盖掉的内容
        io.ToTitle(); t.Tick(io); io.Continue(); t.Tick(io);
        Check(t.phase() == tower::trip::Phase::Entering, "trip counterexample: overwritten tower save loads -> verification fails, still entering");
        Check(!t.error().empty() && t.retries() == 1, "trip: verification failure is reported and counted");
        io.ToTitle(); t.Tick(io); io.Continue();
        Check(t.Tick(io) && t.phase() == tower::trip::Phase::Inside, "trip: after re-put and a real title reload the tower save verifies -> inside");
        {   // 世界模板：塔档的世界 / 剧情用模板，出塔照样放回玩家原档；结构对不上的模板不用
            Value tv; Check(tower::json::Parse(kBaseline, &tv), "template: parse baseline");
            tv.Find("PBZGameSystem")->Set("CurrentLevelName", Value::Str("TEMPLATE_WORLD"));
            FakeIo wio; tower::trip::Trip wt;
            wio.tmpl = tower::json::Dump(tv);
            Check(enter(wio, wt) && wt.phase() == tower::trip::Phase::Inside && wt.usedTemplate(), "template: entered with the world template");
            Value mem; Check(tower::json::Parse(wio.memory, &mem) && mem.Find("PBZGameSystem")->Find("CurrentLevelName")->text == "TEMPLATE_WORLD", "template: tower save world comes from the template");
            Check(wt.Exit(wio) && wio.memory == kBaseline, "template: exit puts the player's own save back");
            {   // 1.0.1：模板主人的平台赠品（AC_Loot 聚宝盆）不进别人的塔档；玩家自己有的赠品照留
                Value tg = tv;
                Value gift = Value::Obj(); gift.Set("CurrentLevel", Value::Num(0)); gift.Set("GrantedByEntitlement", Value::Boolean(true));
                tg.Find("Inventory")->Find("AccessoryInfos")->Set("AC_Loot", gift);
                Value pb; Check(tower::json::Parse(kBaseline, &pb), "entitlement: parse player save");
                pb.Find("Inventory")->Find("AccessoryInfos")->Erase("AC_Loot");  // 这位玩家没有这件赠品
                const std::string pbs = tower::json::Dump(pb);
                FakeIo gio; tower::trip::Trip gt; gio.tmpl = tower::json::Dump(tg);
                gio.baseline = pbs; gio.memory = pbs; gio.live = pbs;
                Check(gt.Enter(gio, kit, tower::save::DefaultEquip(kit), "s") && gt.usedTemplate(), "entitlement: entered with the gifted template");
                Value gm; Check(tower::json::Parse(gio.memory, &gm), "entitlement: tower save parses");
                Check(!gm.Find("Inventory")->Find("AccessoryInfos")->Find("AC_Loot"), "entitlement: template owner's gift is not copied into another player's tower save");
                FakeIo oio; tower::trip::Trip ot; oio.tmpl = tower::json::Dump(tg);
                Check(enter(oio, ot), "entitlement: player who owns the gift enters");
                Value om; Check(tower::json::Parse(oio.memory, &om) && om.Find("Inventory")->Find("AccessoryInfos")->Find("AC_Loot"), "entitlement: the player's own gift is kept");
                {   // 1.0.1 多周目：二周目原档里的赠品（AcquiredCycle 2、精炼 3 级）进塔档后归 0 周目、未精炼
                    Value ng; Check(tower::json::Parse(kBaseline, &ng), "ng+ gift: parse player save");
                    Value g2 = Value::Obj(); g2.Set("CurrentLevel", Value::Num(3)); g2.Set("AcquiredCycle", Value::Num(2)); g2.Set("GrantedByEntitlement", Value::Boolean(true));
                    ng.Find("Inventory")->Find("AccessoryInfos")->Set("AC_Loot", g2);
                    const std::string ngs = tower::json::Dump(ng);
                    Value tn = tv; tn.Find("Inventory")->Find("AccessoryInfos")->Erase("AC_Loot");
                    FakeIo nio; tower::trip::Trip nt; nio.tmpl = tower::json::Dump(tn);
                    nio.baseline = ngs; nio.memory = ngs; nio.live = ngs;
                    Check(nt.Enter(nio, kit, tower::save::DefaultEquip(kit), "s") && nt.usedTemplate(), "ng+ gift: enters with the template");
                    Value nm; Check(tower::json::Parse(nio.memory, &nm), "ng+ gift: tower save parses");
                    const Value* got = nm.Find("Inventory")->Find("AccessoryInfos")->Find("AC_Loot");
                    Check(got && got->Find("AcquiredCycle")->Int() == 0 && got->Find("CurrentLevel")->Int() == 0, "ng+ gift: copied gift is cycle 0 and unrefined");
                }
            }
            {   // 1.0.1：二周目存档多出模板没有的分区 → 仍用模板（周目、角色不跟进塔档），多出的分区不带进去
                Value ng; Check(tower::json::Parse(kBaseline, &ng), "ng+: parse player save");
                Value extra = Value::Obj(); extra.Set("CycleBonus", Value::Num(3));
                ng.Set("NgPlusSystem", extra);
                const std::string ngs = tower::json::Dump(ng);
                FakeIo nio; tower::trip::Trip nt; nio.tmpl = tower::json::Dump(tv);
                nio.baseline = ngs; nio.memory = ngs; nio.live = ngs;
                Check(nt.Enter(nio, kit, tower::save::DefaultEquip(kit), "s") && nt.usedTemplate(), "ng+: a save with an extra section still enters with the world template");
                Value nm; Check(tower::json::Parse(nio.memory, &nm), "ng+: tower save parses");
                Check(!nm.Find("NgPlusSystem") && nm.Find("PBZGameSystem")->Find("CurrentLevelName")->text == "TEMPLATE_WORLD", "ng+: the extra section is left out and the world is the template's");
                Check(nt.droppedKeys().size() == 1 && nt.droppedKeys()[0] == "NgPlusSystem", "ng+: dropped sections are reported");
                Check(nt.Exit(nio) && nio.memory == ngs, "ng+: exit restores the player's own NG+ save untouched");
            }
            Value bad = tv; bad.Erase("Inventory");
            FakeIo bio; tower::trip::Trip bt; bio.tmpl = tower::json::Dump(bad);
            Check(enter(bio, bt) && !bt.usedTemplate(), "template: broken template (no Inventory) falls back to the player's save");
        }
        {   // 塔内重开：已在塔档里 → 用基准 + 新塔档重生成，回标题继续后再次核验进塔档；不备份、不碰沙盒 / 基准
            FakeIo rio; tower::trip::Trip rt;
            Check(enter(rio, rt) && rt.phase() == tower::trip::Phase::Inside, "rebuild: precondition inside the tower save");
            rio.calls.clear();
            const auto before = rio.baseline;
            Check(rt.Rebuild(rio, kit, tower::save::DefaultEquip(kit), "s2") && rt.phase() == tower::trip::Phase::Entering, "rebuild: inside -> entering with a fresh tower save");
            Check(rio.baseline == before && rio.sandbox && std::find(rio.calls.begin(), rio.calls.end(), "backup") == rio.calls.end(), "rebuild: no backup, baseline and sandbox untouched");
            rio.ToTitle(); rt.Tick(rio); rio.Continue();
            bool in = false; for (int i = 0; i < 3 && !in; ++i) in = rt.Tick(rio);
            Check(in && rt.phase() == tower::trip::Phase::Inside, "rebuild: title reload verifies the new tower save -> inside again");
            tower::trip::Trip out;
            Check(!out.Rebuild(rio, kit, tower::save::DefaultEquip(kit), "s3") && out.phase() == tower::trip::Phase::Outside, "rebuild: refused outside the tower save");
        }
        // 出塔：13f7e98 反例——基准放回后又跨图回程（钟传送存档），旧判据会在此提前关沙盒
        Check(t.Exit(io) && io.memory == kBaseline && t.phase() == tower::trip::Phase::Exiting && io.sandbox, "trip: exit puts the baseline back, sandbox stays on");
        Check(io.calls.back() == "put_base" && io.calls[io.calls.size() - 2] == "sandbox", "trip: exit never re-takes the baseline");
        io.Teleport();
        for (int i = 0; i < 5; ++i) t.Tick(io);
        Check(t.phase() == tower::trip::Phase::Exiting && io.sandbox && io.ach, "trip counterexample: cross-map return after exit does NOT turn the sandbox off");
        Check(io.memory != kBaseline, "trip counterexample (fixture): the teleport really overwrote the baseline in memory");
        io.ToTitle(); t.Tick(io);
        Check(io.memory == kBaseline && io.calls.back() == "put_base", "trip: at the title the baseline is put back again (tower world gone)");
        io.Continue(); t.Tick(io);
        Check(t.phase() == tower::trip::Phase::Outside && !io.sandbox && !io.ach && io.live == kBaseline, "trip: real title reload of the original verifies -> sandbox off");
        // 反例：到标题时重放失败 + 载入的是塔内内容（新对象但旧塔内容）→ 核验拒绝、保持保护，可重试
        {
            FakeIo f; tower::trip::Trip u;
            enter(f, u);
            Check(u.phase() == tower::trip::Phase::Inside, "trip fixture: second run inside");
            u.Exit(f);
            f.Teleport();
            f.failTitlePut = true;
            f.ToTitle(); u.Tick(f); f.Continue();
            for (int i = 0; i < 3; ++i) u.Tick(f);
            Check(u.phase() == tower::trip::Phase::Exiting && f.sandbox && u.retries() >= 1 && f.live != kBaseline, "trip counterexample: new objects but tower content -> off refused, protection kept");
            Check(f.memory == kBaseline, "trip: after a failed verification the baseline is put back again");
            f.ToTitle(); u.Tick(f); f.Continue(); u.Tick(f);
            Check(u.phase() == tower::trip::Phase::Outside && !f.sandbox && f.live == kBaseline, "trip: retry after another title reload succeeds");
        }
        // 未回标题就一直等：不关保护
        {
            FakeIo f; tower::trip::Trip u;
            enter(f, u);
            u.Exit(f);
            for (int i = 0; i < 20; ++i) u.Tick(f);
            Check(u.phase() == tower::trip::Phase::Exiting && f.sandbox, "trip: without returning to the title the sandbox stays on indefinitely");
        }
        // 暂离 / 放弃 / 通关 / 失败在引擎里都走同一个 Exit；进塔途中取消也一样
        {
            FakeIo f; tower::trip::Trip u;
            f.baseline = kBaseline; f.memory = kBaseline; f.live = kBaseline;
            u.Enter(f, kit, tower::save::DefaultEquip(kit), "s");
            Check(u.Exit(f) && f.memory == kBaseline, "trip: cancel while entering puts the baseline back");
            f.ToTitle(); u.Tick(f); f.Continue(); u.Tick(f);
            Check(u.phase() == tower::trip::Phase::Outside && !f.sandbox, "trip: cancelled entry ends outside after verification");
        }
        // 状态落盘：没走完的读回来是 Stale；Stale 可以恢复（同样要回标题 + 核验）
        {
            FakeIo f; tower::trip::Trip inside;
            enter(f, inside);
            auto stale = tower::trip::Trip::Load(inside.Save());
            Check(stale.phase() == tower::trip::Phase::Stale && tower::trip::Trip::Load(t.Save()).phase() == tower::trip::Phase::Outside, "trip: unfinished session reloads as stale");
            Check(stale.Exit(f) && f.memory == kBaseline && f.sandbox, "trip: stale session puts the original back (sandbox re-armed, no new baseline)");
            Check(f.calls.back() == "put_base" && f.calls[f.calls.size() - 2] == "sandbox", "trip: stale recovery never snapshots the tower memory as a baseline");
            f.ToTitle(); stale.Tick(f); f.Continue(); stale.Tick(f);
            Check(stale.phase() == tower::trip::Phase::Outside && f.live == kBaseline, "trip: stale recovery verifies before ending");
            stale.Forget();
            Check(stale.phase() == tower::trip::Phase::Outside, "trip: forget clears the mark");
        }
        // 失败：没备份 / 替换失败都不进塔，不留沙盒
        FakeIo f1; f1.baseline = kBaseline; f1.failBackup = true;
        tower::trip::Trip a1;
        Check(!a1.Enter(f1, kit, tower::save::DefaultEquip(kit), "s") && a1.phase() == tower::trip::Phase::Outside && !f1.sandbox && f1.memory == "BASE", "trip: failed backup leaves everything untouched");
        FakeIo f2; f2.baseline = kBaseline; f2.failReplace = true;
        tower::trip::Trip b1;
        Check(!b1.Enter(f2, kit, tower::save::DefaultEquip(kit), "s") && b1.phase() == tower::trip::Phase::Outside && f2.memory == "BASE" && !f2.ach, "trip: failed replace undoes the protection");
        FakeIo f3; f3.baseline = "{}";
        tower::trip::Trip c1;
        Check(!c1.Enter(f3, kit, tower::save::DefaultEquip(kit), "s") && f3.memory == "BASE", "trip: baseline without inventory refuses to enter");
    }

    // ---- 自动切档（敲钟一键进出塔）
    {
        using tower::trip::Phase; using tower::trip::Step;
        auto fresh = [&](AutoIo& io, tower::trip::Trip& t) {
            io.baseline = kBaseline; io.memory = kBaseline; io.live = kBaseline;
            t.SetAuto(true);
            return t.Enter(io, kit, tower::save::DefaultEquip(kit), "s");
        };
        {
            AutoIo io; tower::trip::Trip t;
            Check(fresh(io, t) && t.phase() == Phase::Entering && t.step() == Step::Title && t.txn() == 1, "auto: enter puts the tower save and waits to go to the title");
            Check(!t.Enter(io, kit, tower::save::DefaultEquip(kit), "s") && t.txn() == 1 && t.error().find("正在进入") != std::string::npos, "auto: a second click while entering is refused, same transaction");
            bool entered = false;
            Run(io, t, 12, &entered);
            Check(entered && t.phase() == Phase::Inside && io.toTitle == 1 && io.cont == 1, "auto: one native title + one continue, no manual title menu");
            Check(io.live == io.memory && io.sandbox && io.ach, "auto: the loaded world is the tower save, protection stays on inside");
            Check(t.Exit(io) && !t.pending() && io.memory == kBaseline && t.txn() == 2, "auto: exit puts the original back at once when nothing is loading");
            Check(!t.Exit(io) && t.txn() == 2, "auto: a second exit while restoring is refused");
            Run(io, t, 12);
            Check(t.phase() == Phase::Outside && !io.sandbox && !io.ach && io.live == kBaseline && io.toTitle == 2 && io.cont == 2, "auto: exit returns to the title, continues and only then turns protection off");
            Check(fresh(io, t) && t.txn() == 3, "auto: repeated entry after a full round trip starts a new transaction");
            Run(io, t, 12);
            Check(t.phase() == Phase::Inside && io.toTitle == 3, "auto: repeated entry works");
        }
        {
            // 313987b 反例：跨图加载中暂离——放存档不能在加载中做、不能失败后就算完成
            AutoIo io; tower::trip::Trip t;
            fresh(io, t); Run(io, t, 12);
            io.busy = true;  // 跨图钟传送已发出，原生加载中
            const std::string before = io.memory;
            Check(t.Exit(io) && t.pending() && io.memory == before && t.phase() == Phase::Exiting && io.sandbox, "cancel during load: exit is accepted but the original is not put while loading");
            Run(io, t, 20);
            Check(t.pending() && io.toTitle == 1 && io.sandbox && !t.failed(), "cancel during load: no title request and no put until the load settles");
            io.Teleport();  // 加载落地：游戏把塔内状态存进内存
            io.busy = false;
            Run(io, t, 1);
            Check(!t.pending() && io.memory == kBaseline, "cancel during load: once settled the original is put");
            Run(io, t, 14);
            Check(t.phase() == Phase::Outside && !io.sandbox && io.live == kBaseline, "cancel during load: restored and verified before protection turns off");
        }
        {
            AutoIo io; tower::trip::Trip t;
            fresh(io, t); Run(io, t, 12);
            io.failReplace = true;
            Check(t.Exit(io) && t.pending() && !t.error().empty(), "put failure: exit is pending, not reported as done");
            Run(io, t, 3);
            Check(t.pending() && !t.failed() && io.sandbox, "put failure: retried with a gap, protection kept");
            Run(io, t, 20);
            Check(t.failed() && t.phase() == Phase::Exiting && io.sandbox && io.ach && t.error().find("第 5 次") != std::string::npos, "put failure: five failed puts stop with the reason, protection still on");
            const int asked = io.toTitle;
            Run(io, t, 20);
            Check(io.toTitle == asked && t.failed() && io.sandbox, "put failure: a failed transaction does nothing automatic and never turns protection off");
            io.failReplace = false;
            Check(t.Retry(io) && !t.failed() && io.memory == kBaseline && t.txn() == 3, "put failure: retry re-arms a new transaction and puts the original");
            Run(io, t, 14);
            Check(t.phase() == Phase::Outside && !io.sandbox, "put failure: retry completes");
        }
        {
            AutoIo io; tower::trip::Trip t;
            io.deadTitle = true;  // 原生返回标题没反应
            fresh(io, t);
            Run(io, t, 2);
            Check(io.toTitle == 1 && t.phase() == Phase::Entering, "dead title: first request sent");
            Run(io, t, 88);
            Check(io.toTitle == 1, "dead title: no repeat inside the 45 s window");
            Run(io, t, 3);
            Check(io.toTitle == 2 && !t.failed(), "dead title: asked once more after 45 s");
            Run(io, t, 92);
            Check(t.failed() && io.toTitle == 2 && io.sandbox && t.error().find("返回标题") != std::string::npos, "dead title: two unanswered requests stop with a reason, protection on");
            io.FakeIo::ToTitle(); Run(io, t, 1); io.FakeIo::Continue();
            bool entered = false; Run(io, t, 2, &entered);
            Check(entered && t.phase() == Phase::Inside, "dead title: the manual title / continue path still finishes the transaction (verified)");
        }
        {
            AutoIo io; tower::trip::Trip t;
            io.deadContinue = true;
            fresh(io, t);
            Run(io, t, 6);
            Check(io.title && io.cont == 1, "dead continue: continue requested after the title settled");
            Run(io, t, 200);
            Check(t.failed() && io.cont == 2 && io.sandbox, "dead continue: one retry, then stop with protection on");
        }
        {
            AutoIo io; tower::trip::Trip t;
            io.menu = false;  // 开始菜单一直没就绪
            fresh(io, t);
            Run(io, t, 60);
            Check(io.title && io.cont == 0 && !t.failed(), "title not ready: continue is not pressed early");
            Run(io, t, 450);
            Check(t.failed() && io.cont == 0 && io.sandbox && t.error().find("4 分钟") != std::string::npos, "title not ready: the 4 minute transaction limit stops it with a reason");
        }
        {
            AutoIo io; tower::trip::Trip t;
            io.busy = true;
            io.baseline = kBaseline; io.memory = kBaseline; io.live = kBaseline;
            t.SetAuto(true);
            Check(!t.Enter(io, kit, tower::save::DefaultEquip(kit), "s") && t.phase() == Phase::Outside && !io.sandbox && io.memory == kBaseline && io.calls.empty(), "enter while loading is refused before touching anything");
        }
        {
            // fa164d1 反例：原档核验通过后，锁里排队的原生存档在放开锁 1.28 秒后补存，之后又一次空闲自动存
            SettleIo io; tower::trip::Trip t;
            fresh(io, t); Run(io, t, 12);
            Check(t.phase() == Phase::Inside, "settle fixture: inside");
            t.Exit(io);
            bool loaded = false;
            for (int i = 0; i < 80 && t.phase() != Phase::Outside; ++i) {
                io.now += 500; t.Tick(io);
                if (!loaded && t.step() == Step::Settle) { loaded = true; }
                if (!loaded && io.cont == 2 && !io.title) { io.LoadedAtBell(); }  // 继续原档后：钟旁载入的原生行为
            }
            Check(t.phase() == Phase::Outside && io.realWrites == 0 && io.activity == 2 && t.flushed() >= 1, "settle: the queued and the idle native save both land in the sandbox, real save untouched");
            Check(!io.sandbox && !io.lock && !io.ach, "settle: protection turns off only after the saves settled");
            // 关保护之后空等 20 秒：没有新写入
            for (int i = 0; i < 40; ++i) { io.now += 500; io.Pump(); }
            Check(io.realWrites == 0, "settle: 20 s idle after exit writes nothing to the real save");
        }
        {
            // 原生存档一直脏（或一直在写）：90 秒后停在失败，沙盒仍开，原档不写；重试接着等，不重新载入
            SettleIo io; tower::trip::Trip t;
            fresh(io, t); Run(io, t, 12);
            t.Exit(io);
            for (int i = 0; i < 30 && t.step() != Step::Settle; ++i) { io.now += 500; t.Tick(io); }
            io.dirty = true;
            Run(io, t, 200);
            Check(t.failed() && io.sandbox && io.realWrites == 0 && t.error().find("90 秒") != std::string::npos, "settle: a save that never settles stops with the sandbox on");
            const int conts = io.cont;
            io.dirty = false;
            Check(t.Retry(io) && t.step() == Step::Settle, "settle: retry resumes waiting without reloading");
            Run(io, t, 12);
            Check(t.phase() == Phase::Outside && io.cont == conts && !io.sandbox && io.realWrites == 0, "settle: after the saves settle the exit completes");
        }
        {
            // 塔档核验一直不过：有界重试后停住，保护保持
            AutoIo io; tower::trip::Trip t;
            fresh(io, t);
            io.corrupt = true;  // 每次载入的内容都不对
            Run(io, t, 80);
            Check(t.failed() && t.retries() == tower::trip::Trip::kVerifies && io.sandbox, "verify keeps failing: stops after three verifications, protection on");
        }
    }

    // ---- Boss 准入
    {
        int iron = -1, hun = -1, hundun = -1;
        for (int i = 0; i < static_cast<int>(std::size(tower::kBossPool)); ++i) {
            const auto path = tower::kBossPool[i].path;
            if (path == tower::admission::kVerified[0]) iron = i;
            if (path == tower::admission::kVerified[1]) hundun = i;
            if (path == tower::admission::kDenied[0]) hun = i;
        }
        Check(iron >= 0 && hun >= 0 && hundun >= 0, "admission: verified / denied paths exist in the pool");
        auto admitted = [](int i) { return tower::admission::Admitted(tower::kBossPool[i].path, false, "", ""); };
        Check(admitted(iron) && admitted(hundun) && !admitted(hun), "admission: formal pool = verified only, mirror denied");
        Check(!tower::admission::Admitted(tower::kBossPool[hun].path, true, "", ""), "admission: denied stays out even in audit mode");
        Check(tower::admission::Admitted(tower::kBossPool[hun].path, false, std::string(tower::kBossPool[hun].path) + "\n", "") == false, "admission: built-in denial beats a local verified entry");
        Check(!tower::admission::Admitted(tower::kBossPool[iron].path, false, "", std::string(tower::kBossPool[iron].path) + "\tno bar\n"), "admission: local denial file removes a verified boss");
        const int pool = static_cast<int>(std::size(tower::kBossPool));
        // 准入是硬条件：多次抽取从不出未准入的，用完后在准入内重复
        tower::Rng rng(12345);
        std::vector<int> used;
        bool leak = false;
        for (int k = 0; k < 200; ++k) {
            const int b = tower::PickBoss(rng, 1 + k % 50, 50, pool, used, {}, admitted);
            leak |= b < 0 || !admitted(b);
            used.push_back(b);
        }
        Check(!leak, "admission: 200 picks never leave the admitted set (no fallback to the static pool)");
        Check(tower::PickBoss(rng, 1, 50, pool, {}, {}, [](int) { return false; }) == -1, "admission: nothing admitted -> -1");
        // 每层 Boss：固定只有一只时 1v2 层退成 1v1；不固定时多 Boss 层用准入内不同身份
        tower::Run r;
        r.Start(7, 3);
        int got = r.NextFloor(pool, {}, {}, [iron](int c) { return c == iron; }, 1);
        Check(got == 1 && r.bosses.size() == 1 && r.bosses[0] == iron, "fixed acceptance boss: every floor is that boss, 1v1");
        r.phase = tower::Phase::Ready;
        got = r.NextFloor(pool, [](int a, int b) { return tower::kBossPool[a].identity != tower::kBossPool[b].identity && tower::kBossPool[b].cls == 'A'; }, {}, [iron](int c) { return c == iron; }, 3);
        Check(got == 0 && r.floor == 1 && r.phase == tower::Phase::Ready, "duo floor with one admitted boss: refused and rolled back (engine then retries as 1v1)");
        // 固定 Boss 入口也服从禁用：引擎的准入条件是 Admitted(c) && c == fixed
        auto fixedAdmit = [&](int fixed) { return [&, fixed](int c) { return admitted(c) && c == fixed; }; };
        tower::Run rh;
        rh.Start(9, 3);
        Check(rh.NextFloor(pool, {}, {}, fixedAdmit(hun), 1) == 0, "fixed boss = Hun is still refused by the deny list");
        Check(rh.NextFloor(pool, {}, {}, fixedAdmit(iron), 1) == 1 && rh.bosses[0] == iron, "fixed boss = verified boss works");
        // 审计计划
        tower::plan::PoolView pv;
        pv.size = pool;
        pv.path = [](int i) { return tower::kBossPool[i].path; };
        pv.identity = [](int i) { return tower::kBossPool[i].identity; };
        pv.admitted = [&](int i) { return tower::admission::Admitted(tower::kBossPool[i].path, true, "", ""); };  // pool:audit
        std::vector<tower::plan::Floor> plan;
        std::string why;
        Check(tower::plan::Parse("IronMaskMan_Boss;XiaoXue@here;" + std::string(tower::kBossPool[hundun].path), pv, &plan, &why) &&
              plan.size() == 3 && plan[1].bosses.size() == 1 && plan[1].here && !plan[2].stagger, "plan: varied solo bosses and arenas");
        Check(tower::plan::Parse("XiaoXue@rongshu;IronMaskMan_Boss", pv, &plan, &why) && plan.size() == 2 && plan[0].venue == "fixed:rongshu_zdy" && plan[1].venue.empty(),
              "plan: @rongshu puts that floor in the fixed Rongshu venue only");
        Check(!tower::plan::Parse("XiaoXue@rongshu2", pv, &plan, &why), "plan counterexample: unknown venue option rejected");
        Check(tower::plan::Parse("XiaoXue@endless;IronMaskMan_Boss@endless,rongshu", pv, &plan, &why) && plan.size() == 2 && plan[0].endless && plan[0].venue.empty() &&
              plan[1].endless && plan[1].venue == "fixed:rongshu_zdy", "plan: @endless marks a mixed-wave floor; the named boss only picks the venue");
        Check(!tower::plan::Parse("IronMaskMan_Boss+XiaoXue", pv, &plan, &why) && plan.empty(), "plan counterexample: two solo bosses are not an authored coordinated encounter");
        Check(!tower::plan::Parse("IronMaskMan_Boss+XiaoXue+AHua_GuiTouDao@stagger", pv, &plan, &why), "plan counterexample: staggering does not authorize three solo bosses");
        Check(!tower::plan::Parse("Hun", pv, &plan, &why) && why.find("禁用") != std::string::npos, "plan counterexample: Hun mirror rejected even in audit pool");
        Check(!tower::plan::Parse("IronMaskMan_Boss+IronMaskMan_Boss", pv, &plan, &why), "plan counterexample: same identity twice in a floor rejected");
        Check(!tower::plan::Parse("EMei_Sword", pv, &plan, &why) && why.find("不唯一") != std::string::npos, "plan counterexample: ambiguous identity must be a class path");
        Check(!tower::plan::Parse("NoSuchBoss", pv, &plan, &why), "plan counterexample: unknown boss rejected");
        Check(!tower::plan::Parse("IronMaskMan_Boss@fast", pv, &plan, &why), "plan counterexample: unknown option rejected");
        Check(!tower::plan::Parse("A;B;C;D;E;F;G;H;I;J;K", pv, &plan, &why), "plan counterexample: more than 10 floors rejected");
        pv.admitted = [&](int i) { return admitted(i); };  // 正式准入：只有验证过的
        Check(!tower::plan::Parse("XiaoXue", pv, &plan, &why) && why.find("未准入") != std::string::npos, "plan counterexample: unverified boss rejected in the verified pool (no padding with unverified content)");
        Check(tower::plan::Parse(std::string(tower::kBossPool[hundun].path) + "@nostagger;IronMaskMan_Boss", pv, &plan, &why) && plan.size() == 2, "plan: verified-only plan accepted");
    }

    // ---- 本机真实基准（有就跑；只看结构，不输出内容）
    if (const char* la = std::getenv("LOCALAPPDATA")) {
        std::ifstream f(std::string(la) + "\\PBZTower\\baseline.json", std::ios::binary);
        if (f) {
            std::stringstream ss; ss << f.rdbuf();
            Value real;
            const bool parsed = tower::json::Parse(ss.str(), &real);
            Check(parsed, "local baseline: real save JSON parses");
            if (parsed) {
                Value back;
                Check(tower::json::Parse(tower::json::Dump(real), &back) && tower::json::Dump(back) == tower::json::Dump(real), "local baseline: round trip stable");
                const auto r = tower::save::Build(real, kit, tower::save::DefaultEquip(kit), "2026.10.31-00.00.00");
                Check(r.ok && r.save.Find("Inventory")->Find("MainWeaponInfos")->fields.size() == 2 && r.save.Find("Player")->Find("Dust")->Int() == 0, "local baseline: tower save builds from the real baseline");
                std::printf("info: local baseline %zu chars -> tower save %zu chars\n", ss.str().size(), tower::json::Dump(r.save).size());
            }
        } else std::printf("info: no local baseline.json, real-save check skipped\n");
    }

    if (g_failed) { std::printf("TOWER_SAVE_FAILED %d/%d\n", g_failed, g_checks); return 1; }
    std::printf("TOWER_SAVE_OK %d checks (no game process)\n", g_checks);
    return 0;
}
