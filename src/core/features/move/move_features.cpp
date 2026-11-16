// 移动：坐标列表（保存当前位置 / 传送 / 重命名 / 删除，跨重启保存）、传送到最近敌人、无限空中跳、跳跃高度、飞行穿墙。
// 传送用 Actor.K2_TeleportTo（引擎自带，带落点修正）；坐标与所在地图（World 名）一起保存，不同地图不传。
#include <algorithm>
#include <map>
#include <array>
#include <cmath>
#include <cstring>
#include <format>
#include <fstream>
#include <sstream>
#include <unordered_map>
#include <vector>

#include <windows.h>

#include "../../host/log.h"
#include "../../services/attributes.h"
#include "../../services/diag.h"
#include "../../services/inventory.h"
#include "../../services/process_event.h"
#include "../feature.h"

namespace pbz {

// 传到已知坐标（钟 / 坐标列表）：用地图传送的"未加载就悬停等地面"流程，落在离目标高度最近的面上。定义在地图传送类之后
bool PointTeleport(double x, double y, double z, double yaw, std::string* msg);

namespace {

struct Pose { double loc[3]{}, rot[3]{}; std::string world, name; int id = 0; };

class Mover {
public:
    bool Resolve(std::string* why) {
        if (resolved_) return true;
        auto* actor = ue::FindClass("Actor");
        auto* chr = ue::FindClass("Character");
        auto* cmc = ue::FindClass("CharacterMovementComponent");
        if (!actor || !chr || !cmc) { *why = "角色类未加载"; return false; }
        fnLoc_ = pe::FindFunction(actor, "K2_GetActorLocation");
        fnRot_ = pe::FindFunction(actor, "K2_GetActorRotation");
        fnTp_ = pe::FindFunction(actor, "K2_TeleportTo");
        fnCollide_ = pe::FindFunction(actor, "SetActorEnableCollision");
        fnMode_ = pe::FindFunction(cmc, "SetMovementMode");
        offCmc_ = ue::PropOffset(chr, "CharacterMovement");
        offJumpCount_ = ue::PropOffset(chr, "JumpMaxCount");
        offJumpZ_ = ue::PropOffset(cmc, "JumpZVelocity");
        offGravity_ = ue::PropOffset(cmc, "GravityScale");
        offFly_ = ue::PropOffset(cmc, "MaxFlySpeed");
        npcCls_ = ue::FindClass("PBZCharacter_NPC");
        if (npcCls_) offMonster_ = ue::PropOffset(npcCls_, "BIsMonster");
        if (!fnLoc_ || !fnRot_ || !fnTp_ || offCmc_ < 0) { *why = "传送接口缺失"; return false; }
        resolved_ = true;
        Load();
        return true;
    }

    ue::UObject* Player() {
        for (auto& t : attr::Targets())
            if (t.who == attr::Who::Player && attr::Alive(t)) return t.obj;
        // 目标表平时只在有功能开着时刷新：当场重建一轮。全表重建约几十毫秒，最多 5 秒一次（菜单 / 加载中找不到玩家时不能每帧做）
        const auto now = GetTickCount64();
        if (now - lastRebuild_ < 5000) return nullptr;
        lastRebuild_ = now;
        for (int i = ue::Num() / 2048 + 2; i > 0; --i) attr::Refresh(0);
        for (auto& t : attr::Targets())
            if (t.who == attr::Who::Player && attr::Alive(t)) return t.obj;
        return nullptr;
    }
    ue::UObject* Movement(ue::UObject* p) { return p ? ue::Get<ue::UObject*>(p + offCmc_) : nullptr; }

    bool Get(ue::UObject* a, ue::UObject* fn, double out[3]) {
        uint8_t ret[32] = {};
        pe::Call(a, fn, ret);
        memcpy(out, ret, 24);
        return std::isfinite(out[0]) && std::isfinite(out[1]) && std::isfinite(out[2]);
    }
    bool Teleport(ue::UObject* a, const double loc[3], const double rot[3]) {
        uint8_t args[64] = {};
        memcpy(args, loc, 24);
        memcpy(args + 24, rot, 24);
        pe::Call(a, fnTp_, args);
        return args[48] != 0;
    }
    static std::string WorldOf(ue::UObject* actor) {
        // Actor → Level → World
        auto* level = ue::Get<ue::UObject*>(actor + ue::Layout::ObjOuter);
        auto* world = level ? ue::Get<ue::UObject*>(level + ue::Layout::ObjOuter) : nullptr;
        return world ? ue::ObjName(world) : "";
    }

    // 坐标列表：list / save（name 可空）/ tp / rename / del，返回 {"op":"pos","ok","msg","current","list":[…]}
    std::string Command(const std::string& cmd, int id, const std::string& name) {
        auto* p = Player();
        if (p) current_ = WorldOf(p);
        auto reply = [&](bool ok, const std::string& msg) {
            return std::format(R"({{"op":"pos","ok":{},"msg":"{}","current":"{}","list":[{}]}})", ok ? "true" : "false", Esc(msg), Esc(current_), ListJson());
        };
        if (cmd == "list") return reply(true, "");
        if (cmd == "save") {
            if (!p) return reply(false, "未找到玩家");
            Pose s;
            if (!Get(p, fnLoc_, s.loc) || !Get(p, fnRot_, s.rot)) return reply(false, "读取坐标失败");
            s.world = current_;
            s.id = nextId_++;
            s.name = name.empty() ? std::format("坐标 {}", s.id) : name;
            points_.push_back(s);
            Store();
            return reply(true, "已保存 " + s.name);
        }
        auto it = std::find_if(points_.begin(), points_.end(), [&](const Pose& x) { return x.id == id; });
        if (it == points_.end()) return reply(false, "坐标不存在");
        if (cmd == "del") { const auto n = it->name; points_.erase(it); Store(); return reply(true, "已删除 " + n); }
        if (cmd == "rename") { if (!name.empty()) it->name = name; Store(); return reply(true, "已重命名"); }
        if (cmd == "tp") {
            if (!p) return reply(false, "未找到玩家");
            if (it->world != current_) return reply(false, "该坐标在另一张地图");
            // 走地图传送的悬停等加载流程（直接瞬移到未加载区域会掉进虚空）；保存的是角色中心高度，落点取地面
            std::string m;
            if (!PointTeleport(it->loc[0], it->loc[1], it->loc[2] - 90, it->rot[1], &m)) return reply(false, m);
            return reply(true, (m == "已传送" ? "已传送到 " : m + " ") + it->name);
        }
        return reply(false, "未知操作");
    }

    // 最近的活怪物（BIsMonster）身后 2 米
    bool ToEnemy(std::string* msg) {
        auto* p = Player();
        if (!p) { *msg = "未找到玩家"; return false; }
        double pl[3];
        if (!Get(p, fnLoc_, pl)) { *msg = "读取坐标失败"; return false; }
        const int offHp = attr::AttrOffset("Health");
        const attr::Target* best = nullptr;
        double bestD = 1e18, bl[3]{};
        for (auto& t : attr::Targets()) {
            if (t.who != attr::Who::Enemies || !attr::Alive(t) || !npcCls_ || !ue::IsA(t.obj, npcCls_) ||
                (offMonster_ >= 0 && !ue::Get<uint8_t>(t.obj + offMonster_)) || (offHp >= 0 && *attr::AttrCurrent(t, offHp) <= 0)) continue;
            double e[3];
            if (!Get(t.obj, fnLoc_, e)) continue;
            const double d = (e[0] - pl[0]) * (e[0] - pl[0]) + (e[1] - pl[1]) * (e[1] - pl[1]) + (e[2] - pl[2]) * (e[2] - pl[2]);
            if (d < bestD) { bestD = d; best = &t; memcpy(bl, e, sizeof bl); }
        }
        if (!best) { *msg = "附近没有敌人"; return false; }
        double rot[3];
        Get(best->obj, fnRot_, rot);
        const double yaw = rot[1] * 3.14159265358979 / 180;
        const double dest[3] = {bl[0] - std::cos(yaw) * 200, bl[1] - std::sin(yaw) * 200, bl[2] + 20};
        const double face[3] = {0, rot[1], 0};
        if (!Teleport(p, dest, face)) { *msg = "落点被阻挡，未传送"; return false; }
        *msg = std::format("已传送到敌人身后（{:.0f} 米外）", std::sqrt(bestD) / 100);
        return true;
    }

