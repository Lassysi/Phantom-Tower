// QA 场地容量探测（用户 2026-11-12：无尽模式要在大场地同时刷多 Boss + 小怪，先批量跑图实测各场地装不装得下）。
// 只在 PBZ_HEX_TEST 编译；作为 Tower 类成员 #include 进 tower.cpp。塔档内、准备页（phase=ready）才接受。
// 流程：传送到场地（与正式层同一 GoFloor / 场地扫描 / 原生刷怪点静默）→ 可选把玩家往场地中心挪 → 逐只请求生成
// （Boss 按正式层的玩家周围 8 米搜索，或 bossmode=region 按场地中心区域；小怪按波次的 tower-region 区域）→
// 全部回执后读每只位置、碰撞包围盒、脚下地面，判是否在战斗区内、软边内、两两重叠 → 回报 JSON → 清掉本次生成物。
// 不进战斗阶段、不结算、不写局存档；生成物前缀 tower-qacap-，失败或超时同样清理。
struct CapActor { std::string kind, path, name; UObject* obj = nullptr; };
struct CapProbe {
    int stage = 0, seq = 0, next = 0;
    std::string label, venue, identity, name, stand, bossMode, why;
    std::vector<std::string> bosses, mobs;
    std::vector<CapActor> spawned;
    std::vector<std::string> failures;
    uint64_t since = 0, stageAt = 0;
    tower::arena::Boundary area;
    double inner = 0;
    bool venueFound = false, waiting = false;
    std::unordered_set<UObject*> before;
};
std::optional<CapProbe> cap_;
int capSeq_ = 0;

static std::vector<std::string> CapSplit(const std::string& s) {
    std::vector<std::string> v; size_t a = 0;
    while (a < s.size()) { size_t b = s.find(',', a); if (b == std::string::npos) b = s.size(); if (b > a) v.push_back(s.substr(a, b - a)); a = b + 1; }
    return v;
}
static bool CapLoc(UObject* a, tower::arena::P3* out) {
    auto* f = a && ue::Live(a) ? pe::FindFunction(ue::ClassOf(a), "K2_GetActorLocation") : nullptr;
    const int ret = f ? ue::PropOffset(f, "ReturnValue") : -1;
    if (ret < 0) return false;
    uint8_t buf[64] = {}; pe::Call(a, f, buf);
    double v[3]; std::memcpy(v, buf + ret, 24);
    if (!std::isfinite(v[0]) || !std::isfinite(v[1]) || !std::isfinite(v[2])) return false;
    *out = {v[0], v[1], v[2]}; return true;
}
static double CapFootprint(UObject* a) {  // 胶囊半径（厘米）：CapsuleComponent.GetScaledCapsuleRadius；读不到返回 0
    // 首版用 GetActorBounds：包含武器 / 特效组件，半径读成 2–6 米，重叠全是误报（实机榕树第一轮）
    const int off = a && ue::Live(a) ? ue::PropOffset(ue::ClassOf(a), "CapsuleComponent") : -1;
    auto* cap = off >= 0 ? ue::Get<UObject*>(a + off) : nullptr;
    auto* f = cap && ue::Live(cap) ? pe::FindFunction(ue::ClassOf(cap), "GetScaledCapsuleRadius") : nullptr;
    const int ret = f ? ue::PropOffset(f, "ReturnValue") : -1;
    if (ret < 0 || ret > 28) return 0;
    uint8_t buf[32] = {}; pe::Call(cap, f, buf);
    float r = 0; std::memcpy(&r, buf + ret, 4);
    return std::isfinite(r) && r > 0 ? r : 0;
}
static bool CapPlayer(double out[3]) {  // PawnLoc 依赖战斗时才解析的 fnLoc_，准备页读不到（实机第一轮 player=[0,0,0]）
    double at[4]; std::string world;
    if (!pbz::PlayerWhere(at, &world)) return false;
    out[0] = at[0]; out[1] = at[1]; out[2] = at[2]; return true;
}

void CapFail(const std::string& why) { if (cap_) { cap_->why = why; cap_->stage = 9; } }
void CapStage(int s) { cap_->stage = s; cap_->stageAt = GetTickCount64(); }

