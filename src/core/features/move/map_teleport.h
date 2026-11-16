// Included inside move_features.cpp's anonymous namespace.
class MapTeleport;
MapTeleport* g_mapTeleport = nullptr;

// 地图传送：开启后在大地图上按 F10，传到准星处（中键是游戏的锁敌、T 是地图的重置视图，都不能用）。已加载的地方直接瞬移；
// 光标处没有任何碰撞（远处未加载）时先移到目标上空悬停，等地面加载出来再落地，找不到就传回原位
class MapTeleport : public IFeature {
public:
    const char* Id() const override { return "world.map_tp"; }
    bool IsToggle() const override { return true; }
    bool DefaultOn() const override { return true; }
    MapTeleport() { g_mapTeleport = this; }
    bool Resolve(std::string* why) override {
        if (!M().Resolve(why)) return false;
        if (!G().Resolve()) { *why = "射线接口缺失"; return false; }
        auto* cmc = ue::FindClass("CharacterMovementComponent");
        offMode_ = cmc ? ue::PropOffset(cmc, "MovementMode") : -1;
        offCustom_ = cmc ? ue::PropOffset(cmc, "CustomMovementMode") : -1;
        if (!M().fnMode_ || offMode_ < 0 || offCustom_ < 0) { *why = "悬停恢复接口缺失"; return false; }
        ready_ = true;
        return true;
    }
    // 传到已知坐标（钟）：已加载就落在离目标高度最近的面上；没加载就在目标上空悬停等地面
    bool GoToPoint(double x, double y, double tz, double yaw, std::string* msg) {
        if (!ready_) { *msg = "传送接口不可用"; return false; }
        if (pending_) { *msg = "上一次传送还在等待加载"; return false; }
        auto* p = M().Player();
        if (!p) { *msg = "未找到玩家"; return false; }
        double from[3];
        if (!M().Get(p, M().fnLoc_, from)) { *msg = "读取坐标失败"; return false; }
        // 跨图安全（见 Ground）：本地图出过原生异常就停用；加载遮罩还在不查；查的过程中出异常立即取消，不悬停不重试
        if (G().Faulted(p)) { *msg = std::string(Ground::kFaultTag) + "本地图落点查询曾发生异常，已停用（换图后恢复）"; return false; }
        if (G().LoadingActive(p)) { *msg = "目标地图仍在加载，未传送"; return false; }
        double z;
        const auto found = G().Scan(p, x, y, tz, false);  // 按目标高度选面，不需要地图层
        if (G().Faulted(p)) { *msg = std::string(Ground::kFaultTag) + "落点查询异常，已取消"; return false; }
        if (TeleportOnto(p, x, y, OrderNear(found, tz), yaw, kLift, &z)) { *msg = "已传送"; return true; }
        const double face[3] = {0, yaw, 0};
        // 悬停等加载靠本功能的每帧 Tick；地图传送被关掉时不能悬停，只能直接瞬移
        if (!enabled) {
            const double dest[3] = {x, y, tz};
            if (!M().Teleport(p, dest, face)) { *msg = "落点被阻挡，未传送"; return false; }
            *msg = "已传送（地图传送已关闭，未等待加载）";
            return true;
        }
        bool moved = false;
        for (double up : {300.0, 3000.0, 20000.0}) {
            const double air[3] = {x, y, tz + up};
            if (M().Teleport(p, air, face)) { moved = true; break; }
        }
        if (!moved) { *msg = "无法传送（目标上空被阻挡）"; return false; }
        owner_ = p;
        ownerWorld_ = Mover::WorldOf(p);
        Hover(p, true);
        memcpy(back_, from, sizeof back_);
        yaw_ = yaw; tx_ = x; ty_ = y; nearZ_ = tz; useNear_ = true;
        BeginWait();
        *msg = "正在加载…";
        return true;
    }
    bool Busy() const { return pending_ != 0; }
    // 原生调用异常后取消：结束等待、放开自己的悬停（只在同一角色同一世界时写），本地图停用落点查询
    void Abort(const char* what) {
        auto* p = M().Player();
        if (p) G().Fault(p, what);
        if (pending_ && hovering_ && p && p == owner_ && Mover::WorldOf(p) == ownerWorld_) diag::Guarded("move.point_tp", [&] { Hover(p, false); });
        hovering_ = false;
        pending_ = 0;
    }
    void Enable() override { enabled = true; down_ = true; pending_ = 0; }
    void Disable() override {
        if (pending_ && M().Player() == owner_ && owner_ && Mover::WorldOf(owner_) == ownerWorld_) {
            M().Teleport(owner_, back_, std::array<double, 3>{0, yaw_, 0}.data());
            Hover(owner_, false);
        }
        hovering_ = false;
        enabled = false; pending_ = 0;
    }
    void Tick(float dt) override {
        if (pending_) { Pending(); return; }
        const bool down = (GetAsyncKeyState(VK_F10) & 0x8000) != 0;  // F10
        const bool pressed = down && !down_;
        down_ = down;
        if (!pressed || !GameFocused()) return;
        std::string msg;
        const bool ok = Go(&msg);
        log::Info("地图传送：{}", msg);
        if (!ok) MessageBeep(MB_ICONHAND);
    }

private:
    bool Go(std::string* msg) {
        std::string diag;
        auto r = Locator().Locate(&diag);
        log::Info("地图传送换算：{}", diag);
        if (!r.ok) { *msg = r.msg; return false; }
        auto* p = M().Player();
        if (!p) { *msg = "未找到玩家"; return false; }
        double from[3], rot[3];
        if (!M().Get(p, M().fnLoc_, from) || !M().Get(p, M().fnRot_, rot)) { *msg = "读取坐标失败"; return false; }
        if(!Locator().Close(r.map)){*msg="地图菜单尚未正常退出，未执行传送";return false;}
        if(M().Player()!=p){*msg="菜单退出期间角色已变化，请重试";return false;}
        const int layer = r.layer > 0 ? r.layer : G().CurrentLayer(p);
        auto surfaces = G().Scan(p, r.x, r.y);
        const int hitsAt = G().hits;
        // 准星处落不了地（陡坡 / 屋脊 / 树冠）但这里已加载：在周围 1.5 / 3 / 5 米一圈各 8 个方向找能站的点
        if (OrderSurfaces(surfaces, layer).empty() && hitsAt > 0) {
            for (double radius : {150.0, 300.0, 500.0}) {
                for (int k = 0; k < 8 && surfaces.empty(); ++k) {
                    const double a = k * 3.14159265358979 / 4, nx = r.x + std::cos(a) * radius, ny = r.y + std::sin(a) * radius;
                    if (auto around = G().Scan(p, nx, ny); !around.empty()) { surfaces = around; r.x = nx; r.y = ny; }
                }
                if (!surfaces.empty()) { log::Info("地图传送：准星处无法落地，改落在 {:.1f} 米外", radius / 100); break; }
            }
        }
        double z;
        if (const auto order = OrderSurfaces(surfaces, layer); !order.empty()) {
            if (!TeleportOnto(p, r.x, r.y, order, rot[1], kLift, &z)) {
                *msg = std::format("无法传送（{} 个落点都被阻挡）", order.size());
                log::Info("地图传送 射线命中：{}", G().hitInfo);
                return false;
            }
            *msg = std::format("已传送到 ({:.0f}, {:.0f}, {:.0f})，层 {}，参考图标 {} 个", r.x, r.y, z, layer, r.icons);
            return true;
        }
        // 光标处什么都没有：可能是远处未加载，交给游戏原生传送加载后再找
        if (hitsAt > 0 && Dist2(from, r.x, r.y) < kNear * kNear) {
            *msg = std::format("无法传送（此处及周围 5 米都无法落地：命中 {}）", hitsAt);
            log::Info("地图传送 射线命中：{}", G().hitInfo);
            return false;
        }
        // 远处未加载：不走游戏的钟传送流程（不给钟编号时会停在"到达"运镜、角色锁住）。
        // 直接移到目标上空悬停（飞行模式、速度清零），等世界分区把那里的地面加载出来再落地；20 秒找不到就传回原位
        {
            const double face[3] = {0, rot[1], 0};
            bool moved = false;
            for (double up : {3000.0, 20000.0, 60000.0}) {
                const double air[3] = {r.x, r.y, from[2] + up};
                if (M().Teleport(p, air, face)) { moved = true; break; }
            }
            if (!moved) { *msg = "无法传送（目标上空被阻挡）"; return false; }
            owner_ = p;
            ownerWorld_ = Mover::WorldOf(p);
            Hover(p, true);
        }
        memcpy(back_, from, sizeof back_);
        yaw_ = rot[1];
        tx_ = r.x; ty_ = r.y; layer_ = layer; useNear_ = false;
        BeginWait();
        *msg = "远处，正在加载…";
        return true;
    }
    // 悬停等加载：每帧保持飞行模式、速度清零；每 0.5 秒找一次地面，找到就落地，最多 20 秒
    void BeginWait() {
        pending_ = 1;
        started_ = scanned_ = GetTickCount64();
    }
    void Pending() {
        const auto now = GetTickCount64();
        const double wait = (now - started_) / 1000.0;
        auto* p = M().Player();
        if (!p || p != owner_ || Mover::WorldOf(p) != ownerWorld_) { pending_ = 0; hovering_ = false; return; }
        Hover(p, true);
        if (now - scanned_ < 500) return;  // 1.0.1 改回 0.5 秒：0.1 秒在慢机器上会先落到还没流送完整的地面，和场地高度对不上（"未处于目标场地"）
        scanned_ = now;
        double z;
        const auto found = useNear_ ? G().Scan(p, tx_, ty_, nearZ_, false) : G().Scan(p, tx_, ty_);
        if (G().Faulted(p)) {  // 等加载期间落点查询出原生异常：立即停，放开悬停，不传回（这个世界的原生调用已不可信）
            pending_ = 0;
            Hover(p, false);
            log::Info("地图传送：等加载期间落点查询异常，已取消（等了 {:.1f} 秒）", wait);
            return;
        }
        if (TeleportOnto(p, tx_, ty_, useNear_ ? OrderNear(found, nearZ_) : OrderSurfaces(found, layer_), yaw_, kLift, &z)) {
            Hover(p, false);
            log::Info("地图传送：加载后落地 ({:.0f}, {:.0f}, {:.0f})，等了 {:.1f} 秒", tx_, ty_, z, wait);
            pending_ = 0;
            return;
        }
        if (wait < 20) return;
        pending_ = 0;
        Hover(p, false);
        log::Info("地图传送：无法传送（20 秒内目标处没有可站的地面），传回原位");
        MessageBeep(MB_ICONHAND);
        M().Teleport(p, back_, std::array<double, 3>{0, yaw_, 0}.data());
    }
    // Save both native movement bytes once; do not overwrite them every Tick.
    void Hover(ue::UObject* p, bool on) {
        auto* cmc = M().Movement(p);
        if (!cmc || !M().fnMode_) return;
        if (on && !hovering_) {
            savedMode_ = ue::Get<uint8_t>(cmc + offMode_);
            savedCustom_ = ue::Get<uint8_t>(cmc + offCustom_);
            hovering_ = true;
        }
        uint8_t args[8] = {on ? uint8_t(5) : savedMode_, on ? uint8_t(0) : savedCustom_};
        pe::Call(cmc, M().fnMode_, args);
        if (!on) hovering_ = false;
        if (on) {
            if (offVel_ == -2) { auto* mc = ue::FindClass("MovementComponent"); offVel_ = mc ? ue::PropOffset(mc, "Velocity") : -1; }
            if (offVel_ >= 0) { const double zero[3] = {}; ue::Write(cmc + offVel_, zero, 24); }
        }
    }
    static double Dist2(const double a[3], double x, double y) { return (a[0] - x) * (a[0] - x) + (a[1] - y) * (a[1] - y); }
    static bool GameFocused() {
        DWORD pid = 0;
        GetWindowThreadProcessId(GetForegroundWindow(), &pid);
        return pid == GetCurrentProcessId();
    }
    static constexpr double kLift = 100, kNear = 15000;  // 落点抬高（胶囊半高）；150 米内视为已加载
    bool down_ = true, ready_ = false, useNear_ = false;
    double nearZ_ = 0;
    int pending_ = 0, layer_ = -1;
    ULONGLONG started_ = 0, scanned_ = 0;
    double back_[3]{}, yaw_ = 0, tx_ = 0, ty_ = 0;
    ue::UObject* owner_ = nullptr;
    std::string ownerWorld_;
    int offVel_ = -2, offMode_ = -1, offCustom_ = -1;
    bool hovering_ = false;
    uint8_t savedMode_ = 1, savedCustom_ = 0;
};

