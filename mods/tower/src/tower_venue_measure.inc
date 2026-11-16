// 3.6.0 场地实测（用户 2026-11-14，方案第 3 条）：量「站得住的地面」离场地中心的真实距离，定无尽场地名单。
// 只在测试包（PBZ_PLAYTEST）/ QA（PBZ_HEX_TEST）编译；作为 Tower 类成员 #include 进 tower.cpp。塔档内、准备页才接受。
// {"cmd":"qa_measure","venue":"<Boss 类路径或 fixed:rongshu_zdy>","label":"...","dirs":32,"max":60}
// 流程：传送到场地（与正式层同一 GoFloor / 场地扫描）→ 玩家放到场地中心 → 每帧量一个方向：从中心每 1 米往外走，
//   ① 竖直向下找地面（落差 > 2.5 米或没有地面 = 边缘），② 胸口高度上一步到这一步的水平射线（被挡 = 墙 / 空气墙 / 雾墙）；
// → 回报 JSON（每个方向的半径与止步原因、最小 / 10% 分位 / 中位 / 最大、形状比、面积）。只读射线，不生成、不改场地、不写存档。
struct MeasureProbe {
    int stage = 0, seq = 0, dirs = 32, dir = 0;
    double maxCm = 6000;
    std::string label, venue, identity, name, why;
    uint64_t since = 0, stageAt = 0;
    tower::arena::Boundary area;
    bool venueFound = false;
    double c[3] = {}, foot = 0;
    std::vector<double> r;
    std::vector<std::string> stop;
};
std::optional<MeasureProbe> measure_;
int measureSeq_ = 0;

static bool MeasurePlayer(double out[3]) {
    double at[4]; std::string world;
    if (!pbz::PlayerWhere(at, &world)) return false;
    out[0] = at[0]; out[1] = at[1]; out[2] = at[2]; return true;
}
void MeasureStage(int s) { measure_->stage = s; measure_->stageAt = GetTickCount64(); }

void BeginMeasure(const std::string& line) {
    auto reply = [&](const std::string& err) {
        pbz::ipc::Send(std::format(R"({{"op":"tower","event":"qa_measure","ok":false,"label":"{}","error":"{}"}})", J(pbz::ipc::Field(line, "label").value_or("")), J(err)));
    };
    if (measure_) { reply("measure_in_progress"); return; }
    if (trip_.phase() != tower::trip::Phase::Inside || run_.phase != tower::Phase::Ready) { reply("only_tower_ready_page"); return; }
    if (towertravel::Busy() || pbz::TeleportBusy() || !towerentry::LoadingClear()) { reply("busy"); return; }
    MeasureProbe m;
    m.label = pbz::ipc::Field(line, "label").value_or("");
    m.venue = pbz::ipc::Field(line, "venue").value_or("");
    try { m.dirs = std::clamp(std::stoi(pbz::ipc::Field(line, "dirs").value_or("32")), 8, 72); } catch (...) {}
    try { m.maxCm = std::clamp(std::stod(pbz::ipc::Field(line, "max").value_or("60")), 10.0, 150.0) * 100.0; } catch (...) {}
    if (towerfixed::Is(m.venue)) { m.identity = "fixed"; m.name = m.venue; }
    else {
        const int v = FindBoss(m.venue);
        if (v < 0 || !towertravel::HasVenue(tower::kBossPool[v].path, tower::kBossPool[v].identity)) { reply("unknown_venue"); return; }
        m.venue = std::string(tower::kBossPool[v].path); m.identity = std::string(tower::kBossPool[v].identity); m.name = std::string(tower::kBossPool[v].name);
    }
    m.seq = ++measureSeq_;
    m.since = GetTickCount64();
    towerisolate::Begin(m.identity);
    const int went = towertravel::GoFloor(run_.Zone(), run_.seed, m.venue, m.identity, m.name, false);
    if (went < 0) { towerisolate::End(); reply("travel_refused"); return; }
    measure_ = std::move(m);
    MeasureStage(1);
    pbz::log::Info("影塔 QA 场地实测：{} 场地 {}，{} 个方向、最远 {:.0f} 米", measure_->label, measure_->name, measure_->dirs, measure_->maxCm / 100);
}