    ue::UObject *fnLoc_ = nullptr, *fnRot_ = nullptr, *fnTp_ = nullptr, *fnCollide_ = nullptr, *fnMode_ = nullptr, *npcCls_ = nullptr;
    int offCmc_ = -1, offJumpCount_ = -1, offJumpZ_ = -1, offGravity_ = -1, offFly_ = -1, offMonster_ = -1;

private:
    static std::string Esc(const std::string& v) {
        std::string o;
        for (char c : v) {
            if (c == '"' || c == '\\') o += '\\';
            if (c != '\n' && c != '\r' && c != '\t') o += c;
        }
        return o;
    }
    std::string ListJson() const {
        std::string o;
        for (auto& s : points_)
            o += std::format(R"({}{{"id":{},"name":"{}","world":"{}","here":{},"x":{:.0f},"y":{:.0f},"z":{:.0f}}})", o.empty() ? "" : ",", s.id,
                             Esc(s.name), Esc(s.world), current_.empty() || s.world == current_ ? "true" : "false", s.loc[0], s.loc[1], s.loc[2]);  // 还没找到玩家时不判"其他地图"
        return o;
    }
    // 存档：positions.tsv，每行 id、地图、名称、坐标 3、朝向 3（制表符分隔，名称可含中文 / 空格）
    static std::wstring File() {
        wchar_t d[MAX_PATH] = {};
        GetEnvironmentVariableW(L"LOCALAPPDATA", d, MAX_PATH);
        return std::wstring(d) + L"\\PBZLab\\positions.tsv";
    }
    void Store() {
        std::ofstream f(File(), std::ios::trunc | std::ios::binary);
        for (auto& s : points_)
            f << s.id << '\t' << s.world << '\t' << s.name << '\t' << s.loc[0] << '\t' << s.loc[1] << '\t' << s.loc[2] << '\t'
              << s.rot[0] << '\t' << s.rot[1] << '\t' << s.rot[2] << '\n';
    }
    void Load() {
        std::ifstream f(File(), std::ios::binary);
        std::string line;
        while (std::getline(f, line)) {
            std::vector<std::string> c;
            std::stringstream in(line);
            for (std::string x; std::getline(in, x, '\t');) c.push_back(x);
            if (c.size() != 9) continue;
            Pose s;
            try {
                s.id = std::stoi(c[0]);
                s.world = c[1];
                s.name = c[2];
                for (int k = 0; k < 3; ++k) { s.loc[k] = std::stod(c[3 + k]); s.rot[k] = std::stod(c[6 + k]); }
            } catch (...) { continue; }
            nextId_ = std::max(nextId_, s.id + 1);
            points_.push_back(s);
        }
    }

    bool resolved_ = false;
    unsigned long long lastRebuild_ = 0;
    std::vector<Pose> points_;
    std::string current_;
    int nextId_ = 1;
};

Mover& M() {
    static Mover m;
    return m;
}

class MoveAction : public IFeature {
public:
    enum Kind { ToEnemy };
    MoveAction(const char* id, Kind k) : id_(id), kind_(k) {}
    const char* Id() const override { return id_; }
    bool Resolve(std::string* why) override { return M().Resolve(why); }
    void Enable() override {}
    void Disable() override {}
    void Tick(float) override {}
    bool Run(std::string* msg, int) override { return kind_ == ToEnemy && M().ToEnemy(msg); }

private:
    const char* id_;
    Kind kind_;
};

// 玩家角色 / 移动组件上的一个数值字段：开启时记原值，按开关固定值或滑条倍率写入，每秒复写，关闭写回
class PlayerField : public IFeature {
public:
    enum Where { OnCharacter, OnMovement };
    enum Type { Int, Float };
    PlayerField(const char* id, Where w, int Mover::*off, Type t, bool toggle, double fixed)
        : id_(id), where_(w), off_(off), type_(t), toggle_(toggle), fixed_(fixed) {}
    const char* Id() const override { return id_; }
    bool IsToggle() const override { return toggle_; }
    bool Resolve(std::string* why) override {
        if (!M().Resolve(why)) return false;
        if (M().*off_ < 0) { *why = "字段不存在"; return false; }
        return true;
    }
    void Enable() override {
        obj_ = Target();
        if (!obj_) { enabled = true; return; }
        orig_ = Read(obj_);
        enabled = true;
        Apply();
    }
    void Disable() override {
        if (obj_ && Target() == obj_) Write(obj_, orig_);
        obj_ = nullptr;
        enabled = false;
    }
    void SetValue(double v) override { value = v; if (enabled) Apply(); }
    void Tick(float dt) override {
        if ((since_ += dt) < 1.0f) return;
        since_ = 0;
        auto* t = Target();
        if (t != obj_) { obj_ = t; if (t) orig_ = Read(t); }  // 换角色 / 读档后重新记原值
        Apply();
    }

private:
    ue::UObject* Target() {
        auto* p = M().Player();
        return where_ == OnCharacter ? p : M().Movement(p);
    }
    double Want() const { return toggle_ ? fixed_ : orig_ * value; }
    void Apply() { if (obj_ && std::abs(Read(obj_) - Want()) > 1e-4) Write(obj_, Want()); }
    double Read(ue::UObject* o) const { return type_ == Int ? ue::Get<int32_t>(o + M().*off_) : ue::Get<float>(o + M().*off_); }
    void Write(ue::UObject* o, double v) {
        if (type_ == Int) { const int32_t n = static_cast<int32_t>(std::lround(v)); ue::Write(o + M().*off_, &n, 4); }
        else { const float f = static_cast<float>(v); ue::Write(o + M().*off_, &f, 4); }
    }

