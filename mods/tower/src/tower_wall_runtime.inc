// 3.6.0 统一出界拉回 + 隐形硬墙（用户 2026-11-14，方案第 7 / 7b 条）：Tower 类成员，#include 进 tower.cpp（ArmBoundary 之后）。
// 规则见 tower_wall.h。全部在游戏线程（Leash 每帧调用）；只读写当前帧仍 Live 的对象，召唤物列表每 0.5 秒由 Sweep 重建。
// 原生雾门 BP_FogDoor / BP_FogDoorTotalBlock 是编辑器摆放的样条网格 + 存档组件（PBZActorSaveComponent），运行时生成要重建样条、
// 还可能写进存档，不可靠 → 不生成原生墙，用每帧位置夹回当「隐形硬墙」。
tower::wall::Circle wall_;
bool wallMeasured_ = false, wallChecked_ = false;
std::unordered_map<UObject*, tower::wall::Track> wallTrack_;
std::vector<UObject*> wallSummons_, wallSummonsNext_;  // 本层 Boss 的同身份召唤物（雷婷分身等）
int wallLogs_ = 0;
int shachiRegenLogs_ = 0;  // 3.6.0 无尽回复：杀气回复读回诊断（前 20 次）
int cmcOff_ = -2, moveModeOff_ = -1, velocityOff_ = -1;

