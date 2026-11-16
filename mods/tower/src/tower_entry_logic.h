#pragma once
#include <cstdint>
namespace tower::entry {
enum class State { Idle, Waiting, Countdown, Running, Failed };
// Wall time, independent of game pause / dilation. Readiness must hold continuously.
// 6ede7eb 实测：跨图后日志读完 3/2/1，用户却还在黑屏——就绪（含画面可见）必须先连续保持 kSettleMs 才开始读秒，
// 遮罩淡出动画、相机淡入期间的一两帧"已清"不算
inline constexpr uint64_t kSettleMs = 800;
inline constexpr uint64_t kQuickSettleMs = 250;  // 波次层（用户 2026-11-08：不读秒）：只留开层补满读回需要的几帧
// A confirmed ended wave member must not keep the surviving wave frozen.
// Boss phases retain the stricter original readiness requirement.
inline bool ActorReady(bool wave, bool ended, bool filled, bool zero) {
    return (wave && ended) || (filled && !zero);
}
struct Gate {
    State state = State::Idle;
    uint64_t begun = 0, readyAt = 0, readySince = 0;
    bool quick = false;
    void Begin(uint64_t now, bool noCountdown = false) { state = State::Waiting; begun = now; readyAt = 0; readySince = 0; quick = noCountdown; }
    void Reset() { *this = {}; }
    void Restart(uint64_t now) { if (state == State::Countdown) readyAt = now; }  // 读秒期间做了耗时操作：从现在重新读满 3 秒
    bool Holding() const { return state == State::Waiting || state == State::Countdown; }
    int Step(uint64_t now, bool ready) {
        if (!Holding()) return 0;
        if (now - begun >= 90000) { state = State::Failed; return 0; }
        if (!ready) { state = State::Waiting; readyAt = 0; readySince = 0; return 0; }
        if (!readySince) readySince = now;
        if (state != State::Countdown && now - readySince < (quick ? kQuickSettleMs : kSettleMs)) return 0;
        if (quick) { state = State::Running; return 0; }
        if (state != State::Countdown) { state = State::Countdown; readyAt = now; }
        const auto elapsed = now - readyAt;
        if (elapsed >= 3000) { state = State::Running; return 0; }
        return 3 - static_cast<int>(elapsed / 1000);
    }
};
} // namespace tower::entry