    const char* id_;
    Where where_;
    int Mover::*off_;
    Type type_;
    bool toggle_;
    double fixed_, orig_ = 0;
    ue::UObject* obj_ = nullptr;
    float since_ = 0;
};

// 无限空中跳：本作跳跃是能力（GA_Hun_Jump），不看引擎的 JumpMaxCount。
// 开启后在空中（移动模式 Falling）按空格：先 K2_EndAbility 结束仍在进行的跳跃能力，再用 ASC 重新激活它
class AirJump : public IFeature {
public:
    const char* Id() const override { return "world.inf_jump"; }
    bool IsToggle() const override { return true; }
    bool Resolve(std::string* why) override {
        if (!M().Resolve(why)) return false;
        jumpCls_ = ue::FindClass("GA_Hun_Jump_C");
        auto* asc = ue::FindClass("AbilitySystemComponent");
        auto* ga = ue::FindClass("GameplayAbility");
        auto* cmc = ue::FindClass("CharacterMovementComponent");
        auto* chr = ue::FindClass("PBZCharacter");
        if (!jumpCls_ || !asc || !ga || !cmc || !chr) { *why = "跳跃能力未加载"; return false; }
        fnActivate_ = pe::FindFunction(asc, "TryActivateAbilityByClass");
        fnEnd_ = pe::FindFunction(ga, "K2_EndAbility");
        offMode_ = ue::PropOffset(cmc, "MovementMode");
        offAsc_ = ue::PropOffset(chr, "AbilitySystemComponent");
        if (!fnActivate_ || !fnEnd_ || offMode_ < 0 || offAsc_ < 0) { *why = "跳跃接口缺失"; return false; }
        return true;
    }
    void Enable() override { enabled = true; down_ = true; }
    void Disable() override { enabled = false; }
    void Tick(float) override {
        const bool down = (GetAsyncKeyState(VK_SPACE) & 0x8000) != 0;
        const bool pressed = down && !down_;
        down_ = down;
        if (!pressed || !GameFocused()) return;
        auto* p = M().Player();
        auto* cmc = M().Movement(p);
        if (!cmc || ue::Get<uint8_t>(cmc + offMode_) != 3) return;  // 只在空中（MOVE_Falling）补跳；地面起跳交给游戏
        if (!jump_ || ue::ClassOf(jump_) != jumpCls_ || ue::Get<ue::UObject*>(jump_ + ue::Layout::ObjOuter) != p) {
            jump_ = nullptr;
            ue::ForEach([&](int, ue::UObject* o) {
                if (ue::ClassOf(o) == jumpCls_ && ue::Get<ue::UObject*>(o + ue::Layout::ObjOuter) == p) { jump_ = o; return false; }
                return true;
            });
        }
        if (jump_) pe::Call(jump_, fnEnd_, nullptr);
        auto* asc = ue::Get<ue::UObject*>(p + offAsc_);
        if (!asc) return;
        uint8_t args[32] = {};
        memcpy(args, &jumpCls_, 8);  // InAbilityToActivate
        args[8] = 1;                 // bAllowRemoteActivation
        pe::Call(asc, fnActivate_, args);
    }

private:
    static bool GameFocused() {
        DWORD pid = 0;
        GetWindowThreadProcessId(GetForegroundWindow(), &pid);
        return pid == GetCurrentProcessId();
    }
    ue::UObject *jumpCls_ = nullptr, *fnActivate_ = nullptr, *fnEnd_ = nullptr, *jump_ = nullptr;
    int offMode_ = -1, offAsc_ = -1;
    bool down_ = true;
};

// 飞行穿墙：WASD 沿镜头方向、空格上升、Ctrl 下降，直接按帧移位（无惯性）；关掉碰撞与重力，关闭时恢复 Walking 与碰撞
class Noclip : public IFeature {
public:
    const char* Id() const override { return "world.noclip"; }
    bool IsToggle() const override { return true; }
    bool Resolve(std::string* why) override {
        if (!M().Resolve(why)) return false;
        if (!M().fnMode_ || !M().fnCollide_) { *why = "移动模式接口缺失"; return false; }
        return true;
    }
    void Enable() override {
        auto* pawn = ue::FindClass("Pawn");
        auto* ctrl = ue::FindClass("Controller");
        auto* mc = ue::FindClass("MovementComponent");
        auto* actor = ue::FindClass("Actor");
        offCtrl_ = pawn ? ue::PropOffset(pawn, "Controller") : -1;
        offRot_ = ctrl ? ue::PropOffset(ctrl, "ControlRotation") : -1;
        offVel_ = mc ? ue::PropOffset(mc, "Velocity") : -1;
        fnSet_ = actor ? pe::FindFunction(actor, "K2_SetActorLocation") : nullptr;
        enabled = true;
        since_ = 1;
    }
    void Disable() override {
        if (auto* p = M().Player()) { Mode(p, 1); Collide(p, true); }
        enabled = false;
    }
    // 直接操控：每帧按键决定位移（W/S 沿镜头前后、A/D 左右、空格上升、Ctrl 下降），速度清零防止滑行；
    // 移动模式 Flying 只用来去掉重力，游戏会切回去所以每 0.2 秒复写一次
    void Tick(float dt) override {
        auto* p = M().Player();
        if (!p) return;
        if ((since_ += dt) >= 0.2f) { since_ = 0; Mode(p, 5); Collide(p, false); }
        auto* cmc = M().Movement(p);
        if (cmc && offVel_ >= 0) { const double zero[3] = {}; ue::Write(cmc + offVel_, zero, 24); }
        if (!GameFocused() || !fnSet_ || offCtrl_ < 0 || offRot_ < 0) return;
        auto key = [](int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; };
        const double fwd = key('W') - key('S'), right = key('D') - key('A'), up = key(VK_SPACE) - key(VK_CONTROL);
        if (!fwd && !right && !up) return;
        auto* ctrl = ue::Get<ue::UObject*>(p + offCtrl_);
        if (!ctrl) return;
        const double pitch = ue::Get<double>(ctrl + offRot_) * kDeg, yaw = ue::Get<double>(ctrl + offRot_ + 8) * kDeg;
        // 前后沿镜头朝向（含俯仰，可朝上下看着飞），左右水平
        double d[3] = {fwd * std::cos(pitch) * std::cos(yaw) - right * std::sin(yaw),
                       fwd * std::cos(pitch) * std::sin(yaw) + right * std::cos(yaw),
                       fwd * std::sin(pitch) + up};
        const double len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        if (len < 1e-6) return;
        double loc[3];
        if (!M().Get(p, M().fnLoc_, loc)) return;
        const double step = kSpeed * std::min(dt, 0.1f) / len;
        for (int i = 0; i < 3; ++i) loc[i] += d[i] * step;
        // K2_SetActorLocation(NewLocation, bSweep=false, out SweepHitResult, bTeleport=true)
        std::vector<uint8_t> args(0x200, 0);
        int offLoc = -1, offSweep = -1, offTp = -1;
        for (auto* q = ue::Get<uint8_t*>(fnSet_ + ue::Layout::StructChildProps); q; q = ue::Get<uint8_t*>(q + ue::Layout::FieldNext)) {
            const auto n = ue::Name(ue::Get<uint32_t>(q + ue::Layout::FieldName));
            const int o = ue::Get<int32_t>(q + ue::Layout::PropOffset);
            if (n == "NewLocation") offLoc = o; else if (n == "bSweep") offSweep = o; else if (n == "bTeleport") offTp = o;
        }
        if (offLoc < 0 || offTp < 0 || offTp >= 0x200) return;
        memcpy(args.data() + offLoc, loc, 24);
        if (offSweep >= 0) args[offSweep] = 0;
        args[offTp] = 1;
        pe::Call(p, fnSet_, args.data());
    }

private:
    static constexpr double kDeg = 3.14159265358979 / 180, kSpeed = 1200;  // 12 米/秒
    static bool GameFocused() {
        DWORD pid = 0;
        GetWindowThreadProcessId(GetForegroundWindow(), &pid);
        return pid == GetCurrentProcessId();
    }
    ue::UObject* fnSet_ = nullptr;
    int offCtrl_ = -1, offRot_ = -1, offVel_ = -1;
    void Mode(ue::UObject* p, uint8_t m) {
        auto* cmc = M().Movement(p);
        if (!cmc) return;
        uint8_t args[8] = {m, 0};
        pe::Call(cmc, M().fnMode_, args);
    }
    void Collide(ue::UObject* p, bool on) {
        uint8_t args[8] = {static_cast<uint8_t>(on)};
        pe::Call(p, M().fnCollide_, args);
    }
    float since_ = 0;
};

}  // namespace

namespace {

// 反射调用参数缓冲：按 UFunction 参数名取偏移
struct Args {
    ue::UObject* fn;
    std::vector<uint8_t> buf;
    explicit Args(ue::UObject* f) : fn(f), buf(0x400, 0) {}
    uint8_t* At(std::string_view name) {
        for (auto* p = ue::Get<uint8_t*>(fn + ue::Layout::StructChildProps); p; p = ue::Get<uint8_t*>(p + ue::Layout::FieldNext))
            if (ue::Name(ue::Get<uint32_t>(p + ue::Layout::FieldName)) == name) {
                const int o = ue::Get<int32_t>(p + ue::Layout::PropOffset);
                if (o >= 0 && o < 0x3C0) return buf.data() + o;
            }
        static uint8_t sink[64];
        return sink;
    }
};

// 子系统对象缓存：全表扫描几十毫秒，界面每 5 秒刷新钟列表，不能每次都扫。缓存失效（换图 / 读档）时才重找，且最多 10 秒一次
// 对象仍存活：对象表里它自己的索引处还是它（换图 / 读档后旧世界的子系统会被释放，内存里的类指针不会清零，不能只看类）
bool Live(ue::UObject* o) {
    if (!o) return false;
    const int32_t idx = ue::Get<int32_t>(o + ue::Layout::ObjIndex, -1);
    return idx >= 0 && ue::Obj(idx) == o;
}

ue::UObject* FindSubsystem(const char* cls) {
    struct Entry { ue::UObject *cls = nullptr, *obj = nullptr; unsigned long long lastScan = 0; };
    static std::unordered_map<std::string, Entry> cache;
    auto& e = cache[cls];
    auto* c = ue::FindClass(cls);
    if (!c) return nullptr;
    if (e.obj && e.cls == c && Live(e.obj) && ue::ClassOf(e.obj) == c && !ue::IsDefault(e.obj)) return e.obj;
    const auto now = GetTickCount64();
    if (e.lastScan && now - e.lastScan < 10000) return nullptr;
    e.lastScan = now;
    e.cls = c;
    e.obj = nullptr;
    ue::ForEach([&](int, ue::UObject* o) {
        if (ue::ClassOf(o) == c && !ue::IsDefault(o)) { e.obj = o; return false; }
        return true;
    });
    return e.obj;
}

bool SameLevel(std::string a, std::string b) {
    std::transform(a.begin(), a.end(), a.begin(), ::tolower);
    std::transform(b.begin(), b.end(), b.begin(), ::tolower);
    return !a.empty() && a == b;
}

}  // namespace

// 传送点（钟）：list = 已激活的钟 ID；tp = 同一张地图直接瞬移，跨地图走游戏自己的钟传送（PBZSaveLoadSubsystem.TeleportTo，带加载界面）。
// 落点 / 朝向由界面按 shared/bells.json（取自 DT_BellData.TeleportTransform）传入
std::string BellCommand(const std::string& cmd, const std::string& id, const double loc[3], double yaw) {
    auto fail = [](const std::string& m) { return std::format(R"({{"op":"bell","ok":false,"msg":"{}","active":[]}})", m); };
    std::string why;
    if (!M().Resolve(&why)) return fail(why);
    auto* bells = FindSubsystem("BellManager");
    if (!bells) return fail("传送点系统未就绪（请先进入游戏）");
    if (cmd == "list") {
        auto* fn = pe::FindFunction(ue::ClassOf(bells), "GetAllActivateBellIDs");
        if (!fn) return fail("接口缺失");
        Args a(fn);
        pe::Call(bells, fn, a.buf.data());
        auto* arr = a.At("ReturnValue");
        auto* data = *reinterpret_cast<uint8_t**>(arr);
        const int n = *reinterpret_cast<int32_t*>(arr + 8);
        std::string list;
        for (int i = 0; data && i < n && i < 1024; ++i) {
            const auto name = ue::Name(ue::Get<uint32_t>(data + 8 * i));
            if (!name.empty()) list += std::format(R"({}"{}")", list.empty() ? "" : ",", name);
        }
        return std::format(R"({{"op":"bell","ok":true,"msg":"","active":[{}]}})", list);
    }
    if (cmd != "tp") return fail("未知操作");
    auto* p = M().Player();
    if (!p) return fail("未找到玩家");
    const uint64_t bellName = inv::ToFName(id);
    auto* fnLevel = pe::FindFunction(ue::ClassOf(bells), "GetLevelNameFromBellID");
    if (!bellName || !fnLevel) return fail("传送点无效");
    Args lv(fnLevel);
    memcpy(lv.At("BellID"), &bellName, 8);
    pe::Call(bells, fnLevel, lv.buf.data());
    const uint64_t level = *reinterpret_cast<uint64_t*>(lv.At("ReturnValue"));
    const auto levelStr = ue::Name(static_cast<uint32_t>(level));
    const double rot[3] = {0, yaw, 0};
    if (SameLevel(levelStr, Mover::WorldOf(p))) {
        // 同一张地图：钟附近可能还没加载（直接瞬移会穿过未加载的地面往下掉）—— 走地图传送的悬停等加载流程
        std::string m;
        if (!PointTeleport(loc[0], loc[1], loc[2], yaw, &m)) return fail(m);
        log::Info("传送点：{} {}", id, m);
        return std::format(R"({{"op":"bell","ok":true,"msg":"{}","active":[]}})", m);
    }
    auto* save = FindSubsystem("PBZSaveLoadSubsystem");
    auto* fnTp = save ? pe::FindFunction(ue::ClassOf(save), "TeleportTo") : nullptr;
    if (!fnTp || levelStr.empty() || levelStr == "None") return fail("跨地图传送接口不可用");
    Args t(fnTp);
    memcpy(t.At("NewLevelName"), &level, 8);
    memcpy(t.At("BellID"), &bellName, 8);
    // FTransform（双精度）：Rotation 四元数 xyzw、Translation、Scale3D，各 0x20
    double* tf = reinterpret_cast<double*>(t.At("TeleportTransform"));
    const double h = yaw * 3.14159265358979 / 360;
    tf[0] = 0; tf[1] = 0; tf[2] = std::sin(h); tf[3] = std::cos(h);
    tf[4] = loc[0]; tf[5] = loc[1]; tf[6] = loc[2]; tf[7] = 0;
    tf[8] = 1; tf[9] = 1; tf[10] = 1; tf[11] = 0;
    pe::Call(save, fnTp, t.buf.data());
    log::Info("传送点：跨地图传送到 {}（{}）", id, levelStr);
    return R"({"op":"bell","ok":true,"msg":"正在加载地图…","active":[]})";
}

namespace {

// 地图光标 → 世界坐标。地图控件的 TopLeft/Scale 等字段由脚本层维护、反射里一直是 0，不能用；
// 改用图标：每个 MapWidgetSlotBase 有世界坐标 Location，又放在同一块 CanvasPanel 上（LayoutData 给出画布内位置），
// 对"画布位置 → 世界 XY"做最小二乘仿射拟合；光标屏幕像素经画布的 CachedGeometry.AbsoluteToLocal 换成画布位置后代入。
class MapLocator {
public:
    struct Result {
        bool ok = false;
        std::string msg;
        double x = 0, y = 0, rms = 0;  // 光标处世界 XY；拟合残差（厘米）
        int icons = 0, layer = -1;     // 参与拟合的图标数；地图当前显示层（未知为 -1）
        ue::UObject* map = nullptr;
    };

