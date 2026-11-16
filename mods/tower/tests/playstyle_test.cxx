#include <cmath>
#include <cstdio>
#include <cstdlib>
#include "../src/tower_playstyle.h"
static void Check(bool ok, const char* why) { if (!ok) { std::printf("FAIL %s\n", why); std::exit(1); } }
int main() {
    using namespace tower::playstyle;
    // 数值表（用户 2026-11-15 定稿，全部最高 Lv2）：两级单调不降，倍率与比例在合理范围
    Check(kMaxLv == 2, "all playstyle hexes cap at Lv2");
    Check(kRiposteHitMul[0] <= kRiposteHitMul[1] && kAfterimageSec[0] <= kAfterimageSec[1] &&
          kFrenzyHeal[0] <= kFrenzyHeal[1] && kChainRadius[0] <= kChainRadius[1] &&
          kChainShachiPct[0] <= kChainShachiPct[1] && kLastStandSec[0] <= kLastStandSec[1] && kShadowCloneSec[0] <= kShadowCloneSec[1],
          "every playstyle value is non-decreasing Lv1 -> Lv2");
    Check(kChainShachiPct[1] < 1.0f && kLastStandTimeScale > 0 && kLastStandTimeScale < 1,
          "ratios stay in range (no full refund above cap, time scale never 0 = never a freeze)");
    Check(kRiposteHitMul[1] == 2.0f && kLastStandSec[1] == 10.0f && kShadowCloneSec[1] == 10.0f && kShadowCloneCooldownSec == 8.0f,
          "user values synced from notes/tower-3.7.0-playstyle-hexes.md");

    // 7 破防连锁
    Check(BrokeNow(30, 0) && !BrokeNow(0, 0) && !BrokeNow(30, 10) && !BrokeNow(-1, 0), "break = shachi drops from >0 to 0");
    Check(std::abs(ChainShachiAfter(1, 500, 1000) - 300) < 1e-3 && ChainShachiAfter(2, 100, 1000) == 1.0f && ChainShachiAfter(2, 1, 1000) == 1.0f,
          "neighbours lose a share of max shachi but never reach 0 (writing 0 bypasses native break)");
    Check(InChainRadius(1, 600, 500, 0) && !InChainRadius(1, 600, 600, 0) && InChainRadius(2, 1100, 0, 300) && !InChainRadius(0, 0, 0, 0), "radius per level");

    // 8 绝境时缓：每层一次
    LastStand ls;
    Check(ls.Tick(1, 5, 0, 0.5f) == 1.0f, "above threshold: normal time");
    Check(ls.Tick(1, 5, 1, 0.29f) == kLastStandTimeScale && ls.Tick(1, 5, 5.9, 0.5f) == kLastStandTimeScale && ls.Tick(1, 5, 6.0, 0.5f) == 1.0f, "Lv1 lasts 5 s");
    Check(ls.Tick(1, 5, 10, 0.1f) == 1.0f, "only once per floor");
    Check(ls.Tick(2, 6, 20, 0.2f) == kLastStandTimeScale && ls.Tick(2, 6, 29.9, 0.2f) == kLastStandTimeScale && ls.Tick(2, 6, 30.1, 0.2f) == 1.0f, "new floor re-arms; Lv2 lasts 10 s");
    Check(ls.Tick(2, 7, 40, -1) == 1.0f, "unreadable HP never triggers");

    // 1 以牙还牙
    Riposte r;
    Check(r.ConsumeOnHit(2, 0) == 1.0f, "no perfect deflect: normal hit");
    r.OnPerfectDeflect(10);
    Check(r.ConsumeOnHit(2, 11) == 2.0f && r.ConsumeOnHit(2, 11.1) == 1.0f, "one boosted hit per perfect deflect");
    r.OnPerfectDeflect(20);
    Check(r.ConsumeOnHit(2, 23.5) == 1.0f, "window expires");

    // 2 残影步 / 12 化影随行
    Afterimage a; a.OnPerfectDodge(2, 0);
    Check(a.BreakAdd(2, 4.9) > 0 && a.BreakAdd(2, 5.1) == 0, "Lv2 lasts 5 s");
    CloneGate g;
    Check(g.TrySpawn(1, 0) && !g.TrySpawn(1, 7.9) && g.TrySpawn(1, 8.0) && !g.TrySpawn(0, 100), "clone cooldown and missing hex");
    std::puts("TOWER_PLAYSTYLE_OK");
}
