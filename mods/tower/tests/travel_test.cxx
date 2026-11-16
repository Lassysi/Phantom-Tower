// 影塔传送状态机离线测试（假游戏端，不碰游戏）
#include <cstdio>
#include <cstdlib>
#include <string>

#include "../src/tower_travel.h"
#include "../src/tower_arena.h"

using namespace tower::travel;

static int g_checks = 0;
#define CHECK(c) do { ++g_checks; if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); std::exit(1); } } while (0)

// 假游戏：钟传送后 loadFrames 帧没有玩家，然后出现在钟旁；点传送在 busyFrames 帧后到达（或 failPoint 时传回原处）
struct Fake : IMove {
    Pose pos; std::string level = "PBZ_Xigu_WP";
    int loading = 0, busy = 0, bellCalls = 0, pointCalls = 0, failPoints = 0, cover = 0;  // cover：玩家已在新关卡后原生加载遮罩还剩几次 Ready 查询
    bool faultPoint = false;  // 点传送在运行库里遇到原生异常
    Pose pendingPoint, origin; bool pointPending = false;
    std::string pendingLevel; Pose pendingBell;
    bool Where(Pose* p, std::string* lv) override {
        if (loading > 0) { if (--loading == 0) { level = pendingLevel; pos = pendingBell; } return false; }
        *p = pos; *lv = level; return true;
    }
    std::string BellLevel(const std::string& b) override {
        if (b == "Bell_NLES_004" || b == "Bell_NLES_001") return "PBZ_Xigu_WP";
        if (b == "Bell_SZ_003") return "PBZ_Pangzhen_WP";
        return "";
    }
    bool Bell(const std::string& b, const Pose& at) override { ++bellCalls; pendingLevel = BellLevel(b); pendingBell = at; loading = 30; return true; }
    bool Ready() override { if (cover > 0) { --cover; return false; } return true; }
    bool Point(const Pose& at, std::string* msg) override {
        if (faultPoint) { ++pointCalls; *msg = std::string(kNativeFault) + "落点查询异常，已取消"; return false; }
        ++pointCalls; origin = pos; pendingPoint = at; pointPending = true; busy = 10; return true; }
    bool Busy() override {
        if (!pointPending) return false;
        if (--busy > 0) return true;
        pointPending = false;
        if (failPoints > 0) { --failPoints; pos = origin; } else pos = pendingPoint;  // 悬停没等到地面：传回原处
        return false;
    }
};

// 和游戏端 Travel::Tick 一样：只在 Active() 时推进（首帧也要推进，否则永远不动）
static State Run(Traveler& t, Fake& g, int frames = 2000) {
    for (int i = 0; i < frames && t.Active(); ++i) t.Step(g, 1.0 / 30);
    return t.state();
}