    bool Resolve() {
        if (ok_) return true;
        mapCls_ = ue::FindClass("WB_MapNew_C");
        slotCls_ = ue::FindClass("MapWidgetSlotBase");
        auto* widget = ue::FindClass("Widget");
        auto* panelSlot = ue::FindClass("PanelSlot");
        auto* canvasSlot = ue::FindClass("CanvasPanelSlot");
        auto* lib = ue::FindClass("SlateBlueprintLibrary");
        if (!mapCls_ || !slotCls_ || !widget || !panelSlot || !canvasSlot || !lib) return false;
        offLoc_ = ue::PropOffset(slotCls_, "Location");
        offSlot_ = ue::PropOffset(widget, "Slot");
        offParent_ = ue::PropOffset(panelSlot, "Parent");
        offLayout_ = ue::PropOffset(canvasSlot, "LayoutData");
        offLayer_ = ue::PropOffset(mapCls_, "CurrentLayer");
        offCursor_ = ue::PropOffset(mapCls_, "WB_Cursor");
        canvasSlotCls_ = canvasSlot;
        fnGeom_ = pe::FindFunction(widget, "GetCachedGeometry");
        fnVisible_ = pe::FindFunction(widget, "IsVisible");
        fnA2L_ = pe::FindFunction(lib, "AbsoluteToLocal");
        fnSize_ = pe::FindFunction(lib, "GetLocalSize");
        fnL2A_ = pe::FindFunction(lib, "LocalToAbsolute");
        fnBack_ = pe::FindFunction(mapCls_, "BackInput");
        activeCls_=ue::FindClass("CommonActivatableWidget");
        fnActive_=activeCls_?pe::FindFunction(activeCls_,"IsActivated"):nullptr;
        fnDeactivate_=activeCls_?pe::FindFunction(activeCls_,"DeactivateWidget"):nullptr;
        fnOwner_=pe::FindFunction(widget,"GetOwningPlayer");
        bellCls_=ue::FindClass("BellInteractWidget");
        if ((subCls_ = ue::FindClass("MapSubsystem"))) {
            fnProj_ = pe::FindFunction(subCls_, "ProjectWorldLocationToMap");
            fnMapId_ = pe::FindFunction(subCls_, "GetCurrentMapID");
            fnMapIdAt_ = pe::FindFunction(subCls_, "GetMapIDByLocation");
        }
        ok_ = offLoc_ >= 0 && offSlot_ >= 0 && offParent_ >= 0 && offLayout_ >= 0 && fnGeom_ && fnA2L_ && fnSize_;
        return ok_;
    }

