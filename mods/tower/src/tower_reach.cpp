#include "tower_reach.h"

#include <algorithm>
#include <array>
#include <vector>
#include <cmath>
#include <cstring>
#include <format>

#include "../../../src/core/host/log.h"
#include "../../../src/core/sdk/pbz_module.h"
#include "tower_perf.h"
#include "tower_widgets.h"

namespace towerreach {
namespace {
namespace ue = pbz::ue;
namespace pe = pbz::pe;
using ue::UObject;

struct Buf {
    UObject* fn = nullptr;
    std::array<uint8_t, 0x600> b{};
    uint8_t* At(const char* k) { const int o = fn ? ue::PropOffset(fn, k) : -1; return o >= 0 && o < 0x580 ? b.data() + o : nullptr; }
};

struct Api {
    bool resolved = false;
    uint64_t nextResolve=0;
    UObject *ksl = nullptr, *line = nullptr, *nav = nullptr, *find = nullptr, *pathCls = nullptr, *isValid = nullptr, *isPartial = nullptr;
    int impact = 0x28;
    int normal = -1;
    uint8_t types[2] = {0, 1};  // EObjectTypeQuery：WorldStatic / WorldDynamic（运行时按 ConvertToObjectType 换算）
    int timeOff = 4, compOff = 0xD8;
    int penetrateOff = -1;
    uint8_t penetrateMask = 0;
    void Resolve() {
        if (resolved || GetTickCount64()<nextResolve) return;
        nextResolve=GetTickCount64()+2000;
        if (auto* c = towerperf::ClassNamed("KismetSystemLibrary")) { ksl = towerwidgets::Default(c); line = pe::FindFunction(c, "LineTraceSingleForObjects"); }
        if (auto* c = towerperf::ClassNamed("NavigationSystemV1")) { nav = towerwidgets::Default(c); find = pe::FindFunction(c, "FindPathToLocationSynchronously"); }
        if ((pathCls = towerperf::ClassNamed("NavigationPath"))) { isValid = pe::FindFunction(pathCls, "IsValid"); isPartial = pe::FindFunction(pathCls, "IsPartial"); }
        if (auto* hit = towerperf::ClassNamed("HitResult")) {
            if (const int o = ue::PropOffset(hit, "ImpactPoint"); o >= 0) impact = o;
            normal = ue::PropOffset(hit, "ImpactNormal");
            if (const int o = ue::PropOffset(hit, "Time"); o >= 0) timeOff = o;
            if (const int o = ue::PropOffset(hit, "Component"); o >= 0) compOff = o;
            // Match the SDK BoolProperty layout, not an assumed Time==0 flag.
            auto* f=ue::Get<UObject*>(hit+ue::Layout::StructChildProps);
            for(int n=0; f && n<128; ++n,f=ue::Get<UObject*>(f+ue::Layout::FieldNext)) {
                if(ue::Name(ue::Get<uint32_t>(f+ue::Layout::FieldName))!="bStartPenetrating")continue;
                const int o=ue::Get<int32_t>(f+ue::Layout::PropOffset,-1)+ue::Get<uint8_t>(f+0x71);
                const auto mask=ue::Get<uint8_t>(f+0x72);
                if(o>=0 && o<256 && mask && !(mask&(mask-1))){penetrateOff=o;penetrateMask=mask;}
                break;
            }
        }
        // 物体通道：ECollisionChannel WorldStatic 0 / WorldDynamic 1 → EObjectTypeQuery（与 Core enemy_spawn 同一转换）
        if (auto* c = towerperf::ClassNamed("PBZBlueprintFunctionLibrary"))
            if (auto* conv = pe::FindFunction(c, "ConvertToObjectType")) {
                for (int i = 0; i < 2; ++i) {
                    Buf a{conv};
                    if (!a.At("CollisionChannel") || !a.At("ReturnValue")) break;
                    *a.At("CollisionChannel") = static_cast<uint8_t>(i);
                    pe::Call(towerwidgets::Default(c), conv, a.b.data());
                    if (*a.At("ReturnValue") < 32) types[i] = *a.At("ReturnValue");
                }
            }
        pbz::log::Info("影塔可达探测：射线 {}，导航 {}（路径判定 {}/{}），物体通道 {}/{}，初始穿透位 offset={} mask={}", !!line, !!find, !!isValid, !!isPartial, types[0], types[1],penetrateOff,penetrateMask);
        resolved=line && ksl && find && nav && isValid && isPartial;
    }
    struct Hit {
        bool available=false,ok=false,overlapKnown=false,overlap=false;
        double at[3] = {}; float time = -1; std::string comp; UObject* compObj = nullptr;
        double normalZ = 0;
        bool Surface()const{return ok && overlapKnown && !overlap && time>0.0001f;}
    };
    // 返回是否命中；hit = 命中点
    bool Trace(UObject* ctx, const double s[3], const double e[3], double hit[3]) {
        const auto h = TraceTypes(ctx, s, e, types, 2);
        if (h.Surface()) std::memcpy(hit, h.at, 24);
        return h.Surface();
    }
    Hit TraceTypes(UObject* ctx, const double s[3], const double e[3], const uint8_t* list, int count, const std::vector<UObject*>* extraIgnore = nullptr) {
        Hit out;
        if (!line || !ksl) return out;
        Buf a{line};
        struct Arr { const void* data; int32_t num, max; } objs{list, count, count};
        std::vector<UObject*> ignored{ctx};
        if (extraIgnore) ignored.insert(ignored.end(), extraIgnore->begin(), extraIgnore->end());
        Arr ignore{ignored.data(), static_cast<int32_t>(ignored.size()), static_cast<int32_t>(ignored.size())};
        if (!a.At("WorldContextObject") || !a.At("Start") || !a.At("End") || !a.At("ObjectTypes") || !a.At("ActorsToIgnore") || !a.At("bIgnoreSelf") || !a.At("OutHit") || !a.At("ReturnValue")) return out;
        std::memcpy(a.At("WorldContextObject"), &ctx, 8);
        std::memcpy(a.At("Start"), s, 24); std::memcpy(a.At("End"), e, 24);
        std::memcpy(a.At("ObjectTypes"), &objs, sizeof objs);
        // Reflected library calls execute on a CDO. Explicitly exclude the
        // WorldContext pawn; bIgnoreSelf alone did not exclude its geometry.
        std::memcpy(a.At("ActorsToIgnore"),&ignore,sizeof ignore);
        *a.At("bIgnoreSelf") = 1;
        out.available=true;
        pe::Call(ksl, line, a.b.data());
        out.ok = *a.At("ReturnValue") != 0;
        if (out.ok) {
            const uint8_t* h = a.At("OutHit");
            std::memcpy(out.at, h + impact, 24);
            if (normal >= 0 && normal + 24 <= 256) std::memcpy(&out.normalZ, h + normal + 16, 8);
            out.time = ue::Get<float>(const_cast<uint8_t*>(h) + timeOff);
            out.overlapKnown=penetrateOff>=0 && penetrateMask;
            out.overlap=out.overlapKnown && (h[penetrateOff]&penetrateMask)!=0;
            if(!std::isfinite(out.time) || !std::isfinite(out.at[0]) || !std::isfinite(out.at[1]) || !std::isfinite(out.at[2])){out.ok=false;return out;}
            const int32_t idx = ue::Get<int32_t>(const_cast<uint8_t*>(h) + compOff);  // FWeakObjectPtr.ObjectIndex
            UObject* c = idx > 0 ? ue::Obj(idx) : nullptr;
            out.comp = c && ue::Live(c) ? ue::ClassName(c) : "?";
            out.compObj = c && ue::Live(c) ? c : nullptr;
        }
        return out;
    }
};
Api g;

double HalfHeight(UObject* pawn){
    const int off=ue::PropOffset(ue::ClassOf(pawn),"CapsuleComponent");
    auto* capsule=off>=0?ue::Get<UObject*>(pawn+off):nullptr;
    if(!capsule || !ue::Live(capsule))return 0;
    auto* cls=towerperf::ClassNamed("CapsuleComponent");
    auto* fn=cls?pe::FindFunction(cls,"GetScaledCapsuleHalfHeight"):nullptr;
    if(!fn || !ue::IsA(capsule,cls))return 0;
    Buf a{fn};if(!a.At("ReturnValue"))return 0;
    pe::Call(capsule,fn,a.b.data());const float h=ue::Get<float>(a.At("ReturnValue"));
    return std::isfinite(h) && h>=10 && h<=5000?h:0;
}
// A cached pre-teleport floor or walking mode alone is never support proof.
// Require this position, a real capsule, and a walkable blocking impact at its feet.
void NativeSupport(UObject* pawn,const double at[3],Probe& out){
    if(out.capsuleHalfHeight<=0)return;
    const int movement=ue::PropOffset(ue::ClassOf(pawn),"CharacterMovement");
    auto* obj=movement>=0?ue::Get<UObject*>(pawn+movement):nullptr;
    if(!obj || !ue::Live(obj) || ue::Get<UObject*>(obj+ue::Layout::ObjOuter)!=pawn)return;
    auto* cls=ue::ClassOf(obj);auto* floor=towerperf::ClassNamed("FindFloorResult");
    const int mode=ue::PropOffset(cls,"MovementMode"),last=ue::PropOffset(cls,"LastUpdateLocation"),current=ue::PropOffset(cls,"CurrentFloor");
    const int distance=floor?ue::PropOffset(floor,"FloorDist"):-1,hit=floor?ue::PropOffset(floor,"HitResult"):-1;
    const int blocking=floor?ue::PropOffset(floor,"bBlockingHit"):-1,walkable=floor?ue::PropOffset(floor,"bWalkableFloor"):-1;
    // Build26266538 read-only floor oracle: blocking/walkable bits0/1 at byte0.
    // Check the reflected fields and struct size before using that bit contract.
    if(mode<0 || last<0 || current<0 || distance<0 || hit<0 || blocking!=0 || walkable!=0 || ue::Get<int32_t>(floor+ue::Layout::StructSize)!=272)return;
    double updated[3],impactAt[3];
    if(!ue::Read(obj+last,updated,24) || !ue::Read(obj+current+hit+g.impact,impactAt,24))return;
    for(int i=0;i<3;++i)if(!std::isfinite(updated[i]) || !std::isfinite(impactAt[i]) || !std::isfinite(at[i]))return;
    if(std::hypot(updated[0]-at[0],updated[1]-at[1],updated[2]-at[2])>5)return;
    const float dist=ue::Get<float>(obj+current+distance);
    if(!std::isfinite(dist))return;
    out.nativeKnown=true;out.nativeGap=at[2]-out.capsuleHalfHeight-impactAt[2];
    const uint8_t state=ue::Get<uint8_t>(obj+mode),flags=ue::Get<uint8_t>(obj+current);
    out.nativeSupport=(state==1 || state==2) && (flags&3)==3 && dist>=-5 && dist<=60 && std::abs(out.nativeGap)<=60 && std::hypot(impactAt[0]-at[0],impactAt[1]-at[1])<=100;
}

}  // namespace

bool StandingAt(void* actorV, const double at[3]) {
    auto* actor=static_cast<UObject*>(actorV);
    if(!actor || !ue::Live(actor))return false;
    for(int i=0;i<3;++i)if(!std::isfinite(at[i]))return false;
    g.Resolve();
    if(g.impact<0)return false;
    Probe support;support.capsuleHalfHeight=HalfHeight(actor);
    NativeSupport(actor,at,support);
    return support.nativeKnown && support.nativeSupport;
}

bool Landing(void* actorV, const double nearAt[3], double out[3], std::string* why, bool trustDynamic) {
    auto fail = [&](const char* s) { if (why) *why = s; return false; };
    auto* actor = static_cast<UObject*>(actorV);
    if (!actor || !ue::Live(actor)) return fail("落点角色不可用");
    for (int i=0;i<3;++i) if (!std::isfinite(nearAt[i])) return fail("落点坐标无效");
    g.Resolve();
    const double half = HalfHeight(actor);
    if (half <= 0 || g.normal < 0) return fail("落点胶囊或地面法线接口未就绪");
    const double start[3]={nearAt[0],nearAt[1],nearAt[2]+150};
    const double end[3]={nearAt[0],nearAt[1],nearAt[2]-2000};
    bool found=false; double floorZ=-1e30;
    for (int i=0;i<2;++i) {
        const auto h=g.TraceTypes(actor,start,end,&g.types[i],1);
        if (h.Surface() && std::isfinite(h.normalZ) && h.normalZ>=0.71 && h.at[2]<=start[2] && h.at[2]>=end[2] &&
            std::hypot(h.at[0]-nearAt[0],h.at[1]-nearAt[1])<5) {
            floorZ=std::max(floorZ,h.at[2]); found=true;
        }
    }
    if (!found) return fail("目标没有已确认的可站立地面，拒绝直接放置");
    double candidate[3]={nearAt[0],nearAt[1],floorZ+half+5};
    if (std::abs(candidate[2]-nearAt[2])>2000) return fail("落点高度偏差过大");
    const int off=ue::PropOffset(ue::ClassOf(actor),"CapsuleComponent");
    auto* capsule=off>=0?ue::Get<UObject*>(actor+off):nullptr;
    auto* radiusFn=capsule?pe::FindFunction(ue::ClassOf(capsule),"GetScaledCapsuleRadius"):nullptr;
    auto* sweep= g.ksl ? pe::FindFunction(ue::ClassOf(g.ksl),"CapsuleTraceSingleForObjects") : nullptr;
    if (!radiusFn || !sweep) return fail("落点胶囊净空接口未就绪");
    Buf radiusArgs{radiusFn}; if(!radiusArgs.At("ReturnValue"))return fail("胶囊半径参数未核");
    pe::Call(capsule,radiusFn,radiusArgs.b.data());
    const float radius=ue::Get<float>(radiusArgs.At("ReturnValue")), height=static_cast<float>(half);
    if(!std::isfinite(radius)||radius<=0||radius>half)return fail("胶囊半径无效");
    // 34 层断魂冰面（ui10 实机）：胶囊净空在原高度被挡 → 抬高再试；只有动态通道挡、落点已知可放时照放
    bool clear=false, dynOnly=false;
    for (const double lift : {0.0, 60.0, 120.0}) {
    bool hit[2]={false,false};
    candidate[2]=floorZ+half+5+lift;
    for (int i=0;i<2;++i) {
        Buf a{sweep};
        for(const char* k:{"WorldContextObject","Start","End","Radius","HalfHeight","ObjectTypes","ActorsToIgnore","bIgnoreSelf","ReturnValue"})
            if(!a.At(k))return fail("落点胶囊净空参数未核");
        struct Arr { const void* data; int32_t num,max; } types{&g.types[i],1,1};
        UObject* ignored[]={actor}; Arr ignore{ignored,1,1};
        double from[3]={candidate[0],candidate[1],candidate[2]+2};
        std::memcpy(a.At("WorldContextObject"),&actor,8);
        std::memcpy(a.At("Start"),from,24);std::memcpy(a.At("End"),candidate,24);
        std::memcpy(a.At("Radius"),&radius,4);std::memcpy(a.At("HalfHeight"),&height,4);
        std::memcpy(a.At("ObjectTypes"),&types,16);std::memcpy(a.At("ActorsToIgnore"),&ignore,16);*a.At("bIgnoreSelf")=1;
        pe::Call(g.ksl,sweep,a.b.data());
        hit[i]=*a.At("ReturnValue")!=0;
    }
    if(!hit[0] && !hit[1]){clear=true;break;}
    if(!hit[0] && trustDynamic){dynOnly=true;break;}
    }
    if(!clear && !dynOnly)return fail("落点胶囊净空被阻挡");
    std::memcpy(out,candidate,24);if(why)*why=clear?"地面与胶囊净空已确认":"地面已确认；仅动态通道重叠，落点曾实机放置成功";return true;
}

bool StopMotion(void* actorV) {
    auto* actor=static_cast<UObject*>(actorV);
    if(!actor || !ue::Live(actor))return false;
    const int off=ue::PropOffset(ue::ClassOf(actor),"CharacterMovement");
    auto* movement=off>=0?ue::Get<UObject*>(actor+off):nullptr;
    if(!movement || !ue::Live(movement) || ue::Get<UObject*>(movement+ue::Layout::ObjOuter)!=actor)return false;
    auto* fn=pe::FindFunction(ue::ClassOf(movement),"StopMovementImmediately");
    const int velocity=ue::PropOffset(ue::ClassOf(movement),"Velocity");
    if(!fn || velocity<0 || ue::Get<int32_t>(fn+ue::Layout::StructSize)!=0)return false;
    uint8_t args[16]={};pe::Call(movement,fn,args);
    double v[3];if(!ue::Read(movement+velocity,v,24))return false;
    return std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]) && std::hypot(v[0],v[1],v[2])<.01;
}