// 一个方向：从中心每 1 米往外，直到没有地面 / 落差过大 / 被挡 / 到最远
void MeasureDirection(UObject* ctx) {
    auto& m = *measure_;
    const double ang = 2.0 * 3.14159265358979323846 * m.dir / m.dirs, dx = std::cos(ang), dy = std::sin(ang);
    double prev[3] = {m.c[0], m.c[1], m.foot}, reach = 0;
    std::string why = "max";
    for (double d = 100; d <= m.maxCm + 1; d += 100) {
        const double x = m.c[0] + dx * d, y = m.c[1] + dy * d;
        double g[3];
        if (!towerreach::GroundBelow(ctx, x, y, prev[2] + 250, prev[2] - 600, g)) {
            why = "no_ground";
            if (m.dir < 2) pbz::log::Info("影塔 QA 场地实测：方向 {} 第 {:.0f} 米没有地面（脚下 z {:.0f}）：{}", m.dir, d / 100, prev[2], towerreach::GroundDiag(ctx, x, y, prev[2] + 250, prev[2] - 600));
            break;
        }
        if (std::abs(g[2] - prev[2]) > 250) { why = std::format("step_{:.0f}", g[2] - prev[2]); break; }
        const double a[3] = {prev[0], prev[1], prev[2] + 100}, b[3] = {x, y, g[2] + 100};
        double hit = 0; std::string comp;
        if (towerreach::Ray(ctx, a, b, &hit, &comp)) { reach += std::min(hit, 100.0); why = "wall:" + comp; break; }
        reach = d;
        prev[0] = x; prev[1] = y; prev[2] = g[2];
    }
    m.r.push_back(reach / 100.0);
    m.stop.push_back(why);
    ++m.dir;
}

void MeasureReport() {
    auto& m = *measure_;
    std::vector<double> s = m.r;
    std::sort(s.begin(), s.end());
    auto q = [&](double f) { return s.empty() ? 0.0 : s[std::min(s.size() - 1, static_cast<size_t>(f * (s.size() - 1) + 0.5))]; };
    double area = 0;
    for (double x : m.r) area += 0.5 * x * x * (2.0 * 3.14159265358979323846 / std::max(1, m.dirs));
    const double mn = q(0), p10 = q(0.1), med = q(0.5), p90 = q(0.9), mx = q(1);
    const double ratio = mx > 0 ? mn / mx : 0, ratioRobust = p90 > 0 ? p10 / p90 : 0;
    int walls = 0, edges = 0, atMax = 0;
    std::string rays;
    for (size_t i = 0; i < m.r.size(); ++i) {
        walls += m.stop[i].starts_with("wall"); edges += m.stop[i] == "no_ground" || m.stop[i].starts_with("step"); atMax += m.stop[i] == "max";
        rays += std::format(R"({}[{:.1f},"{}"])", rays.empty() ? "" : ",", m.r[i], J(m.stop[i]));
    }
    const auto event = std::format(
        R"({{"op":"tower","event":"qa_measure","ok":{},"label":"{}","venue":"{}","name":"{}","why":"{}","venue_found":{},"boundary":"{}","inner_m":{:.1f},"center":[{:.0f},{:.0f},{:.0f}],"dirs":{},"min_m":{:.1f},"p10_m":{:.1f},"median_m":{:.1f},"p90_m":{:.1f},"max_m":{:.1f},"ratio":{:.2f},"ratio_p10_p90":{:.2f},"area_m2":{:.0f},"equiv_r_m":{:.1f},"walls":{},"edges":{},"far":{},"seconds":{:.1f},"rays":[{}]}})",
        m.why.empty(), J(m.label), J(m.venue.substr(m.venue.rfind('.') + 1)), J(m.name), J(m.why), m.venueFound, J(m.area.Describe()), m.area.InnerRadius(250, 1400) / 100,
        m.c[0], m.c[1], m.foot, m.dirs, mn, p10, med, p90, mx, ratio, ratioRobust, area, std::sqrt(area / 3.14159265358979323846), walls, edges, atMax,
        (GetTickCount64() - m.since) / 1000.0, rays);
    pbz::ipc::Send(event);
    pbz::log::Info("影塔 QA 场地实测：{} {}：最小 {:.1f} 米、10% {:.1f}、中位 {:.1f}、最大 {:.1f}，形状比 {:.2f}（10/90 分位 {:.2f}），面积 {:.0f} 平方米（等效半径 {:.1f} 米）；墙 {} 边缘 {} 到顶 {}{}",
                   m.label, m.name, mn, p10, med, mx, ratio, ratioRobust, area, std::sqrt(area / 3.14159265358979323846), walls, edges, atMax, m.why.empty() ? "" : "（" + m.why + "）");
    pbz::log::Info("影塔 QA 场地实测 JSON：{}", event);
}