int main() {
    // 场地回退与战斗边界（tower_arena.h）
    {
        namespace ar = tower::arena;
        auto yes = [](int) { return true; }; auto no = [](int) { return false; };
        CHECK(ar::ChooseVenue(true, false, true, 0, no).kind == ar::Kind::Native);
        CHECK(ar::ChooseVenue(true, true, false, 2, yes).kind == ar::Kind::Arena && ar::ChooseVenue(true, true, false, 2, yes).arena == 2);
        CHECK(ar::ChooseVenue(true, true, false, 2, no).kind == ar::Kind::None);    // 竞技场没验证：失败关闭
        CHECK(ar::ChooseVenue(true, true, true, 2, yes).kind == ar::Kind::None);    // 吃场景的不搬去竞技场
        CHECK(ar::ChooseVenue(false, true, false, -1, yes).kind == ar::Kind::None); // 本区没有竞技场
        CHECK(ar::ParseVerified("arena_z1_1  # 32ffb7d\n\n# x\narena_z2_1\r\n").size() == 2);
        ar::Boundary b; b.Arm({0, 0, 0}, 3000);
        CHECK(b.Player({2000, 0, 0}) == ar::Act::None && b.Player({3100, 0, 0}) == ar::Act::PushBack && b.Player({0, 0, 2000}) == ar::Act::PushBack);
        const auto in = b.Inside({6000, 0, 0});
        CHECK(std::abs(in.x - 2800) < 1 && std::abs(in.y) < 1);                   // 放回边界内 2 米
        // 39 层寒岁：Boss 越界先拉回（5 秒，最多 3 次），拉回用完仍在界外满 15 秒才作废
        // 41 层雷婷会自己瞬移出雾门：出界 3 秒拉回，成功不封顶；连续 3 次拉回失败后仍在界外满 15 秒才作废
        CHECK(b.Boss({5000, 0, 0}, 10) == ar::Act::None && b.Boss({5000, 0, 0}, 12) == ar::Act::None && b.Boss({5000, 0, 0}, 13) == ar::Act::PullBack);
        for (int i = 0; i < 6; ++i) b.Pulled(13, true);
        CHECK(b.pulls == 6 && b.failedPulls == 0 && b.Boss({5000, 0, 0}, 16) == ar::Act::PullBack);   // 拉回成功不封顶
        // 1.1.1 18 层疯医：每次拉回试 4 个候选落点，连败 10 次（不是 3 次）后仍在界外满 15 秒才作废
        b.Pulled(20, false); b.Pulled(20, false); b.Pulled(20, false);
        CHECK(b.failedPulls == 3 && b.Boss({5000, 0, 0}, 23) == ar::Act::PullBack);
        for (int i = 0; i < 7; ++i) b.Pulled(20, false);
        CHECK(b.failedPulls == 10 && b.Boss({5000, 0, 0}, 34) == ar::Act::None && b.Boss({5000, 0, 0}, 35) == ar::Act::Void);
        CHECK(b.Boss({100, 0, 0}, 36) == ar::Act::None && b.Boss({5000, 0, 0}, 37) == ar::Act::None);  // 回到区内重新计时
        b.Arm({0, 0, 0}, 3000); CHECK(b.pulls == 0);                                   // 重新开层清零
        // 软边：越界不超过 slack 不推人、不算 Boss 出界
        b.slack = 1000; CHECK(b.Player({3900, 0, 0}) == ar::Act::None && b.Player({4100, 0, 0}) == ar::Act::PushBack);
        { const auto deep = b.InsideDeep({6000, 0, 0}, 0.5); CHECK(std::abs(deep.x - 1400) < 1 && std::abs(deep.y) < 1); }
        // 2026-11-12：严格边界与「远超软边」分开判定；拉回计时由外部判定驱动（雾门外 1.5 秒拉回，雾门内不拉）
        CHECK(b.OutsideBy({3500, 0, 0}, 0) && !b.Outside({3500, 0, 0}) && !b.OutsideBy({5400, 0, 0}, b.slack + 1500) && b.OutsideBy({5600, 0, 0}, b.slack + 1500));
        CHECK(b.BossAt(false, 50, 1.5) == ar::Act::None && b.BossAt(true, 51, 1.5) == ar::Act::None && b.BossAt(true, 52.6, 1.5) == ar::Act::PullBack);
        CHECK(b.BossAt(false, 53, 1.5) == ar::Act::None && b.BossAt(true, 53.5, 1.5) == ar::Act::None);  // 回到雾门内重新计时
        b.slack = 0;
        b.Disarm(); CHECK(b.Player({9000, 0, 0}) == ar::Act::None);
    }
    // 场内起点判定（313987b 第 4 层雾门反例：同图 40 米内旧判据视为已就位）
    CHECK(!AtSpot({3000, 0, 0, 0}, {0, 0, 0, 0}, 600));    // 雾门边 30 米：要回场内起点
    CHECK(!AtSpot({0, 0, 500, 0}, {0, 0, 0, 0}, 600));     // 同 XY 高 5 米（高台 / 上层）：不算
    CHECK(AtSpot({300, 200, 100, 0}, {0, 0, 0, 0}, 600));  // 起点旁 4 米：不必再传
    // 擂台选择：同区同种子稳定；已标定的优先
    Centers none;
    const int a = ChooseArena(1, 42, none);
    CHECK(a >= 0 && tower::kArenas[a].zone == 1 && ChooseArena(1, 42, none) == a);
    CHECK(tower::kArenas[a].id == "arena_z1_1");  // 没标定：每区首选
    Centers marked{{"arena_z1_2", {{1, 2, 3, 4}, ""}}};
    const int b = ChooseArena(1, 42, marked);
    CHECK(tower::kArenas[b].id == "arena_z1_2");
    CHECK(ArenaDest(b, marked).target.x == 1 && ArenaDest(a, none).target.x == tower::kArenas[a].x);
    CHECK(ChooseArena(9, 1, none) == -1);
    // 标定文件往返
    const auto c = ParseCenters(FormatCenters(Centers{{"arena_z1_1", {{10.5, -20, 30, 90}, ""}}, {"boss:MoTian", {{1, 2, 3, 4}, "PBZ_Xigu_WP"}}}) + "garbage\n# x 1 2 3 4\n");
    CHECK(c.size() == 2 && c.at("arena_z1_1").at.y == -20 && c.at("arena_z1_1").level.empty() && c.at("boss:MoTian").level == "PBZ_Xigu_WP");
    CHECK(ParseCenters("old 1 2 3 4\n").at("old").level.empty());  // 旧格式（无关卡）照样读

    // Boss 原生场地：没标定 → 没有；标定了 → 关卡里最近的钟补上；从别的关卡过去：钟 → 点
    {
        Dest d;
        CHECK(!BossDest("MoTian", "魔渊", none, &d));
        Centers cs{{BossKey("MoTian"), {{3000, 279000, -32000, 0}, "PBZ_Xigu_WP"}}};
        CHECK(BossDest("MoTian", "魔渊", cs, &d) && d.level == "PBZ_Xigu_WP" && d.bell.empty());
        Fake g; g.level = "PBZ_Pangzhen_WP";
        FillBell(&d, [&](const std::string& id) { return g.BellLevel(id); });
        CHECK(d.bell.rfind("Bell_NLES", 0) == 0);
        Traveler t; t.Go(d);
        CHECK(Run(t, g) == State::Done && g.level == "PBZ_Xigu_WP" && g.pos.x == 3000 && g.bellCalls == 1);
        // 标定的关卡没有已知的钟：失败，不乱传
        Dest bad; Centers cb{{BossKey("X"), {{1, 1, 1, 0}, "PBZ_Nowhere"}}};
        CHECK(BossDest("X", "X", cb, &bad));
        FillBell(&bad, [&](const std::string& id) { return g.BellLevel(id); });
        Fake g2; t.Go(bad); CHECK(Run(t, g2) == State::Failed && g2.bellCalls == 0);
    }

    // 原生场地（游戏数据）：魔渊在悲泣溪谷的天堡；没有数据的路径返回 false
    {
        Dest d;
        CHECK(NativeDest("/PBZCombatAsset/ActorData/Mobs/067_MoTian/BP_Monster_MoTian.BP_Monster_MoTian_C", "魔渊", &d) && d.level == "PBZ_Xigu_WP" && d.target.z > 1e6);
        CHECK(!NativeDest("/Game/Nothing.Nothing_C", "x", &d));
        {  // 固定场地（榕树内场，实机采样）：落点、刷怪点、实测边缘都在圆 + 软边内；短 id 与类路径都不当成 Boss 场地
            Dest f;
            CHECK(FixedDest("fixed:rongshu_zdy", &f) && f.level == "PBZ_Zhengdaodaying_WP" && f.target.z > 2e6 && f.resolveSurfaceHeight && f.arrive > 0 && f.arrive < 600);
            const auto* v = tower::FindFixedVenue("fixed:rongshu_zdy");
            CHECK(v && tower::FindFixedVenueById("rongshu") == v && !tower::FindFixedVenue("rongshu") && !FixedDest(tower::kNativeSpots[0].path, &f));
            tower::arena::Boundary b; b.Arm({v->cx, v->cy, v->sz}, v->radius); b.slack = 1000;
            CHECK(b.Contains({v->px, v->py, v->pz}, 0) && b.Contains({v->sx, v->sy, v->sz}, 0));
            for (auto e : {tower::arena::P3{-2814.957, 103488.924, v->pz}, {3042.271, 102797.188, v->pz}, {738.663, 105315.242, v->pz}, {-783.839, 101927.390, v->pz}})
                CHECK(std::hypot(e.x - v->cx, e.y - v->cy) <= v->radius + b.slack);
            CHECK(std::hypot(-818.399 - v->cx, 100890.714 - v->cy) > v->radius);  // 原生复活点在缺口外
            // 实机反例：落在钟旁 (-136, 101087) 离落点 8 米、在圈外；固定场地的到达容差必须小于它，不能当成已到
            CHECK(std::hypot(-136.3 - v->px, 101086.6 - v->py) > f.arrive && std::hypot(-136.3 - v->cx, 101086.6 - v->cy) > v->radius);
        }
        for (auto& s : tower::kNativeSpots) CHECK(s.level != "PSOCombat");
    }

    // Go 之后、第一步之前就算进行中；结束后不再进行中；Reset 清掉
    {
        Traveler t; CHECK(!t.Active());
        Dest d; d.level = "PBZ_Xigu_WP"; d.target = {90000, 0, 0, 0};
        t.Go(d); CHECK(t.Active());
        Fake g; CHECK(Run(t, g) == State::Done && !t.Active());
        t.Go(d); t.Reset(); CHECK(!t.Active());
    }

    // 同关卡：直接点传送
    {
        Fake g; g.pos = {0, 0, 0, 0};
        Traveler t; Dest d; d.bell = "Bell_NLES_004"; d.target = {50000, 0, 0, 0};
        t.Go(d);
        CHECK(Run(t, g) == State::Done && g.bellCalls == 0 && g.pointCalls == 1);
    }
    // 已经在目的地：不动
    {
        Fake g; g.pos = {100, 100, 0, 0};
        Traveler t; Dest d; d.bell = "Bell_NLES_004"; d.target = {200, 200, 0, 0};
        t.Go(d);
        CHECK(Run(t, g) == State::Done && g.pointCalls == 0 && g.bellCalls == 0);
    }
    // 跨关卡：钟传送 → 加载期间没有玩家 → 新关卡稳定后点传送到擂台中心
    {
        Fake g; g.pos = {0, 0, 0, 0};
        Traveler t; const auto d = ArenaDest(b, marked);  // PangZhen，中心 (1,2,3)
        t.Go(d);
        CHECK(Run(t, g) == State::Done && g.bellCalls == 1 && g.pointCalls == 1 && g.level == "PBZ_Pangzhen_WP" && g.pos.x == 1);
    }
    // 跨关卡、中心就在钟旁：钟传送后不再点传送
    {
        Fake g;
        Traveler t; t.Go(ArenaDest(b, none));
        CHECK(Run(t, g) == State::Done && g.bellCalls == 1 && g.pointCalls == 0);
    }
    // 悬停没等到地面传回原处：重试一次；两次都失败 → Failed
    {
        Fake g; g.failPoints = 1;
        Traveler t; Dest d; d.level = "PBZ_Xigu_WP"; d.target = {90000, 0, 0, 0};
        t.Go(d);
        CHECK(Run(t, g) == State::Failed && g.pointCalls == 1 && t.stop());
        Fake g2; g2.failPoints = 5;
        t.Go(d);
        CHECK(Run(t, g2) == State::Failed && g2.pointCalls == 1 && !t.why().empty() && t.stop());
        // 34 层力者断魂冰面：运行库地面查询两次拒绝 → 直接放到落点一次，到位即算到达（地面交给塔的落地审计）
        struct FakeDirect : Fake { int directCalls = 0; bool Direct(const Pose& at, std::string*) override { ++directCalls; pos = at; return true; } };
        FakeDirect g3; g3.failPoints = 5;
        t.Go(d);
        CHECK(Run(t, g3) == State::Done && g3.pointCalls == 1 && g3.directCalls == 1);  // 第一次被拒就直接放，不再悬空第二轮
    }
    // 钟所在关卡未知 / 不在目标关卡又没钟：失败而不是乱传
    {
        Fake g; Traveler t; Dest d; d.bell = "Bell_Unknown"; d.target = {1, 1, 1, 0};
        t.Go(d); CHECK(Run(t, g) == State::Failed && g.pointCalls == 0);
        Dest h; h.level = "PBZ_Pangzhen_WP"; h.target = {1, 1, 1, 0};
        t.Go(h); CHECK(Run(t, g) == State::Failed && g.pointCalls == 0 && g.bellCalls == 0);
    }
    // 加载一直不结束：超时失败
    {
        Fake g; Traveler t; t.Go(ArenaDest(b, none));
        t.Step(g, 0); g.loading = 1 << 30;
        CHECK(Run(t, g, 10000) == State::Failed);
    }
    // 回家：同关卡最近的钟；格式往返；从别的关卡回去走钟再点传送
    {
        Home h; h.level = "PBZ_Xigu_WP"; h.at = {1000, 280000, -32000, 10};
        h.bell = NearestBell(h.at, h.level, [](const std::string& id) { return id.rfind("Bell_NLES", 0) == 0 ? std::string("PBZ_Xigu_WP") : std::string("x"); });
        CHECK(h.bell == "Bell_NLES_001");
        const auto back = ParseHome(FormatHome(h));
        CHECK(back.valid && back.bell == h.bell && back.at.y == 280000 && back.level == h.level);
        CHECK(!ParseHome("").valid);
        Fake g; g.level = "PBZ_Pangzhen_WP"; g.pos = {0, 0, 0, 0};
        Traveler t; t.Go(HomeDest(back));
        CHECK(Run(t, g) == State::Done && g.level == "PBZ_Xigu_WP" && g.pos.x == 1000 && g.bellCalls == 1 && g.pointCalls == 1);
    }
    // 跨图安全（7c2668e 第三层黑屏）：新关卡里遮罩未结束不查落点；结束后才点传送
    {
        Fake g; g.pos = {0, 0, 0, 0}; g.cover = 200;
        Traveler t; t.Go(ArenaDest(b, marked));
        for (int i = 0; i < 2000 && t.Active() && g.cover > 0; ++i) { t.Step(g, 1.0 / 30); CHECK(g.pointCalls == 0); }
        CHECK(Run(t, g) == State::Done && g.pointCalls == 1 && !t.stop());
    }
    // 遮罩一直不结束：超时失败且停机（不让外层换 Boss 再跨图）
    {
        Fake g; g.pos = {0, 0, 0, 0}; g.cover = 1 << 30;
        Traveler t; t.Go(ArenaDest(b, marked));
        CHECK(Run(t, g, 10000) == State::Failed && g.pointCalls == 0 && t.stop());
    }
    // 原生异常：一次就失败停机，不重试、不再点传送
    {
        Fake g; g.pos = {0, 0, 0, 0}; g.faultPoint = true;
        Traveler t; t.Go(ArenaDest(b, marked));
        CHECK(Run(t, g) == State::Failed && g.pointCalls == 1 && t.stop() && t.why().rfind(kNativeFault, 0) == 0);
        Fake g2; g2.pos = {0, 0, 0, 0}; g2.faultPoint = true;
        Dest d; d.level = "PBZ_Xigu_WP"; d.target = {90000, 0, 0, 0};
        t.Go(d); CHECK(Run(t, g2) == State::Failed && g2.pointCalls == 1 && t.stop());
        // 普通失败（悬停没等到地面）不停机：外层照旧换 Boss
        Fake g3; g3.failPoints = 5;
        t.Go(d); CHECK(Run(t, g3) == State::Failed && t.stop());
        t.Go(d); CHECK(!t.stop());  // 下一次 Go 清掉
    }
    // 同关卡但遮罩未结束：先等再点传送
    {
        Fake g; g.cover = 50;
        Traveler t; Dest d; d.level = "PBZ_Xigu_WP"; d.target = {90000, 0, 0, 0};
        t.Go(d); t.Step(g, 0);
        CHECK(g.pointCalls == 0 && t.Active());
        CHECK(Run(t, g) == State::Done && g.pointCalls == 1);
    }
    // F39: horizontal proximity must not accept a void fall or another world's respawn.
    {
        struct WrongHeight : Fake {
            bool Busy() override { const bool b=Fake::Busy(); if(!b && pointCalls) pos.z=1000000; return b; }
        } g;
        Traveler t; Dest d; d.level="PBZ_Xigu_WP"; d.target={90000,0,0,0};
        t.Go(d);CHECK(Run(t,g)==State::Failed && t.stop());
        struct WrongWorld : Fake {
            bool Busy() override { const bool b=Fake::Busy(); if(!b && pointCalls) level="OtherWorld"; return b; }
        } other;
        t.Go(d);CHECK(Run(t,other)==State::Failed && t.stop());
        d.target.z=NAN;Fake invalid;t.Go(d);CHECK(Run(t,invalid)==State::Failed && invalid.pointCalls==0 && t.stop());
        tower::arena::Boundary bound;bound.Arm({0,0,100},3500);
        const auto recovered=bound.InsideDeep({2000,0,31638902},.6);
        CHECK(!bound.Outside(recovered) && std::abs(recovered.z-100)<=50);
        CHECK(bound.Outside({NAN,0,100}) && !bound.Contains({0,0,NAN},200));
    }
    // 2026-11-11 实机：同场地上一场打完人在雾门边 9 米（> 开层 6 米、< 到达 15 米），必须真的点传送回起点
    {
        Fake g; g.pos = {-43772, 161543, -215, 0};
        Dest d; d.level = "PBZ_Xigu_WP"; d.target = {-42983, 162021, -304, 0};
        Traveler t; t.Go(d);
        CHECK(Run(t, g) == State::Done && g.pointCalls == 1 && AtSpot(g.pos, d.target, 600));
        Fake near; near.pos = {-42900, 162000, -304, 0};  // 起点旁 1 米：不传送
        t.Go(d); CHECK(Run(t, near) == State::Done && near.pointCalls == 0);
    }
    // Regression from live 174792c F37: authored center 5860.2 is not the
    // standing capsule's ~5410 height. Core already landed; Direct must not run.
    {
        struct GroundAdjusted : Fake {
            int directCalls=0,groundCalls=0; bool supported=true;
            double heightOffset=-450.073259832263,xyOffset=0;
            bool otherWorld=false;
            bool Busy() override {
                const bool b=Fake::Busy();
                if(!b && pointCalls){pos=pendingPoint;pos.z+=heightOffset;pos.x+=xyOffset;if(otherWorld)level="OtherWorld";}
                return b;
            }
            bool StandingAt(const Pose&) override {++groundCalls;return supported;}
            bool Direct(const Pose&,std::string*) override {++directCalls;return false;}
        } g;
        Dest d;d.level="PBZ_Xigu_WP";d.target={-203843.2,86036.9,5860.2,0};d.resolveSurfaceHeight=true;
        Traveler t;t.Go(d);
        CHECK(Run(t,g)==State::Done && g.pointCalls==1 && g.directCalls==0 && g.groundCalls==1);
        CHECK(std::abs(t.dest().target.z-5410.126740167737)<.001 && AtSpot(g.pos,t.dest().target,Traveler::kArrive));
        GroundAdjusted airborne;airborne.supported=false;t.Go(d);CHECK(Run(t,airborne)==State::Failed && t.stop());
        GroundAdjusted far;far.heightOffset=1e6;t.Go(d);CHECK(Run(t,far)==State::Failed && far.groundCalls==0);
        GroundAdjusted sideways;sideways.xyOffset=301;t.Go(d);CHECK(Run(t,sideways)==State::Failed && sideways.groundCalls==0);
        GroundAdjusted wrong;wrong.otherWorld=true;t.Go(d);CHECK(Run(t,wrong)==State::Failed && wrong.groundCalls==0);
        GroundAdjusted markedPoint;d.resolveSurfaceHeight=false;t.Go(d);CHECK(Run(t,markedPoint)==State::Failed && markedPoint.groundCalls==0);
        // Same XY, other level: a failed asynchronous teleport returning to
        // the original supported position is NOT a successful destination.
        struct ReturnedOrigin : Fake { bool StandingAt(const Pose&) override{return true;} } back;
        d.resolveSurfaceHeight=true;back.pos=d.target;back.pos.z-=500;back.failPoints=1;
        t.Go(d);CHECK(Run(t,back)==State::Failed && t.stop());
        // 1.1.1 42 层蛊鹤高空平台：刚落地时"站稳"还不成立、直接放置也被拒，几秒后站稳了 → 宽限期内判到达，不停机跳层
        struct LateFooting : GroundAdjusted { int asks=0; bool StandingAt(const Pose&) override {++groundCalls;return ++asks>90;} } late;
        late.heightOffset=-600;t.Go(d);CHECK(Run(t,late)==State::Done && late.directCalls>=1 && !t.stop());
        GroundAdjusted gate;gate.cover=50;t.Go(d);CHECK(Run(t,gate)==State::Done && gate.pointCalls==1 && gate.directCalls==0);
        GroundAdjusted reuse;t.Go(d);CHECK(Run(t,reuse)==State::Done);reuse.pos={0,0,0,0};reuse.supported=false;t.Go(d);CHECK(Run(t,reuse)==State::Failed);
        CHECK(!HomeDest(Home{}).resolveSurfaceHeight && !ArenaDest(a,none).resolveSurfaceHeight);
        Dest native;CHECK(NativeDest(tower::kNativeSpots[0].path,"test",&native) && native.resolveSurfaceHeight);
    }
    // 39 层寒岁：场地单元只在剧情数据层 DL_3721Unload 里（初始 Unloaded）。点传送前必须先激活；激活失败停机，不点传送、不直接放
    {
        const std::string_view hansui = "/PBZCombatAsset/ActorData/Mobs/QiShu/BP_Monster_QiShu_Bing.BP_Monster_QiShu_Bing_C";
        CHECK(NativeLayerFor(hansui) == "DataLayer_946171F64F6F9FF290B62AA52648BE79");
        CHECK(NativeLayerFor("/PBZCombatAsset/ActorData/Mobs/QiShu/BP_Monster_QiShu_Bing_Phantom.BP_Monster_QiShu_Bing_Phantom_C").empty());
        Dest hd; const bool haveSpot = NativeDest(hansui, "寒岁", &hd);
        if (haveSpot) CHECK(hd.dataLayer == "DataLayer_946171F64F6F9FF290B62AA52648BE79" && hd.level == "PBZ_Pangzhen_WP");
        for (auto& s : tower::kNativeSpots) if (s.path != hansui) { Dest o; CHECK(NativeDest(s.path, "x", &o) && o.dataLayer.empty()); }
        struct Layered : Fake {
            int ensureCalls = 0, pointsBeforeLayer = 0; bool allow = true, active = false;
            bool EnsureLayer(const std::string& n, std::string* msg) override {
                ++ensureCalls; if (!allow) { *msg = "当前世界里找不到数据层 " + n; return false; } active = true; return true; }
            bool Point(const Pose& at, std::string* msg) override { if (!active) ++pointsBeforeLayer; return Fake::Point(at, msg); }
        };
        Dest d; d.name = "寒岁的原生场地"; d.level = "PBZ_Pangzhen_WP"; d.target = {5000, 0, 0, 0}; d.dataLayer = "DataLayer_946171F64F6F9FF290B62AA52648BE79";
        Traveler t;
        Layered ok; ok.level = "PBZ_Pangzhen_WP"; t.Go(d);
        CHECK(Run(t, ok) == State::Done && ok.ensureCalls == 1 && ok.pointsBeforeLayer == 0 && ok.pointCalls == 1);
        Layered no; no.level = "PBZ_Pangzhen_WP"; no.allow = false; t.Go(d);
        CHECK(Run(t, no) == State::Failed && t.stop() && no.pointCalls == 0 && t.why().find("数据层") != std::string::npos);
        // 跨图：到了庞镇、遮罩结束后才激活（激活要在目标世界里找层）
        Layered cross; cross.level = "PBZ_Xigu_WP"; Dest c = d; c.bell = "Bell_SZ_003"; c.bellPose = {100, 0, 0, 0}; t.Go(c);
        CHECK(Run(t, cross) == State::Done && cross.bellCalls == 1 && cross.ensureCalls == 1 && cross.pointsBeforeLayer == 0);
        // 不带数据层的落点不调用
        Layered plain; plain.level = "PBZ_Pangzhen_WP"; Dest pd = d; pd.dataLayer.clear(); t.Go(pd);
        CHECK(Run(t, plain) == State::Done && plain.ensureCalls == 0);
    }
    {  // 9 层拙壁（1.1.1）：人和 Boss 落到雾门凸包中心两侧各 4 米，都在凸包里；别的 Boss 不受影响
        const std::string_view zb = "/Game/RenewalContent/Actors/Mobs/Normal/BP_Monster_Zhuobi.BP_Monster_Zhuobi_C";
        CenterPair c;
        CHECK(CenteredPair(zb, &c));
        CHECK(std::abs(DistXY(c.player, c.boss) - 2 * kCenterGap) < 1);
        const tower::VenueDef* v = nullptr;
        for (auto& x : tower::kVenues) if (x.path == zb) v = &x;
        CHECK(v != nullptr);
        auto inside = [&](const Pose& p) {  // 凸包内：所有边同号叉积
            int pos = 0, neg = 0;
            for (int i = 0; i < v->hullN; ++i) {
                const int a = v->hullAt + i, b = v->hullAt + (i + 1) % v->hullN;
                const double ax = tower::kVenueHull[2 * a], ay = tower::kVenueHull[2 * a + 1], bx = tower::kVenueHull[2 * b], by = tower::kVenueHull[2 * b + 1];
                const double cr = (bx - ax) * (p.y - ay) - (by - ay) * (p.x - ax);
                (cr >= 0 ? pos : neg)++;
            }
            return pos == 0 || neg == 0;
        };
        CHECK(inside(c.player) && inside(c.boss));
        Dest nd; CHECK(NativeDest(zb, "拙壁", &nd) && DistXY(nd.target, c.player) < 1 && nd.resolveSurfaceHeight);
        CHECK(!CenteredPair("/PBZCombatAsset/ActorData/Mobs/057_WanJun/BP_Monster_WanJun_JinGua.BP_Monster_WanJun_JinGua_C", &c));
        // 1.1.1 22 层游龙：落点在 Boss 出生点旁 6 米、出生点高度（不再是低 2 米多的河道），在凸包里
        const std::string_view yl = "/PBZCombatAsset/ActorData/Mobs/056_YouLong/BP_Monster_YouLong_Harpoon.BP_Monster_YouLong_Harpoon_C";
        Pose yp; CHECK(NearSpawner(yl, &yp));
        const tower::VenueDef* yv = nullptr;
        for (auto& x : tower::kVenues) if (x.path == yl) yv = &x;
        CHECK(yv && std::abs(DistXY(yp, {yv->sx, yv->sy, 0, 0}) - kSpawnerGap) < 1 && std::abs(yp.z - yv->sz) < 1);
        Dest yd; CHECK(NativeDest(yl, "游龙", &yd) && DistXY(yd.target, yp) < 1 && yd.target.z > 700 && yd.resolveSurfaceHeight);
        CHECK(!NearSpawner(zb, &yp));
        std::printf("zhuobi player %.0f/%.0f yaw %.1f boss %.0f/%.0f\n", c.player.x, c.player.y, c.player.yaw, c.boss.x, c.boss.y);
    }
    std::printf("TOWER_TRAVEL_OK %d checks (no game process)\n", g_checks);
    return 0;
}