Probe Measure(void* playerV, const double pl[3], void* bossV, const double bo[3]) {
    towerperf::Section _("可达探测");
    g.Resolve();
    Probe p;
    auto* player = static_cast<UObject*>(playerV);
    auto* boss = static_cast<UObject*>(bossV);
    if (!player || !ue::Live(player)) return p;
    p.capsuleHalfHeight=HalfHeight(player);
    if (g.line && g.ksl) {
        // 地面：从脚上方 1 米往下 20 米（玩家位置是胶囊中心，约在脚上 90 厘米）
        const double s[3] = {pl[0], pl[1], pl[2] + 100}, e[3] = {pl[0], pl[1], pl[2] - 2000};
        const auto ground=g.TraceTypes(player,s,e,g.types,2);
        p.groundKnown=ground.available && p.capsuleHalfHeight>0;
        p.ground=ground.Surface();
        p.traceTime=ground.time;p.groundComponent=ground.comp;
        p.traceOverlapKnown=ground.overlapKnown;p.traceOverlap=ground.overlap;
        if(ground.ok)p.traceGap=p.groundGap=pl[2]-p.capsuleHalfHeight-ground.at[2];
        p.groundSource="排除玩家后的射线";
        if (boss && ue::Live(boss)) {
            // 胸口高度的射线：在离 Boss 2 米以外被挡才算挡住（打到 Boss 自己 / 贴身的东西不算）
            const double a[3] = {pl[0], pl[1], pl[2] + 60}, b[3] = {bo[0], bo[1], bo[2] + 60};
            const auto sight=g.TraceTypes(player,a,b,g.types,2);
            p.lineKnown=sight.available && (!sight.ok || sight.Surface());
            if (sight.Surface()) p.lineBlocked = std::hypot(sight.at[0] - bo[0], sight.at[1] - bo[1], sight.at[2] - (bo[2] + 60)) > 200;
        }
    }
    NativeSupport(player,pl,p);
    if(p.nativeSupport){p.groundKnown=p.ground=true;p.groundGap=p.nativeGap;p.groundSource="原生当前可站立地面（位置已核）";}
    if (g.find && g.nav && boss && ue::Live(boss)) {
        Buf a{g.find};
        if (a.At("WorldContextObject") && a.At("PathStart") && a.At("PathEnd") && a.At("ReturnValue")) {
            std::memcpy(a.At("WorldContextObject"), &playerV, 8);
            std::memcpy(a.At("PathStart"), pl, 24); std::memcpy(a.At("PathEnd"), bo, 24);
            if (auto* c = a.At("PathfindingContext")) std::memcpy(c, &playerV, 8);
            pe::Call(g.nav, g.find, a.b.data());
            UObject* path = ue::Get<UObject*>(a.At("ReturnValue"));
            if (path && ue::Live(path) && g.isValid && g.isPartial) {
                uint8_t r[16] = {};
                pe::Call(path, g.isValid, r);
                if (r[0]) {  // 没有导航数据时路径无效：算"不知道"，不拒绝
                    std::memset(r, 0, sizeof r);
                    pe::Call(path, g.isPartial, r);
                    p.navKnown = true;
                    p.navOk = r[0] == 0;
                }
            }
        }
    }
    return p;
}