    // 全对象表扫一遍（几十毫秒），只在按键 / 探测时调用
    Result Locate(std::string* diag = nullptr) {
        Result r;
        if (!Resolve()) { r.msg = "地图类未加载（先打开一次地图）"; return r; }
        ue::UObject *lib = nullptr, *sub = nullptr;
        bellMenus_.clear();
        auto* player=M().Player();
        auto* level=player?ue::Get<ue::UObject*>(player+ue::Layout::ObjOuter):nullptr;
        auto* world=level?ue::Get<ue::UObject*>(level+ue::Layout::ObjOuter):nullptr;
        std::vector<ue::UObject*> maps, slots;
        ue::ForEach([&](int, ue::UObject* o) {
            auto* c = ue::ClassOf(o);
            if (ue::IsDefault(o)) { if (!lib && ue::ObjName(o) == "Default__SlateBlueprintLibrary") lib = o; return true; }
            if(bellCls_&&ue::IsA(o,bellCls_))bellMenus_.push_back(o);
            if (c == mapCls_) maps.push_back(o);
            else if (!sub && world && subCls_ && ue::IsA(o, subCls_) && ue::Get<ue::UObject*>(o+ue::Layout::ObjOuter)==world) sub = o;
            else if (slots.size() < 20000 && ue::IsA(o, slotCls_)) slots.push_back(o);
            return true;
        });
        lib_ = lib;
        if (!lib_) { r.msg = "界面库未加载"; return r; }
        for (auto* m : maps) if (Visible(m)&&ActiveOwner(m)) { r.map = m; break; }
        if (!r.map) { r.msg = "地图没有打开"; return r; }
        if (offLayer_ >= 0) r.layer = ue::Get<int32_t>(r.map + offLayer_);

        // 按所在画布分组，取图标最多的一块
        struct Pt { double u, v, x, y; };
        std::vector<std::pair<ue::UObject*, int>> groups;
        auto canvasOf = [&](ue::UObject* s) -> ue::UObject* {
            const double x = ue::Get<double>(s + offLoc_), y = ue::Get<double>(s + offLoc_ + 8);
            if ((x == 0 && y == 0) || !std::isfinite(x) || !std::isfinite(y)) return nullptr;
            auto* ps = ue::Get<ue::UObject*>(s + offSlot_);
            return ps && ue::IsA(ps, canvasSlotCls_) ? ue::Get<ue::UObject*>(ps + offParent_) : nullptr;
        };
        for (auto* s : slots) {
            auto* canvas = canvasOf(s);
            if (!canvas || !Inside(canvas, r.map)) continue;
            auto it = std::find_if(groups.begin(), groups.end(), [&](auto& g) { return g.first == canvas; });
            if (it == groups.end()) groups.push_back({canvas, 1}); else ++it->second;
        }
        if (groups.empty()) { r.msg = "地图上没有图标"; return r; }
        auto* canvas = std::max_element(groups.begin(), groups.end(), [](auto& a, auto& b) { return a.second < b.second; })->first;
        uint8_t geom[0x38];
        Geometry(canvas, geom);
        double size[2];
        Call2(fnSize_, geom, nullptr, size);
        // LayoutData：Offsets(float L,T,R,B) / Anchors(double Min xy, Max xy) / Alignment；点锚时画布内位置 = 锚点 × 画布尺寸 + Offsets.LT
        std::vector<Pt> pts;
        for (auto* s : slots) {
            if (canvasOf(s) != canvas) continue;
            auto* ps = ue::Get<ue::UObject*>(s + offSlot_);
            const double l = ue::Get<float>(ps + offLayout_), t = ue::Get<float>(ps + offLayout_ + 4);
            const double ax = ue::Get<double>(ps + offLayout_ + 0x10), ay = ue::Get<double>(ps + offLayout_ + 0x18);
            pts.push_back({ax * size[0] + l, ay * size[1] + t, ue::Get<double>(s + offLoc_), ue::Get<double>(s + offLoc_ + 8)});
        }
        // 图标画布位置只作粗估 / 诊断：缩放后部分图标的 LayoutData 会滞后于当前缩放，不能用于精确换算
        double cx[3] = {}, cy[3] = {};
        const bool haveFit = Fit(pts, cx, cy, &r.rms);
        if (pts.empty()) { r.msg = "地图上没有图标"; return r; }
        r.icons = static_cast<int>(pts.size());

        // 光标：游戏大地图画的是自己的十字准星 WB_Cursor（系统鼠标被锁在屏幕中间不动），取准星控件中心的屏幕坐标；
        // 取不到时才退回系统鼠标
        POINT pt{};
        GetCursorPos(&pt);
        double cursor[2] = {static_cast<double>(pt.x), static_cast<double>(pt.y)}, local[2];
        const char* cursorSrc = "系统鼠标";
        if (CursorAbs(r.map, cursor)) cursorSrc = "准星";
        Call2(fnA2L_, geom, cursor, local);
        if (haveFit) {
            r.x = cx[0] * local[0] + cx[1] * local[1] + cx[2];
            r.y = cy[0] * local[0] + cy[1] * local[1] + cy[2];
        } else { r.x = pts[0].x; r.y = pts[0].y; }

        // 精确换算：地图按锚点分段拉伸，世界 → 地图不是线性的，图标直接拟合误差几十米。
        // 用游戏自己的 ProjectWorldLocationToMap：先拟合"投影值 → 画布位置"（这一步是线性的：只差缩放 / 平移），
        // 再以图标拟合结果为初值，牛顿迭代求 Project(世界 XY) = 光标画布位置
        double pxRms = -1, pxErr = -1;
        std::string projDiag;
        uint64_t mapId = 0;
        if (sub && fnProj_ && fnMapId_) {
            Args id(fnMapId_);
            pe::Call(sub, fnMapId_, id.buf.data());
            mapId = *reinterpret_cast<uint64_t*>(id.At("ReturnValue"));
            auto project = [&](double x, double y, double out[2]) {
                Args a(fnProj_);
                const double v[3] = {x, y, 0};
                memcpy(a.At("Location"), v, 24);
                memcpy(a.At("MapID"), &mapId, 8);
                pe::Call(sub, fnProj_, a.buf.data());
                memcpy(out, a.At("ReturnValue"), 16);
            };
            // 当前所在区域 ≠ 正在看的分图时（大地图可切换区域），用当前 ID 投影全是 (0,0)：改用图标世界坐标所属的分图 ID（取多数）
            if (fnMapIdAt_ && !pts.empty()) {
                int hit = 0;
                for (auto& q : pts) {
                    double m[2];
                    project(q.x, q.y, m);
                    if (m[0] != 0 || m[1] != 0) ++hit;
                }
                if (hit * 2 < static_cast<int>(pts.size())) {
                    std::map<uint64_t, int> votes;
                    for (size_t i = 0; i < pts.size() && i < 40; ++i) {
                        Args at(fnMapIdAt_);
                        const double v[3] = {pts[i].x, pts[i].y, 0};
                        memcpy(at.At("Location"), v, 24);
                        pe::Call(sub, fnMapIdAt_, at.buf.data());
                        const uint64_t id = *reinterpret_cast<uint64_t*>(at.At("ReturnValue"));
                        if (id) ++votes[id];
                    }
                    auto best = std::max_element(votes.begin(), votes.end(), [](auto& a, auto& b) { return a.second < b.second; });
                    if (best != votes.end() && best->first != mapId) {
                        log::Info("地图传送：所在区域 {} 与查看的分图不同，改用 {}", ue::Name(static_cast<uint32_t>(mapId)), ue::Name(static_cast<uint32_t>(best->first)));
                        mapId = best->first;
                    }
                }
            }
            std::vector<Pt> proj, uvWorld;  // proj：u,v = 投影值，x,y = 画布位置（诊断）；uvWorld：u,v = 投影值，x,y = 世界坐标（初值）
            for (auto& q : pts) {
                double m[2];
                project(q.x, q.y, m);
                if (m[0] != 0 || m[1] != 0) { proj.push_back({m[0], m[1], q.u, q.v}); uvWorld.push_back({m[0], m[1], q.x, q.y}); }  // (0,0) = 别的分图
            }
            if (diag) {
                projDiag = R"(,"proj":[)";
                for (size_t i = 0; i < proj.size() && i < 60; ++i) projDiag += std::format(R"({}[{:.4f},{:.4f}])", i ? "," : "", proj[i].u, proj[i].v);
                projDiag += "]";
            }
            // 画布位置 = 投影 UV × 画布尺寸（实测拟合系数 1920.05 / -0.01，无平移）；图标拟合只算诊断残差
            { double fx[3], fy[3]; auto copy = proj; if (!Fit(copy, fx, fy, &pxRms, 2)) pxRms = -1; }
            const double ax[3] = {size[0], 0, 0}, ay[3] = {0, size[1], 0};
            if (mapId && size[0] > 1 && size[1] > 1) {
                auto f = [&](double x, double y, double out[2]) {
                    double m[2];
                    project(x, y, m);
                    out[0] = ax[0] * m[0] + ax[1] * m[1] + ax[2] - local[0];
                    out[1] = ay[0] * m[0] + ay[1] * m[1] + ay[2] - local[1];
                };
                // 稳健反解：投影是分段拉伸的，纯牛顿一步跨出去会发散。中心差分求雅可比 + 回溯线搜索（误差不降就减半步长），
                // 从两个初值出发取最好：图标拟合估计、画布上离准星最近的图标的世界坐标
                auto err = [&](double x, double y, double e[2]) {
                    double m[2];
                    project(x, y, m);
                    if (m[0] == 0 && m[1] == 0) return 1e18;  // 出了本图
                    e[0] = ax[0] * m[0] + ax[1] * m[1] + ax[2] - local[0];
                    e[1] = ay[0] * m[0] + ay[1] * m[1] + ay[2] - local[1];
                    return std::hypot(e[0], e[1]);
                };
                // 起点越多越稳：大地图分段拉伸的接缝多，单个起点常被接缝挡住（第三张图实测：相邻点都能传，个别点残差几百像素）。
                // 按投影 UV 离准星最近排序，取最近 8 个图标的世界坐标，外加最近 6 个图标的局部仿射插值、全图粗拟合
                std::vector<std::pair<double, double>> starts;
                if (!uvWorld.empty()) {
                    const double tu = local[0] / size[0], tv = local[1] / size[1];
                    auto near_ = uvWorld;
                    std::sort(near_.begin(), near_.end(), [&](auto& a, auto& b) {
                        return std::hypot(a.u - tu, a.v - tv) < std::hypot(b.u - tu, b.v - tv); });
                    if (near_.size() >= 3) {
                        std::vector<Pt> loc(near_.begin(), near_.begin() + std::min<size_t>(6, near_.size()));
                        double lx[3], ly[3], lr;
                        if (Fit(loc, lx, ly, &lr, 1e9)) starts.push_back({lx[0] * tu + lx[1] * tv + lx[2], ly[0] * tu + ly[1] * tv + ly[2]});
                    }
                    for (size_t i = 0; i < near_.size() && i < 8; ++i) starts.push_back({near_[i].x, near_[i].y});
                }
                starts.push_back({r.x, r.y});
                double bestX = r.x, bestY = r.y, best = 1e18;
                for (auto [x, y] : starts) {
                    double e[2], cur = err(x, y, e);
                    for (int i = 0; i < 60 && cur > 0.3 && cur < 1e17; ++i) {
                        constexpr double h = 100;
                        double exp_[2], exm[2], eyp[2], eym[2];
                        if (err(x + h, y, exp_) > 1e17 || err(x - h, y, exm) > 1e17 || err(x, y + h, eyp) > 1e17 || err(x, y - h, eym) > 1e17) break;
                        const double j00 = (exp_[0] - exm[0]) / (2 * h), j10 = (exp_[1] - exm[1]) / (2 * h);
                        const double j01 = (eyp[0] - eym[0]) / (2 * h), j11 = (eyp[1] - eym[1]) / (2 * h);
                        const double det = j00 * j11 - j01 * j10;
                        if (std::abs(det) < 1e-14) break;
                        double dx = -(j11 * e[0] - j01 * e[1]) / det, dy = -(-j10 * e[0] + j00 * e[1]) / det;
                        const double len = std::hypot(dx, dy);
                        if (len > 30000) { dx *= 30000 / len; dy *= 30000 / len; }
                        bool moved = false;
                        for (double lambda = 1; lambda > 1.0 / 128; lambda /= 2) {
                            double en[2];
                            const double c = err(x + lambda * dx, y + lambda * dy, en);
                            if (c < cur) { x += lambda * dx; y += lambda * dy; cur = c; e[0] = en[0]; e[1] = en[1]; moved = true; break; }
                        }
                        if (!moved) break;
                    }
                    if (cur < best) { best = cur; bestX = x; bestY = y; }
                    if (best <= 0.3) break;
                }
                double x = bestX, y = bestY, e[2];
                f(x, y, e);
                pxErr = std::hypot(e[0], e[1]);
                double m[2];
                project(x, y, m);
                if (pxErr < 2 && std::isfinite(x) && std::isfinite(y) && (m[0] != 0 || m[1] != 0)) { r.x = x; r.y = y; r.ok = true; }
            }
            r.icons = static_cast<int>(proj.size());
        }
        // 只接受精确换算：图标直接拟合误差几十米，可能落到地图外
        if (!r.ok) r.msg = mapId ? "无法传送（光标处不在当前地图内）" : "无法传送（地图换算失败）";
        if (diag) {
            *diag = std::format(R"("maps":{},"canvases":{},"icons":{},"rms":{:.0f},"canvas_size":[{:.1f},{:.1f}],"cursor":[{:.0f},{:.0f}],"cursor_src":"{}","local":[{:.1f},{:.1f}],"world":[{:.0f},{:.0f}],"layer":{},"fx":[{:.4f},{:.4f},{:.0f}],"fy":[{:.4f},{:.4f},{:.0f}],"map_id":"{}","proj_rms_px":{:.2f},"newton_err_px":{:.3f},"pts":[)",
                                maps.size(), groups.size(), r.icons, r.rms, size[0], size[1], cursor[0], cursor[1], cursorSrc, local[0], local[1], r.x, r.y, r.layer,
                                cx[0], cx[1], cx[2], cy[0], cy[1], cy[2], ue::Name(static_cast<uint32_t>(mapId)), pxRms, pxErr);
            for (size_t i = 0; i < pts.size() && i < 60; ++i)
                *diag += std::format(R"({}[{:.1f},{:.1f},{:.0f},{:.0f}])", i ? "," : "", pts[i].u, pts[i].v, pts[i].x, pts[i].y);
            *diag += "]" + projDiag;
        }
        return r;
    }