void BeginCap(const std::string& line) {
    auto reply = [&](const std::string& err) {
        pbz::ipc::Send(std::format(R"({{"op":"tower","event":"qa_cap","ok":false,"label":"{}","error":"{}"}})", J(pbz::ipc::Field(line, "label").value_or("")), J(err)));
    };
    if (cap_) { reply("capture_in_progress"); return; }
    if (trip_.phase() != tower::trip::Phase::Inside || run_.phase != tower::Phase::Ready) { reply("only_tower_ready_page"); return; }
    if (towertravel::Busy() || pbz::TeleportBusy() || !towerentry::LoadingClear()) { reply("busy"); return; }
    CapProbe c;
    c.label = pbz::ipc::Field(line, "label").value_or("");
    c.venue = pbz::ipc::Field(line, "venue").value_or("");
    c.stand = pbz::ipc::Field(line, "stand").value_or("landing");
    c.bossMode = pbz::ipc::Field(line, "bossmode").value_or("player");
    c.bosses = CapSplit(pbz::ipc::Field(line, "bosses").value_or(""));
    c.mobs = CapSplit(pbz::ipc::Field(line, "mobs").value_or(""));
    if (c.bosses.size() + c.mobs.size() == 0 || c.bosses.size() > 3 || c.mobs.size() > 12) { reply("bad_counts"); return; }
    if (c.stand != "landing" && c.stand != "inward" && c.stand != "center") { reply("bad_stand"); return; }
    if (towerfixed::Is(c.venue)) { c.identity = "fixed"; c.name = c.venue; }
    else {
        const int v = FindBoss(c.venue);
        if (v < 0 || !towertravel::HasVenue(tower::kBossPool[v].path, tower::kBossPool[v].identity)) { reply("unknown_venue"); return; }
        c.venue = std::string(tower::kBossPool[v].path); c.identity = std::string(tower::kBossPool[v].identity); c.name = std::string(tower::kBossPool[v].name);
    }
    for (auto* list : {&c.bosses, &c.mobs}) for (auto& p : *list) {
        const int b = list == &c.bosses ? FindBoss(p) : -1;
        if (list == &c.bosses) { if (b < 0) { reply("unknown_boss:" + p); return; } p = std::string(tower::kBossPool[b].path); }
        else if (std::none_of(std::begin(tower::mobs::kMobs), std::end(tower::mobs::kMobs), [&](const auto& m) { return m.path == p; })) { reply("unknown_mob:" + p); return; }
    }
    c.seq = ++capSeq_;
    c.since = GetTickCount64();
    towerisolate::Begin(c.identity);
    const int went = towertravel::GoFloor(run_.Zone(), run_.seed, c.venue, c.identity, c.name, false);
    if (went < 0) { towerisolate::End(); reply("travel_refused"); return; }
    cap_ = std::move(c);
    CapStage(1);
    pbz::log::Info("影塔 QA 容量：{} 场地 {}，Boss {} 只（{}）、小怪 {} 只，站位 {}", cap_->label, cap_->name, cap_->bosses.size(), cap_->bossMode, cap_->mobs.size(), cap_->stand);
}

void CapSubmitNext() {
    auto& c = *cap_;
    const int total = static_cast<int>(c.bosses.size() + c.mobs.size());
    int i = -1; bool boss = false, region = false;
    while (c.next < total) {
        i = c.next++;
        boss = i < static_cast<int>(c.bosses.size());
        region = !boss || c.bossMode == "region";
        if (!region || c.inner >= 300) break;
        c.failures.push_back(std::format("{} 跳过：场内安全区半径只有 {:.1f} 米", boss ? "boss" : "mob", c.inner / 100));
        i = -1;
    }
    if (i < 0) { CapStage(5); return; }
    const std::string path = boss ? c.bosses[i] : c.mobs[i - c.bosses.size()];
    pbz::enemies::Request r;
    r.command = "spawn"; r.path = path; r.count = 1;
    r.requestId = std::format("tower-qacap-{}-{}{}-", c.seq, region ? "mob" : "b", i);
    if (region) {
        r.distanceCm = std::clamp(600.0, 200.0, std::max(200.0, c.inner - 300.0));
        r.faction = std::format("tower-region {:.3f} {:.3f} {:.3f}", c.area.center.x, c.area.center.y, c.inner);
    } else r.distanceCm = 800;
    c.before.clear();
    for (auto* a : pbz::enemies::OwnedActors()) c.before.insert(a);
    c.waiting = true;
    const int seq = c.seq;
    const std::string kind = boss ? "boss" : "mob", request = r.requestId;
    pbz::enemies::Submit(std::move(r), [this, seq, kind, path, request](std::string json) {
        if (!cap_ || cap_->seq != seq || cap_->stage != 4) { pbz::enemies::ClearTagged(request); return; }
        cap_->waiting = false;
        if (json.find("\"ok\":true") == std::string::npos) {
            const auto m = json.find("\"message\":\"");
            std::string msg = m == std::string::npos ? json.substr(0, 160) : json.substr(m + 11, 200);
            if (const auto q = msg.find('"'); q != std::string::npos) msg.resize(q);
            cap_->failures.push_back(std::format("{} {}：{}", kind, path.substr(path.rfind('.') + 1), msg));
            pbz::enemies::ClearTagged(request);
            return;
        }
        for (auto* a : pbz::enemies::OwnedActors())
            if (!cap_->before.count(a) && ue::Live(a)) { cap_->spawned.push_back({kind, path, ue::ObjName(a), a}); break; }
    });
}