FloorAudit Audit(void* playerV, const double pl[3], double halfHeight, double distanceCm) {
    towerperf::Section _("地面审计");
    g.Resolve();
    FloorAudit out;
    auto* player = static_cast<UObject*>(playerV);
    if (!g.line || !player || !ue::Live(player)) return out;
    Probe native;native.capsuleHalfHeight=HalfHeight(player);NativeSupport(player,pl,native);
    out.nativePlayerSupport=native.nativeSupport;out.nativePlayerGap=native.nativeGap;
    const uint8_t st[1] = {g.types[0]}, dy[1] = {g.types[1]};
    auto shot = [&](double x, double y, const uint8_t* t) {
        const double s[3] = {x, y, pl[2] + 250}, e[3] = {x, y, pl[2] - halfHeight - 250};
        return g.TraceTypes(player, s, e, t, 1);
    };
    {
        const auto a = shot(pl[0], pl[1], st), b = shot(pl[0], pl[1], dy);
        out.playerOn = a.Surface() ? "WorldStatic:" + a.comp : b.Surface() ? "WorldDynamic:" + b.comp : (a.ok||b.ok)?"查询被起点命中遮挡，不能当作脚下地面":"WorldStatic / WorldDynamic 未命中";
        if(out.nativePlayerSupport)out.playerOn+=std::format("；原生当前位置有可站立支撑，脚下{:.1f}厘米",out.nativePlayerGap);
    }
    for (int i = 0; i < 12; ++i) {
        const double ang = i * 3.14159265358979323846 / 6.0;
        const double x = pl[0] + distanceCm * std::cos(ang), y = pl[1] + distanceCm * std::sin(ang);
        const auto a = shot(x, y, st), b = shot(x, y, dy);
        ++out.n;
        out.staticOverlap+=a.ok && a.overlap;
        out.dynamicOverlap+=b.ok && b.overlap;
        if (a.Surface()) ++out.staticHits;
        else if (b.Surface()) ++out.dynamicHits;
        else if ((a.ok && a.overlap) || (b.ok && b.overlap)) ++out.startSolid;
        else if (a.ok || b.ok || !a.available || !b.available) ++out.unresolved;
        else ++out.none;
        if (i < 3)
            out.sample += std::format("{}[{:.0f},{:.0f}] 静态 {}{} 动态 {}{}", out.sample.empty() ? "" : "；", x, y, a.ok ? std::format("z{:.0f}/t{:.2f}/初始穿透{}", a.at[2], a.time,a.overlapKnown?(a.overlap?"是":"否"):"未核") : "无",
                                      a.ok ? "(" + a.comp + ")" : "", b.ok ? std::format("z{:.0f}/t{:.2f}/初始穿透{}", b.at[2], b.time,b.overlapKnown?(b.overlap?"是":"否"):"未核") : "无", b.ok ? "(" + b.comp + ")" : "");
    }
    return out;
}