    bool Close(ue::UObject* map) {
        auto* root=ActiveOwner(map);
        if(!root||!fnDeactivate_||!fnOwner_)return false;
        Args args(fnOwner_);pe::Call(map,fnOwner_,args.buf.data());
        auto* pc=ue::Get<ue::UObject*>(args.At("ReturnValue"));
        if(!Live(pc))return false;
        // BackInput may merely dismiss a popup or switch tabs. Close the
        // owning CommonUI screen so its native input/camera cleanup runs.
        pe::Call(root,fnDeactivate_,nullptr);
        if(Active(root))return false;
        // A bell's teleport sub-screen can reactivate the underlying bell
        // menu. End that same local player's interaction once before moving.
        for(auto* menu:bellMenus_) {
            if(!Live(menu)||!Active(menu))continue;
            args.buf.assign(args.buf.size(),0);pe::Call(menu,fnOwner_,args.buf.data());
            if(ue::Get<ue::UObject*>(args.At("ReturnValue"))!=pc)continue;
            if(auto* cancel=pe::FindFunction(ue::ClassOf(menu),"CancelLatentActions"))pe::Call(menu,cancel,nullptr);
            pe::Call(menu,fnDeactivate_,nullptr);
            if(Active(menu))return false;
        }
        return !Active(root);
    }

private:
    bool Active(ue::UObject* w) {
        if(!Live(w)||!activeCls_||!fnActive_||!ue::IsA(w,activeCls_))return false;
        uint8_t args[8]{};pe::Call(w,fnActive_,args);return args[0]!=0;
    }
    ue::UObject* ActiveOwner(ue::UObject* w) {
        for(int i=0;w&&i<16;++i) {
            if(!Live(w))return nullptr;
            if(activeCls_&&ue::IsA(w,activeCls_))return Active(w)?w:nullptr;
            w=ue::Get<ue::UObject*>(w+ue::Layout::ObjOuter);
        }
        return nullptr;
    }
    // 准星（WB_Cursor）中心的屏幕绝对坐标
    bool CursorAbs(ue::UObject* map, double out[2]) {
        auto* cw = offCursor_ >= 0 ? ue::Get<ue::UObject*>(map + offCursor_) : nullptr;
        if (!cw || !fnL2A_) return false;
        uint8_t cg[0x38];
        Geometry(cw, cg);
        double cs[2], half[2], abs[2];
        Call2(fnSize_, cg, nullptr, cs);
        if (!(cs[0] > 1 && cs[1] > 1)) return false;
        half[0] = cs[0] / 2; half[1] = cs[1] / 2;
        Call2(fnL2A_, cg, half, abs);
        if (!std::isfinite(abs[0]) || !std::isfinite(abs[1])) return false;
        out[0] = abs[0]; out[1] = abs[1];
        return true;
    }
    bool Visible(ue::UObject* w) {
        if (fnVisible_) { uint8_t ret[8] = {}; pe::Call(w, fnVisible_, ret); if (!ret[0]) return false; }
        uint8_t g[0x38];
        Geometry(w, g);
        double s[2];
        Call2(fnSize_, g, nullptr, s);
        return s[0] > 1 && s[1] > 1;
    }
    // 画布要属于这个地图控件：沿 Outer 链（UMG 子控件的 Outer 是 WidgetTree，再上是用户控件）
    static bool Inside(ue::UObject* w, ue::UObject* map) {
        for (int i = 0; w && i < 8; ++i) { if (w == map) return true; w = ue::Get<ue::UObject*>(w + ue::Layout::ObjOuter); }
        return false;
    }
    void Geometry(ue::UObject* w, uint8_t out[0x38]) {
        Args a(fnGeom_);
        pe::Call(w, fnGeom_, a.buf.data());
        memcpy(out, a.At("ReturnValue"), 0x38);
    }
    // SlateBlueprintLibrary 的 (Geometry[, Vector2D]) -> Vector2D
    void Call2(ue::UObject* fn, const uint8_t geom[0x38], const double in[2], double out[2]) {
        Args a(fn);
        memcpy(a.At("Geometry"), geom, 0x38);
        if (in) memcpy(a.At(fn == fnA2L_ ? "AbsoluteCoordinate" : "LocalCoordinate"), in, 16);
        pe::Call(lib_, fn, a.buf.data());
        memcpy(out, a.At("ReturnValue"), 16);
    }
    // 最小二乘仿射 x = a·u + b·v + c（y 同），残差大于 3 倍中位数的点剔除后再拟合一次
    template <class P>
    static bool Fit(std::vector<P>& pts, double cx[3], double cy[3], double* rms, double floor = 100) {
        auto solve = [&](const std::vector<P>& p) {
            if (p.size() < 3) return false;
            double m[3][3] = {}, bx[3] = {}, by[3] = {};
            for (auto& q : p) {
                const double r[3] = {q.u, q.v, 1};
                for (int i = 0; i < 3; ++i) {
                    for (int j = 0; j < 3; ++j) m[i][j] += r[i] * r[j];
                    bx[i] += r[i] * q.x;
                    by[i] += r[i] * q.y;
                }
            }
            const double det = m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
                               m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
            if (std::abs(det) < 1e-6) return false;
            auto cramer = [&](const double b[3], double out[3]) {
                for (int k = 0; k < 3; ++k) {
                    double t[3][3];
                    memcpy(t, m, sizeof t);
                    for (int i = 0; i < 3; ++i) t[i][k] = b[i];
                    out[k] = (t[0][0] * (t[1][1] * t[2][2] - t[1][2] * t[2][1]) - t[0][1] * (t[1][0] * t[2][2] - t[1][2] * t[2][0]) +
                              t[0][2] * (t[1][0] * t[2][1] - t[1][1] * t[2][0])) / det;
                }
            };
            cramer(bx, cx);
            cramer(by, cy);
            return true;
        };
        auto err = [&](const P& q) {
            const double dx = cx[0] * q.u + cx[1] * q.v + cx[2] - q.x, dy = cy[0] * q.u + cy[1] * q.v + cy[2] - q.y;
            return std::sqrt(dx * dx + dy * dy);
        };
        if (!solve(pts)) return false;
        for (int round = 0; round < 5; ++round) {
            std::vector<double> e;
            for (auto& q : pts) e.push_back(err(q));
            auto sorted = e;
            std::nth_element(sorted.begin(), sorted.begin() + sorted.size() / 2, sorted.end());
            const double lim = std::max(3 * sorted[sorted.size() / 2], floor);
            std::vector<P> keep;
            for (size_t i = 0; i < pts.size(); ++i) if (e[i] <= lim) keep.push_back(pts[i]);
            if (keep.size() == pts.size() || !solve(keep)) break;
            pts = keep;
        }
        double s = 0;
        for (auto& q : pts) s += err(q) * err(q);
        *rms = std::sqrt(s / pts.size());
        return true;
    }

