// 胜负契约（1–32 层：21 层投技后 HP1 被判负而 god 开着；20 层血量归零就派奖，没有死亡确认）。纯逻辑，离线测试。
//
// 玩家：塔把生命下限钉在 1（不走原生死亡），生命 <= 1.5 视为"致死"。
//   - 修改器测试辅助（player.god / player.demigod）开着：不判负，回满血、记一次"辅助接住"，本层无伤挑战作废；
//     投技 / 处决 / 脚本伤害绕过了 Core 的受伤倍率时就走这里——测试辅助的含义是"不死"，不是"死了算失败"；
//   - 辅助关着：先用复活符，没有就判负（合法失败照常）。
// Boss：生命 <= 0.5 只是"归零"；连续 kConfirmSec 秒仍归零才算死亡确认。期间回血 = 阶段续接（同一个类自带的阶段）。
//   最后一只确认后立即派奖；战后原生演出由真实执行门在执行前冻结。
#pragma once
#include <cmath>

namespace tower::fate {

enum class Verdict { Fine, Assisted, Revive, Lost };
inline Verdict JudgePlayer(float hp, double sinceFight, bool assist, bool reviveLeft) {
    if(!std::isfinite(hp) || !std::isfinite(sinceFight))return Verdict::Fine;
    if (hp > 1.5f || sinceFight <= 1) return Verdict::Fine;
    if (assist) return Verdict::Assisted;
    return reviveLeft ? Verdict::Revive : Verdict::Lost;
}

inline constexpr double kConfirmSec = 1.0, kSettleSec = 0.0; // final confirmation -> immediate Tower reward; never wait for native ending

struct DeathTrack {
    enum class Ev { None, Zero, Resumed, Confirmed };
    double zeroAt = -1;
    bool confirmed = false;
    int resumes = 0;
    Ev Sample(double now, float hp) {
        if(!std::isfinite(hp) || !std::isfinite(now)){zeroAt=-1;confirmed=false;return Ev::None;} // invalid reads cannot extend a death confirmation
        if (hp > 0.5f) {
            if (zeroAt < 0) return Ev::None;
            zeroAt = -1; confirmed = false; ++resumes;
            return Ev::Resumed;
        }
        if (zeroAt < 0) { zeroAt = now; return Ev::Zero; }
        if (!confirmed && now - zeroAt >= kConfirmSec) { confirmed = true; return Ev::Confirmed; }
        return Ev::None;
    }
    bool Dead() const { return confirmed; }
};

}  // namespace tower::fate