// 3.6.0 场地实测：射线起点在触发盒 / 体积里（Box / Brush，起点重叠、Time 0）时，把那个组件所属的 Actor 加进忽略名单再打（最多 4 次），
// 打到真正的地面 / 墙为止。只读
Api::Hit TraceThrough(UObject* ctx, const double s[3], const double e[3], int channel) {
    static UObject* getOwner = nullptr;
    if (!getOwner) getOwner = pe::FindFunction(towerperf::ClassNamed("ActorComponent"), "GetOwner");
    std::vector<UObject*> skip;
    Api::Hit h;
    for (int i = 0; i < 5; ++i) {
        h = g.TraceTypes(ctx, s, e, &g.types[channel], 1, &skip);
        if (!h.ok || !h.overlap || !h.compObj || !getOwner) return h;
        uint8_t r[16] = {};
        pe::Call(h.compObj, getOwner, r);
        UObject* owner = *reinterpret_cast<UObject**>(r);
        if (!owner || !ue::Live(owner) || std::find(skip.begin(), skip.end(), owner) != skip.end()) return h;
        skip.push_back(owner);
    }
    return h;
}
bool Ray(void* ctxV, const double from[3], const double to[3], double* dist, std::string* comp) {
    g.Resolve();
    auto* ctx = static_cast<UObject*>(ctxV);
    if (!g.line || !ctx || !ue::Live(ctx)) return false;
    // 静态 / 动态分开打：一个通道起点在体积里（断魂冰面、榕树一带的 WorldDynamic 体积）不影响另一个；取更近的有效命中
    bool hit = false; double best = 1e18;
    for (int i = 0; i < 2; ++i) {
        const auto h = TraceThrough(ctx, from, to, i);
        if (!h.Surface()) continue;
        const double d = std::sqrt((h.at[0] - from[0]) * (h.at[0] - from[0]) + (h.at[1] - from[1]) * (h.at[1] - from[1]) + (h.at[2] - from[2]) * (h.at[2] - from[2]));
        if (d < best) { best = d; hit = true; if (comp) *comp = (i ? "WorldDynamic:" : "WorldStatic:") + h.comp; }
    }
    if (hit && dist) *dist = best;
    return hit;
}
bool GroundBelow(void* ctxV, double x, double y, double zTop, double zBottom, double out[3]) {
    g.Resolve();
    auto* ctx = static_cast<UObject*>(ctxV);
    if (!g.line || !ctx || !ue::Live(ctx)) return false;
    const double s[3] = {x, y, zTop}, e[3] = {x, y, zBottom};
    bool hit = false;
    for (int i = 0; i < 2; ++i) {
        const auto h = TraceThrough(ctx, s, e, i);
        if (!h.Surface()) continue;
        if (g.normal >= 0 && std::isfinite(h.normalZ) && std::abs(h.normalZ) > 1e-6 && h.normalZ < 0.3) continue;  // 读到了法线且是墙面才拒绝
        if (!hit || h.at[2] > out[2]) { std::memcpy(out, h.at, 24); hit = true; }  // 两个通道都命中取更高的（站得上去的那层）
    }
    return hit;
}
std::string GroundDiag(void* ctxV, double x, double y, double zTop, double zBottom) {
    g.Resolve();
    auto* ctx = static_cast<UObject*>(ctxV);
    if (!g.line || !ctx || !ue::Live(ctx)) return "射线不可用";
    const double s[3] = {x, y, zTop}, e[3] = {x, y, zBottom};
    std::string out;
    for (int i = 0; i < 2; ++i) {
        const auto h = TraceThrough(ctx, s, e, i);
        out += std::format("{}{} 可用{} 命中{} 重叠{}/{} t{:.3f} z{:.0f} 法线z{:.2f} {}", i ? "；" : "", i ? "动态" : "静态", h.available, h.ok, h.overlapKnown, h.overlap, h.time, h.at[2], h.normalZ, h.comp);
    }
    return out;
}