void CapReport() {
    auto& c = *cap_;
    double pl[3] = {}; const bool havePlayer = CapPlayer(pl);
    const tower::arena::P3 player{pl[0], pl[1], pl[2]};
    std::string items;
    int inside = 0, soft = 0, outside = 0, grounded = 0, overlaps = 0;
    struct M { tower::arena::P3 p; double r; bool ok; };
    std::vector<M> ms;
    for (auto& a : c.spawned) {
        M m{{}, 0, CapLoc(a.obj, &m.p)};
        m.r = CapFootprint(a.obj);
        const double at[3] = {m.p.x, m.p.y, m.p.z};
        const bool in = m.ok && c.area.Contains(m.p, 0), inSoft = m.ok && !c.area.OutsideBy(m.p, c.area.slack);
        const bool stand = m.ok && towerreach::StandingAt(a.obj, at);
        inside += in; soft += !in && inSoft; outside += !inSoft; grounded += stand;
        ms.push_back(m);
        items += std::format(R"({}{{"kind":"{}","class":"{}","pos":[{:.0f},{:.0f},{:.0f}],"r":{:.0f},"center_m":{:.1f},"player_m":{:.1f},"inside":{},"soft":{},"standing":{}}})",
            items.empty() ? "" : ",", a.kind, J(a.path.substr(a.path.rfind('.') + 1)), m.p.x, m.p.y, m.p.z, m.r,
            std::hypot(m.p.x - c.area.center.x, m.p.y - c.area.center.y) / 100, havePlayer ? std::hypot(m.p.x - pl[0], m.p.y - pl[1]) / 100 : -1.0,
            in, !in && inSoft, stand);
    }
    for (size_t i = 0; i < ms.size(); ++i) for (size_t j = i + 1; j < ms.size(); ++j)
        if (ms[i].ok && ms[j].ok && std::abs(ms[i].p.z - ms[j].p.z) < 300 && std::hypot(ms[i].p.x - ms[j].p.x, ms[i].p.y - ms[j].p.y) < std::max(60.0, ms[i].r + ms[j].r)) ++overlaps;
    std::string fails;
    for (auto& f : c.failures) fails += (fails.empty() ? "\"" : ",\"") + J(f) + "\"";
    const int wanted = static_cast<int>(c.bosses.size() + c.mobs.size());
    const bool pass = c.why.empty() && c.venueFound && static_cast<int>(c.spawned.size()) == wanted && outside == 0 && overlaps == 0 &&
                      grounded == static_cast<int>(c.spawned.size()) && havePlayer && c.area.Contains(player, 0);
    const auto event = std::format(
        R"({{"op":"tower","event":"qa_cap","ok":{},"pass":{},"label":"{}","venue":"{}","name":"{}","why":"{}","venue_found":{},"boundary":"{}","center":[{:.0f},{:.0f}],"inner_m":{:.1f},"stand":"{}","bossmode":"{}","player":[{:.0f},{:.0f},{:.0f}],"player_inside":{},"player_center_m":{:.1f},"wanted":{},"spawned":{},"inside":{},"soft":{},"outside":{},"standing":{},"overlaps":{},"seconds":{:.1f},"failures":[{}],"actors":[{}]}})",
        c.why.empty(), pass, J(c.label), J(c.venue.substr(c.venue.rfind('.') + 1)), J(c.name), J(c.why), c.venueFound, J(c.area.Describe()), c.area.center.x, c.area.center.y,
        c.inner / 100, c.stand, c.bossMode, pl[0], pl[1], pl[2], havePlayer && c.area.Contains(player, 0),
        havePlayer ? std::hypot(pl[0] - c.area.center.x, pl[1] - c.area.center.y) / 100 : -1.0, wanted, c.spawned.size(), inside, soft, outside, grounded, overlaps,
        (GetTickCount64() - c.since) / 1000.0, fails, items);
    pbz::ipc::Send(event);
    pbz::log::Info("影塔 QA 容量：{} {} {}：生成 {}/{}，界内 {}、软边 {}、界外 {}，站稳 {}，重叠 {}{}", c.label, c.name, pass ? "通过" : "未通过", c.spawned.size(), wanted,
                   inside, soft, outside, grounded, overlaps, c.why.empty() ? "" : "（" + c.why + "）");
}