void MeasureEnd() {
    tvenue::End();
    towerisolate::End();
    measure_.reset();
}

void MeasureTick() {
    if (!measure_) return;
    auto& m = *measure_;
    const uint64_t now = GetTickCount64(), inStage = now - m.stageAt;
    if (trip_.phase() != tower::trip::Phase::Inside || run_.phase != tower::Phase::Ready) { m.why = "left_ready_page"; MeasureReport(); MeasureEnd(); return; }
    if (now - m.since > 180000 && m.stage < 9) { m.why = "timeout_" + std::to_string(m.stage); m.stage = 9; }
    switch (m.stage) {
    case 1:  // 传送
        if (towertravel::Busy()) return;
        if (towertravel::Failed()) { m.why = "travel_failed:" + towertravel::Why(); m.stage = 9; return; }
        if (!towerentry::LoadingClear() || inStage < 1500) return;
        tvenue::Arrive(m.venue);
        MeasureStage(2);
        return;
    case 2: {  // 场地扫描 → 中心（与容量探测同一规则：有凸包取顶点平均，否则取边界圆心）
        tvenue::Tick();
        if (!tvenue::Ready()) return;
        std::vector<tower::arena::P3> hull; tower::arena::P3 ce; double rr = 0, lo = 0, hi = 0;
        m.venueFound = tvenue::Boundary(&hull, &ce, &rr, &lo, &hi);
        double pl[3];
        if (!MeasurePlayer(pl)) { m.why = "no_player"; m.stage = 9; return; }
        if (m.venueFound) { if (!hull.empty()) m.area.ArmHull(hull, ce, lo, hi); else { m.area.Arm(ce, rr); m.area.zmin = lo; m.area.zmax = hi; } }
        else m.area.Arm({pl[0], pl[1], pl[2]}, 0);
        if (!hull.empty()) {
            m.area.center.x = 0; m.area.center.y = 0;
            for (const auto& p : hull) { m.area.center.x += p.x / hull.size(); m.area.center.y += p.y / hull.size(); }
        }
        const double tx = m.venueFound ? m.area.center.x : pl[0], ty = m.venueFound ? m.area.center.y : pl[1];
        const double yaw = std::atan2(ty - pl[1], tx - pl[0]) * 180.0 / 3.14159265358979323846;
        std::string msg;
        if (std::hypot(tx - pl[0], ty - pl[1]) > 150 && !pbz::PointTeleport(tx, ty, pl[2], yaw, &msg)) pbz::log::Info("影塔 QA 场地实测：放到中心失败（{}），从当前位置量", msg);
        MeasureStage(3);
        return;
    }
    case 3: {  // 站稳后定中心与脚下高度
        if (pbz::TeleportBusy() || inStage < 2000) return;
        double pl[3];
        const Target* me = Player();
        if (!me || !MeasurePlayer(pl)) { m.why = "no_player"; m.stage = 9; return; }
        double g[3];
        m.c[0] = pl[0]; m.c[1] = pl[1]; m.c[2] = pl[2];
        const bool footOk = towerreach::GroundBelow(me->obj, pl[0], pl[1], pl[2] + 100, pl[2] - 500, g);
        m.foot = footOk ? g[2] : pl[2] - 90;
        pbz::log::Info("影塔 QA 场地实测：中心 {:.0f}/{:.0f}/{:.0f}，脚下 {}（{}）", pl[0], pl[1], pl[2], footOk ? std::format("z {:.0f}", g[2]) : std::string("没测到，按 −90 厘米"),
                       towerreach::GroundDiag(me->obj, pl[0], pl[1], pl[2] + 100, pl[2] - 500));
        MeasureStage(4);
        return;
    }
    case 4: {  // 每帧一个方向
        const Target* me = Player();
        if (!me) { m.why = "no_player"; m.stage = 9; return; }
        MeasureDirection(me->obj);
        if (m.dir >= m.dirs) { MeasureReport(); MeasureEnd(); }
        return;
    }
    case 9:
        MeasureReport(); MeasureEnd();
        return;
    }
}