    bool ok_ = false;
    ue::UObject *mapCls_ = nullptr, *slotCls_ = nullptr, *canvasSlotCls_ = nullptr, *subCls_ = nullptr, *lib_ = nullptr;
    ue::UObject *fnProj_ = nullptr, *fnMapId_ = nullptr, *fnMapIdAt_ = nullptr;
    ue::UObject *fnGeom_ = nullptr, *fnVisible_ = nullptr, *fnA2L_ = nullptr, *fnL2A_ = nullptr, *fnSize_ = nullptr, *fnBack_ = nullptr;
    ue::UObject *activeCls_=nullptr,*bellCls_=nullptr,*fnActive_=nullptr,*fnDeactivate_=nullptr,*fnOwner_=nullptr;
    std::vector<ue::UObject*> bellMenus_;
    int offLoc_ = -1, offSlot_ = -1, offParent_ = -1, offLayout_ = -1, offLayer_ = -1, offCursor_ = -1;
};

MapLocator& Locator() {
    static MapLocator l;
    return l;
}

// 落点：在 XY 处自上而下逐层射线（Pawn 碰撞配置，能看到空气墙），收集朝上的可站表面；
// 跳过阻挡体积（空气墙顶面）、头顶不足 2 米的面；按地图当前层选面，没有层信息时选最高面。
// 跨图安全（影塔 7c2668e 实测：跨图后第三层落点查询在游戏函数里连续 51 次访问冲突，Codex 2026-11-01）：
//   - 地图子系统是每个世界一份（WorldSubsystem）：只用 Outer 就是玩家所在世界的那份，旧世界正在拆 / 新世界还没建好时不查层；
//   - 射线与查层都在异常保护里调；某个世界里出过一次异常，这个世界就停用落点查询（换图后恢复），绝不逐帧重试同一个原生调用；
//   - 原生目的地加载遮罩还在时不查（LoadingActive）。
class Ground {
public:
    struct Surface { double z; int layer; };
    static constexpr const char* kFaultTag = "原生异常：";  // 传送失败消息的前缀：调用方据此停机而不是换个点再试（影塔 tower_travel.cpp）

    bool Resolve() {
        if (ok_) return true;
        auto* ksl = ue::FindClass("KismetSystemLibrary");
        if (!ksl) return false;
        fnTrace_ = pe::FindFunction(ksl, "LineTraceSingleByProfile");
        if (auto* ms = ue::FindClass("MapSubsystem")) { fnLayer_ = pe::FindFunction(ms, "GetLayerByLocation"); fnCurLayer_ = pe::FindFunction(ms, "GetCurrentMapLayer"); }
        volumeCls_ = ue::FindClass("Volume");
        mapSubCls_ = ue::FindClass("MapSubsystem");
        if (auto* dl = ue::FindClass("PBZDestinationLoadingSubsystem")) fnLoading_ = pe::FindFunction(dl, "IsDestinationLoadingActive");
        profile_ = inv::ToFName("Pawn");
        ok_ = fnTrace_ && profile_;
        return ok_;
    }

    // zc：扫描中心高度。默认 0（地图传送：地表在 ±3 千米内）；已知目标高度时围绕它扫——天空擂台在一千万厘米以上
    // （魔渊天堡 z≈10008830、荒行子锯轮 z≈10037551，Codex 2026-10-31 实测原先固定 ±300000 扫不到）
    // layers：是否查每个面的地图层（只有按地图层选面时要；已知目标高度的点传送不查，少一个跨图时的原生调用）
    std::vector<Surface> Scan(ue::UObject* ctx, double x, double y, double zc = 0, bool layers = true) {
        std::vector<Surface> out;
        if (!lib_ || !Live(lib_)) lib_ = Cdo("Default__KismetSystemLibrary");
        if (!lib_ || Faulted(ctx)) return out;
        const double bottom = zc - 300000;
        double top = zc + 300000;
        hits = 0;
        hitInfo.clear();
        // 阻挡体积 / 射线起点就在物体内部（起始穿透：命中点 = 起点，原地反复命中）：把该物体加入忽略表从同一高度重打，
        // 否则每次只下移 10 厘米，几十次都穿不过一个大体积
        std::vector<ue::UObject*> ignore;
        for (int i = 0; i < 64 && top > bottom; ++i) {
            double p[3], n[3];
            ue::UObject* actor = nullptr;
            bool inside = false;
            if (!Trace(ctx, {x, y, top}, {x, y, bottom}, p, n, &actor, &inside, &ignore)) break;
            if (Faulted(ctx)) { out.clear(); return out; }
            ++hits;
            const bool volume = actor && volumeCls_ && ue::IsA(actor, volumeCls_);
            if (hitInfo.size() < 300)
                hitInfo += std::format("{}{}@{:.0f}{}{}", hitInfo.empty() ? "" : " ", actor ? ue::ClassName(actor) : "?", p[2], inside ? "(内部)" : "", volume ? "(体积)" : "");
            if ((inside || volume) && actor && ignore.size() < 32 && std::find(ignore.begin(), ignore.end(), actor) == ignore.end()) {
                ignore.push_back(actor);
                continue;
            }
            top = p[2] - (inside ? 100 : 10);
            if (hitInfo.size() < 300) hitInfo += std::format("n{:.2f}", n[2]);
            if (inside || volume || n[2] < 0.7) continue;  // 墙面 / 陡坡
            // 头顶净空不在这里判：树枝 / 植被常挡住向上射线；交给 K2_TeleportTo 的穿插检测（被挡会修正或失败，失败再试下一个面）
            out.push_back({p[2], layers ? Layer(ctx, x, y, p[2] + 50) : -1});
            if (Faulted(ctx)) { out.clear(); return out; }
        }
        return out;
    }
    int hits = 0;         // 上次扫描的射线命中数（诊断）
    std::string hitInfo;  // 上次扫描命中的物体类 @ 高度（诊断）
    // 地图子系统随世界走（每个世界一份）：只认 Outer 就是 ctx 所在世界、仍存活的那份；跨图期间找不到就不查层（-1）。
    // 找不到时每秒最多全表扫一次
    ue::UObject* Sub(ue::UObject* ctx) {
        auto* world = WorldPtr(ctx);
        if (!world || !mapSubCls_) return nullptr;
        if (mapSub_ && subWorld_ == world && Live(mapSub_) && ue::Get<ue::UObject*>(mapSub_ + ue::Layout::ObjOuter) == world) return mapSub_;
        const auto now = GetTickCount64();
        if (subWorld_ == world && !mapSub_ && now - searchedAt_ < 1000) return nullptr;
        mapSub_ = nullptr; subWorld_ = world; searchedAt_ = now;
        ue::ForEach([&](int, ue::UObject* o) {
            if (ue::IsDefault(o) || !ue::IsA(o, mapSubCls_) || ue::Get<ue::UObject*>(o + ue::Layout::ObjOuter) != world || !Live(o)) return true;
            mapSub_ = o;
            return false;
        });
        return mapSub_;
    }
    int CurrentLayer(ue::UObject* ctx) {
        auto* sub = Faulted(ctx) || !fnCurLayer_ ? nullptr : Sub(ctx);
        if (!sub) return -1;
        uint8_t ret[16] = {};
        if (!diag::Guarded("move.map_layer", [&] { pe::Call(sub, fnCurLayer_, ret); })) { Fault(ctx, "GetCurrentMapLayer"); return -1; }
        return *reinterpret_cast<int32_t*>(ret);
    }
    // 本世界的落点查询出过原生异常：停用到换图（玩家换了世界自动恢复）
    bool Faulted(ue::UObject* ctx) const { return faultWorld_ && WorldPtr(ctx) == faultWorld_; }
    int faults = 0;  // 累计（诊断）
    void Fault(ue::UObject* ctx, const char* what) {
        ++faults;
        if (faultWorld_ == WorldPtr(ctx)) return;
        faultWorld_ = WorldPtr(ctx);
        log::Info("落点查询：{} 在当前地图里发生原生异常，本地图停用落点查询（换图后恢复，不逐帧重试）", what);
    }
    // 原生目的地加载遮罩是否还在（跨图 / 读档）。查不到接口返回 false（不挡修改器自己的传送；影塔另有入场门）
    bool LoadingActive(ue::UObject* ctx) {
        if (!ctx || !fnLoading_) return false;
        if (!loadingCdo_ || !Live(loadingCdo_)) loadingCdo_ = Cdo("Default__PBZDestinationLoadingSubsystem");
        if (!loadingCdo_) return false;
        Args a(fnLoading_);
        auto* wc = a.At("WorldContext");
        auto* rv = a.At("ReturnValue");
        if (!wc || !rv) return false;
        memcpy(wc, &ctx, 8);
        if (!diag::Guarded("move.loading", [&] { pe::Call(loadingCdo_, fnLoading_, a.buf.data()); })) return true;  // 查询本身异常：当作还在加载
        return *rv != 0;
    }

private:
    struct V { double x, y, z; };
    bool Trace(ue::UObject* ctx, V a, V b, double p[3], double n[3], ue::UObject** actor, bool* inside, std::vector<ue::UObject*>* ignore) {
        Args t(fnTrace_);
        if (ignore && !ignore->empty()) {  // ActorsToIgnore：TArray {Data, Num, Max}，指向本地数组（调用期间有效）
            auto* arr = t.At("ActorsToIgnore");
            auto* data = ignore->data();
            const int32_t num = static_cast<int32_t>(ignore->size());
            memcpy(arr, &data, 8); memcpy(arr + 8, &num, 4); memcpy(arr + 12, &num, 4);
        }
        memcpy(t.At("WorldContextObject"), &ctx, 8);
        memcpy(t.At("Start"), &a, 24);
        memcpy(t.At("End"), &b, 24);
        memcpy(t.At("ProfileName"), &profile_, 8);
        *t.At("bIgnoreSelf") = 1;
        if (!diag::Guarded("move.ground_trace", [&] { pe::Call(lib_, fnTrace_, t.buf.data()); })) { Fault(ctx, "LineTraceSingleByProfile"); return false; }
        if (!*t.At("ReturnValue")) return false;
        auto* hit = t.At("OutHit");  // HitResult：ImpactPoint +0x28、ImpactNormal +0x58、Component 弱引用 +0xD8
        memcpy(p, hit + 0x28, 24);
        memcpy(n, hit + 0x58, 24);
        if (inside) *inside = (hit[0xAD] & 2) != 0;  // bStartPenetrating
        if (actor) {
            auto* comp = ue::Obj(*reinterpret_cast<int32_t*>(hit + 0xD8));
            *actor = comp ? ue::Get<ue::UObject*>(comp + ue::Layout::ObjOuter) : nullptr;
        }
        return std::isfinite(p[2]);
    }
    int Layer(ue::UObject* ctx, double x, double y, double z) {
        auto* sub = fnLayer_ ? Sub(ctx) : nullptr;
        if (!sub) return -1;
        Args a(fnLayer_);
        const double v[3] = {x, y, z};
        memcpy(a.At("Location"), v, 24);
        if (!diag::Guarded("move.map_layer", [&] { pe::Call(sub, fnLayer_, a.buf.data()); })) { Fault(ctx, "GetLayerByLocation"); return -1; }
        return *reinterpret_cast<int32_t*>(a.At("ReturnValue"));
    }
    // Actor → Level → World（玩家所在的持久世界）
    static ue::UObject* WorldPtr(ue::UObject* actor) {
        if (!actor) return nullptr;
        auto* level = ue::Get<ue::UObject*>(actor + ue::Layout::ObjOuter);
        return level ? ue::Get<ue::UObject*>(level + ue::Layout::ObjOuter) : nullptr;
    }
    static ue::UObject* Cdo(const char* name) {
        ue::UObject* found = nullptr;
        ue::ForEach([&](int, ue::UObject* o) {
            if (ue::IsDefault(o) && ue::ObjName(o) == name) { found = o; return false; }
            return true;
        });
        return found;
    }