void CapEnd() {
    const size_t left = pbz::enemies::ClearTagged("tower-qacap-");
    if (left) pbz::log::Info("影塔 QA 容量：仍有 {} 个生成物未清掉", left);
    tvenue::End();
    towerisolate::End();
    cap_.reset();
}

void CapTick() {
    if (!cap_) return;
    auto& c = *cap_;
    const uint64_t now = GetTickCount64(), inStage = now - c.stageAt;
    if (trip_.phase() != tower::trip::Phase::Inside || run_.phase != tower::Phase::Ready) { c.why = "left_ready_page"; CapReport(); CapEnd(); return; }
    if (now - c.since > 240000 && c.stage < 9) CapFail("timeout_" + std::to_string(c.stage));
    switch (c.stage) {
    case 1:  // 传送
        if (towertravel::Busy()) return;
        if (towertravel::Failed()) { CapFail("travel_failed:" + towertravel::Why()); return; }
        if (!towerentry::LoadingClear() || inStage < 1500) return;
        tvenue::Arrive(c.venue);
        CapStage(2);
        return;
    case 2: {  // 场地扫描 → 战斗区（与 ArmBoundary 同一规则）
        tvenue::Tick();
        if (!tvenue::Ready()) return;
        std::vector<tower::arena::P3> hull; tower::arena::P3 ce; double r = 0, lo = 0, hi = 0;
        c.venueFound = tvenue::Boundary(&hull, &ce, &r, &lo, &hi);
        double pl[3];
        if (c.venueFound) { if (!hull.empty()) c.area.ArmHull(hull, ce, lo, hi); else { c.area.Arm(ce, r); c.area.zmin = lo; c.area.zmax = hi; } }
        else if (CapPlayer(pl)) c.area.Arm({pl[0], pl[1], pl[2]}, 0);
        else { CapFail("no_player"); return; }
        c.area.slack = 1000;
        if (!hull.empty()) {  // 波次同款：凸包顶点平均作区域中心
            c.area.center.x = 0; c.area.center.y = 0;
            for (const auto& p : hull) { c.area.center.x += p.x / hull.size(); c.area.center.y += p.y / hull.size(); }
        }
        c.inner = c.area.InnerRadius(250, 1400);
        if (c.stand != "landing" && CapPlayer(pl)) {
            const double k = c.stand == "center" ? 1.0 : 0.6;
            const double tx = pl[0] + (c.area.center.x - pl[0]) * k, ty = pl[1] + (c.area.center.y - pl[1]) * k;
            const double yaw = std::atan2(c.area.center.y - ty, c.area.center.x - tx) * 180.0 / 3.14159265358979323846;
            std::string msg;
            if (std::hypot(tx - pl[0], ty - pl[1]) > 150 && !pbz::PointTeleport(tx, ty, pl[2], yaw, &msg)) c.failures.push_back("stand:" + msg);
        }
        CapStage(3);
        return;
    }
    case 3:  // 站位稳定
        if (pbz::TeleportBusy() || inStage < 1500) return;
        CapStage(4);
        return;
    case 4:  // 逐只生成
        if (c.waiting) { if (inStage > 30000) CapFail("spawn_reply_timeout"); return; }
        CapSubmitNext();
        return;
    case 5:  // 落地稳定后测量
        if (inStage < 700) return;
        CapReport(); CapEnd();
        return;
    case 9:
        CapReport(); CapEnd();
        return;
    }
}