void ArmWall() {
    wall_ = {}; wallMeasured_ = false; wallChecked_ = false; wallTrack_.clear(); wallLogs_ = 0; wallSummons_.clear(); wallSummonsNext_.clear();
    if (!boundary_.on) return;
    const std::string key = !venuePath_.empty() ? venuePath_ : venueBoss_ >= 0 ? std::string(tower::kBossPool[venueBoss_].path) : std::string();
    if (const auto* real = tower::venuereal::Find(key)) {
        wall_.x = real->cx; wall_.y = real->cy; wall_.ok = true; wallMeasured_ = true;
        for (int i = 0; i < real->n; ++i) wall_.rays.push_back(real->rays[static_cast<size_t>(i)] * 100.0);
        wall_.r = *std::max_element(wall_.rays.begin(), wall_.rays.end());
    } else WallFromBoundary();  // 没实测：战斗区外接圆 + 软边（只给硬墙用；拉回照旧走战斗区规则）
    pbz::log::Info("影塔边界：{}，圆心 {:.0f}/{:.0f}，判定线 {:.1f}–{:.1f} 米（墙内 1.5 米）", wallMeasured_ ? "实测真实可站边界" : "未实测，按战斗区外接圆 + 软边",
                   wall_.x, wall_.y, wall_.Line() / 100, (wall_.r - tower::wall::kInset) / 100);
}
void WallFromBoundary() {
    wall_ = {}; wall_.x = boundary_.center.x; wall_.y = boundary_.center.y; wall_.ok = true;
    double r = boundary_.radius;
    if (!boundary_.hull.empty()) { r = 0; for (const auto& q : boundary_.hull) r = std::max(r, std::hypot(q.x - wall_.x, q.y - wall_.y)); }
    wall_.r = r + boundary_.slack;
}
// 3.6.0 打包前（雷婷实机：实测圆心与这一场实际站位差 32 米，硬墙把她按错位的线反复夹、半截入土）：
// 开战后第一次检查——玩家本人站在实测边界外 = 这份实测和这场的实际场地对不上，本层改回按实际站位的战斗区，不用实测
void CheckWallMatch(UObject* pawn) {
    if (!wallMeasured_ || wallChecked_ || fightSince_ <= 0 || !pawn) return;
    double pl[3];
    if (!PawnLoc(pl)) return;
    wallChecked_ = true;
    const tower::wall::P me{pl[0], pl[1], pl[2]};
    if (!tower::wall::Outside(wall_, me)) return;
    const double off = tower::wall::Dist2D(me, wall_.x, wall_.y) / 100, line = wall_.LineAt(me.x, me.y) / 100;
    WallFromBoundary(); wallMeasured_ = false; wallTrack_.clear();
    pbz::log::Info("影塔边界：玩家站在实测边界外（离实测圆心 {:.1f} 米，判定线 {:.1f} 米），实测与本场位置不符，本层改用战斗区（圆心 {:.0f}/{:.0f}，{:.1f} 米）",
                   off, line, wall_.x, wall_.y, wall_.r / 100);
}
// 硬墙对象：无尽层所有敌人；前 100 层只管飞行中的敌人（CharacterMovement.MovementMode == MOVE_Flying）
UObject* MoveComp(UObject* obj) {
    if (cmcOff_ == -2) {
        auto* ch = towerperf::ClassNamed("Character");
        auto* cmc = towerperf::ClassNamed("CharacterMovementComponent");
        auto* mc = towerperf::ClassNamed("MovementComponent");
        cmcOff_ = ch ? ue::PropOffset(ch, "CharacterMovement") : -1;
        moveModeOff_ = cmc ? ue::PropOffset(cmc, "MovementMode") : -1;
        velocityOff_ = mc ? ue::PropOffset(mc, "Velocity") : -1;
    }
    if (cmcOff_ < 0 || !obj || !ue::Live(obj)) return nullptr;
    UObject* c = ue::Get<UObject*>(obj + cmcOff_);
    return c && ue::Live(c) ? c : nullptr;
}
bool HardWallFor(UObject* obj) {
    if (run_.endless && run_.floor > 50) return true;
    // 3.6.0 打包前（用户 2026-11-15：前 100 层也不能刷出 / 闪出 / 飞出雾门）：有实测边界的场地，开战 3 秒后（入场动作走完）所有敌人都上硬墙
    if (wallMeasured_ && fightSince_ > 0 && now_ - fightSince_ >= 3.0) return true;
    UObject* c = MoveComp(obj);
    return c && moveModeOff_ >= 0 && ue::Get<uint8_t>(c + moveModeOff_) == 5;  // EMovementMode::MOVE_Flying
}
bool SetLocFast(UObject* a, const tower::wall::P& p) {  // 瞬移（不扫掠、不清速度），读回 < 3 米算成功
    if (!a || !ue::Live(a) || !fnLoc_) return false;
    auto* fn = pe::FindFunction(ue::ClassOf(a), "K2_SetActorLocation");
    if (!fn) return false;
    const int loc = ue::PropOffset(fn, "NewLocation"), tp = ue::PropOffset(fn, "bTeleport"), sw = ue::PropOffset(fn, "bSweep");
    const int size = ue::Get<int>(fn + ue::Layout::StructSize);
    if (loc < 0 || tp < 0 || sw < 0 || size <= 0 || size > 0x200 || loc + 24 > size || tp >= size || sw >= size) return false;
    if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z)) return false;
    std::array<uint8_t, 0x200> args{};
    const double v[3] = {p.x, p.y, p.z};
    std::memcpy(args.data() + loc, v, 24);
    args[tp] = 1; args[sw] = 0;
    pe::Call(a, fn, args.data());
    uint8_t b[32] = {}; pe::Call(a, fnLoc_, b); double q[3]; std::memcpy(q, b, 24);
    return std::hypot(q[0] - p.x, q[1] - p.y) < 300 && std::abs(q[2] - p.z) < 300;
}
// 硬墙：越过判定线（或飞得比战斗区顶还高）就夹回，切掉向外 / 向上的速度；返回这一帧是否拦过
bool HardClamp(UObject* obj, const std::string& name, const tower::wall::P& p, tower::wall::Track& tr) {
    // 3.6.0 打包前（雷婷半截入土）：硬墙只顶回一小段（越线 ≤ 1.5 米，正常走 / 冲到墙边的那一下）；
    // 越线更多（闪现、刚开战位置不对）交给带地面检测的拉回，不再直接平移
    if (tower::wall::Dist2D(p, wall_.x, wall_.y) - wall_.LineAt(p.x, p.y) > 150) return false;
    tower::wall::P q = p;
    bool moved = tower::wall::ClampXY(wall_, q);
    if (boundary_.zmax > boundary_.zmin && q.z > boundary_.zmax - 100) { q.z = boundary_.zmax - 100; moved = true; }
    if (!moved) return false;
    UObject* mc = MoveComp(obj);
    const bool flying = mc && moveModeOff_ >= 0 && ue::Get<uint8_t>(mc + moveModeOff_) == 5;
    if (!flying) {  // 地面上的敌人：新位置按地面高差抬 / 降，保持脚底贴地（胶囊中心离地高度不变）
        double gp[3], gq[3];
        if (!towerreach::GroundBelow(obj, p.x, p.y, p.z + 100, p.z - 600, gp) || !towerreach::GroundBelow(obj, q.x, q.y, p.z + 300, p.z - 600, gq)) return false;
        // 3.6.2（玩家直播：花千笑断桥上被夹进桥板下面）：新位置的地面和原来不在同一层（桥面 / 桥下、台阶上下，高差 > 0.6 米）就不夹，
        // 交给带落地检测的拉回；同一层才贴地平移
        if (std::abs(gq[2] - gp[2]) > 60) return false;
        q.z = gq[2] + (p.z - gp[2]);
    }
    const bool ok = SetLocFast(obj, q);
    if (UObject* c = MoveComp(obj); c && velocityOff_ >= 0) {
        double v[3]; std::memcpy(v, reinterpret_cast<uint8_t*>(c) + velocityOff_, 24);
        if (std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2])) {
            bool cut = tower::wall::CutOutward(wall_, q, v[0], v[1]);
            if (q.z < p.z && v[2] > 0) { v[2] = 0; cut = true; }
            if (cut) ue::Write(reinterpret_cast<uint8_t*>(c) + velocityOff_, v, 24);
        }
    }
    ++tr.blocked;
    if (wallLogs_ < 60 && (tr.blocked == 1 || tr.blocked % 50 == 0)) {
        ++wallLogs_;
        pbz::log::Info("影塔硬墙：{} 在 {:.0f}/{:.0f}/{:.0f} 被边界拦住（第 {} 次，{}）", name, p.x, p.y, p.z, tr.blocked, ok ? "已夹回" : "夹回未确认");
    }
    return true;
}
void PullInside(UObject* obj, const std::string& name, const tower::wall::P& p, tower::wall::Track& tr) {
    double pl[3] = {wall_.x, wall_.y, p.z};
    PawnLoc(pl);
    const bool hard = tr.Hard(now_);
    const auto cands = tower::wall::PullCandidates(wall_, p, {pl[0], pl[1], pl[2]}, hard, run_.seed ^ (static_cast<uint64_t>(tr.pulled + 1) * 0x9E3779B97F4A7C15ull));
    bool ok = false; std::string why;
    for (const auto& q : cands) {
        const double nearAt[3] = {q.x, q.y, (boundary_.on ? boundary_.center.z : q.z) + 50}; double landing[3]; std::string w;
        if (!towerreach::Landing(obj, nearAt, landing, &w, true)) { why += (why.empty() ? "" : "；") + w; continue; }
        const tower::arena::P3 dst{landing[0], landing[1], landing[2]};
        if (tower::wall::Outside(wall_, {dst.x, dst.y, dst.z}) || !MoveActor(obj, dst)) { why += (why.empty() ? "" : "；") + std::string("放下后仍在线外"); continue; }
        ok = true; break;
    }
    tr.Pulled(now_);
    boundary_.Pulled(now_, ok);
    if (wallLogs_ < 120) {
        ++wallLogs_;
        pbz::log::Info("影塔出界拉回：{} 在 {:.0f}/{:.0f}/{:.0f}（离中心 {:.1f} 米，判定线 {:.1f} 米）第 {} 次{}：{}{}", name, p.x, p.y, p.z,
                       tower::wall::Dist2D(p, wall_.x, wall_.y) / 100, wall_.LineAt(p.x, p.y) / 100, tr.pulled, hard ? "" : "",
                       ok ? "已拉回" : "拉回失败", ok ? "" : "（" + why + "）");
    }
}
// 一个敌人（Boss / 小怪 / 召唤物）每帧：硬墙 → 实测边界的统一拉回。返回 true = 已由新规则处理（不再走旧的战斗区规则）
bool WallStep(UObject* obj, const std::string& name, const tower::arena::P3& bp, UObject* pawn) {
    if (!wall_.ok || !obj) return false;
    CheckWallMatch(pawn);
    auto& tr = wallTrack_[obj];
    const tower::wall::P p{bp.x, bp.y, bp.z};
    if (HardWallFor(obj) && HardClamp(obj, name, p, tr)) return true;
    if (!wallMeasured_) return false;
    (void)pawn;  // 3.6.0 打包前（用户）：不再按「路被墙隔开」拉回，只认明确出了雾门
    if (tr.Due(wall_, p, now_)) PullInside(obj, name, p, tr);
    return true;
}