std::string Describe(const FloorAudit& a) {
    return std::format("候选 {} 个：WorldStatic 有效命中 {}、只有 WorldDynamic 有效命中 {}、两通道均无有效支撑且初始重叠 {}、都没打到 {}、未核 {}；通道初始重叠 Static={} Dynamic={}（可与有效支撑并存）；玩家脚下 {}；样本 {}", a.n, a.staticHits, a.dynamicHits,
                       a.startSolid, a.none,a.unresolved,a.staticOverlap,a.dynamicOverlap,a.playerOn, a.sample.empty() ? "-" : a.sample);
}

std::string Describe(const Probe& p) {
    return std::format("地面 {}{}，视线 {}，导航 {}；来源 {}，胶囊半高 {:.1f}，射线差 {:.1f}/t{:.3f}/{} 初始穿透{}，原生地面 {}差{:.1f}", !p.groundKnown ? "未测" : p.ground ? "有" : "无", p.ground ? std::format("（脚下 {:.1f} 厘米）", p.groundGap) : "",
                       !p.lineKnown ? "未测" : p.lineBlocked ? "被挡" : "通", !p.navKnown ? "无数据" : p.navOk ? "可达" : "不完整",p.groundSource,p.capsuleHalfHeight,p.traceGap,p.traceTime,p.groundComponent,p.traceOverlapKnown?(p.traceOverlap?"是":"否"):"未核",p.nativeKnown?(p.nativeSupport?"可站立":"未支持"):"未验证",p.nativeGap);
}

}  // namespace towerreach