    bool ok_ = false;
    ue::UObject *fnTrace_ = nullptr, *fnLayer_ = nullptr, *fnCurLayer_ = nullptr, *volumeCls_ = nullptr, *lib_ = nullptr, *mapSub_ = nullptr;
    ue::UObject *mapSubCls_ = nullptr, *subWorld_ = nullptr, *faultWorld_ = nullptr, *fnLoading_ = nullptr, *loadingCdo_ = nullptr;
    ULONGLONG searchedAt_ = 0;
    uint64_t profile_ = 0;
};

Ground& G() {
    static Ground g;
    return g;
}

// 选面：优先与地图当前层一致的面；层未知或没有一致的面时取最高面
// 候选落点高度，按优先级排：与地图当前层一致的在前，同组内从高到低
std::vector<double> OrderSurfaces(const std::vector<Ground::Surface>& s, int layer) {
    std::vector<Ground::Surface> v = s;
    std::stable_sort(v.begin(), v.end(), [&](auto& a, auto& b) {
        const bool am = layer >= 0 && a.layer == layer, bm = layer >= 0 && b.layer == layer;
        return am != bm ? am : a.z > b.z;
    });
    std::vector<double> z;
    for (auto& c : v) z.push_back(c.z);
    return z;
}
// 依次试各候选面（被挡 / 穿插时 K2_TeleportTo 返回失败），成功返回落地高度
bool TeleportOnto(ue::UObject* p, double x, double y, const std::vector<double>& zs, double yaw, double lift, double* landed) {
    const double face[3] = {0, yaw, 0};
    for (double z : zs) {
        const double dest[3] = {x, y, z + lift};
        if (M().Teleport(p, dest, face)) { *landed = z; return true; }
    }
    return false;
}

// 已知目标高度（钟的落点 / 影塔场地）：候选面按与目标高度的差排序，只取 15 米以内的
std::vector<double> OrderNear(const std::vector<Ground::Surface>& s, double target) {
    std::vector<Ground::Surface> v;
    for (auto& c : s) if (std::abs(c.z - target) < 1500) v.push_back(c);  // 15 米：影塔落点取战斗区域中心，可能比地面高几米
    std::stable_sort(v.begin(), v.end(), [&](auto& a, auto& b) { return std::abs(a.z - target) < std::abs(b.z - target); });
    std::vector<double> z;
    for (auto& c : v) z.push_back(c.z);
    return z;
}

#include "map_teleport.h"

}  // namespace

bool PointTeleport(double x, double y, double z, double yaw, std::string* msg) {
    if (!g_mapTeleport) { *msg = "传送接口不可用"; return false; }
    bool ok = false;
    if (diag::Guarded("move.point_tp", [&] { ok = g_mapTeleport->GoToPoint(x, y, z, yaw, msg); })) return ok;
    // 传送途中原生调用异常（落点查询之外，例如瞬移本身）：取消这次传送、放开悬停，本地图停用落点查询
    g_mapTeleport->Abort("点传送调用");
    *msg = std::string(Ground::kFaultTag) + "传送调用异常，已取消";
    return false;
}
bool TeleportBusy() { return g_mapTeleport && g_mapTeleport->Busy(); }

std::string BellLevel(const std::string& id) {
    std::string why;
    if (!M().Resolve(&why)) return {};
    auto* bells = FindSubsystem("BellManager");
    auto* fn = bells ? pe::FindFunction(ue::ClassOf(bells), "GetLevelNameFromBellID") : nullptr;
    const uint64_t name = inv::ToFName(id);
    if (!fn || !name) return {};
    Args a(fn);
    memcpy(a.At("BellID"), &name, 8);
    pe::Call(bells, fn, a.buf.data());
    const auto level = ue::Name(static_cast<uint32_t>(*reinterpret_cast<uint64_t*>(a.At("ReturnValue"))));
    return level == "None" ? std::string() : level;
}

bool PlayerWhere(double out[4], std::string* level) {
    std::string why;
    if (!M().Resolve(&why)) return false;
    auto* p = M().Player();
    double loc[3], rot[3];
    if (!p || !M().Get(p, M().fnLoc_, loc) || !M().Get(p, M().fnRot_, rot)) return false;
    out[0] = loc[0]; out[1] = loc[1]; out[2] = loc[2]; out[3] = rot[1];
    if (level) *level = Mover::WorldOf(p);
    return true;
}

// 研究用：地图打开时导出换算过程（图标拟合、光标画布位置、光标处可站表面），用来核对地图传送
std::string MapProbe() {
    std::string diag;
    auto r = Locator().Locate(&diag);
    std::string out = std::format(R"({{"op":"mapprobe","ok":{},"msg":"{}")", r.ok ? "true" : "false", r.msg);
    if (!diag.empty()) out += "," + diag;
    std::string why;
    if (r.ok && M().Resolve(&why) && G().Resolve())
        if (auto* p = M().Player()) {
            double at[3];
            M().Get(p, M().fnLoc_, at);
            out += std::format(R"(,"player":[{:.0f},{:.0f},{:.0f}],"cur_layer":{},"surfaces":[)", at[0], at[1], at[2], G().CurrentLayer(p));
            bool first = true;
            for (auto& s : G().Scan(p, r.x, r.y)) { out += std::format(R"({}[{:.0f},{}])", first ? "" : ",", s.z, s.layer); first = false; }
            out += "]";
        }
    return out + "}";
}

std::string PositionCommand(const std::string& cmd, int id, const std::string& name) {
    std::string why;
    if (!M().Resolve(&why)) return std::format(R"({{"op":"pos","ok":false,"msg":"{}","list":[]}})", why);
    return M().Command(cmd, id, name);
}

void RegisterMoveFeatures() {
    auto& F = Features();
    F.push_back(std::make_unique<MoveAction>("world.tp_enemy", MoveAction::ToEnemy));
    F.push_back(std::make_unique<AirJump>());
    F.push_back(std::make_unique<Noclip>());
    F.push_back(std::make_unique<MapTeleport>());
}

}  // namespace pbz
