// 自动应对：蓝光自动格挡 / 红光自动闪避 / 普攻自动格挡 / 普攻自动闪避（后两者二选一）。
// 敌人每次攻击会在玩家 DeflectComponent.ReceiveFrames 里写一条"来袭帧"（命中前约 0.3~0.7 秒出现），
// 帧里 SourceAction 弱引用指向攻击动作，动作的 DeflectBulletType 区分 Normal / BlueEye / RedEye / CantDeflect …
// （实机：弓手特殊攻击 = RedEye，女匪双锄攻击 = BlueEye，炸弹人技能 = CantDeflect）。
// 使用游戏自己的格挡/完美闪避入口；不模拟按键、不写判定时间戳、不强制 CanActivate。
// 红光与普攻分别调时机；ReceiveFrames 是稀疏表，必须检查分配位，-1 镜像只用于观察结果。
#include <cmath>
#include <algorithm>
#include <array>
#include <cstring>
#include <format>
#include <map>
#include <limits>
#include <fstream>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>

#include <windows.h>

#include "../../host/log.h"
#include "../../services/attributes.h"
#include "../../services/inventory.h"
#include "../../services/abshook.h"
#include "../../services/process_event.h"
#include "../../services/native_exec.h"
#include "../feature.h"
#include "../world/enemy_spawn.h"

namespace pbz {
namespace {

enum DeflectType : uint8_t { CantDeflect = 0, Normal, BlueEye, RedEye, PowerAttack, HoldDeflect };
enum Flag { kBlue = 1, kRed = 2, kNormalDeflect = 4, kNormalDodge = 8, kPerfectDeflect = 16, kPerfectDodge = 32, kNormalMix = 64, kAutoAttack = 128 };
constexpr int kNormalFlags = kNormalDeflect | kNormalDodge | kNormalMix;
constexpr int kContactFlags = kNormalFlags | kBlue | kRed;
constexpr int kDodgeFlags = kRed; // 只需观察红光反制；普通模式不调用反制闪避兜底
constexpr int kDeflectFlags = kBlue | kNormalFlags;

bool NormalType(int type) { return type == CantDeflect || type == Normal || type == HoldDeflect; }
bool Guardable(int type) { return type == Normal || type == BlueEye || type == HoldDeflect; }
bool ColoredType(int type) { return type==RedEye||type==BlueEye||type==PowerAttack; }
int AttackTagType(std::string_view tag) {
    if(tag=="Bullet.Tag.Feature.Deflect.PowerAttack")return PowerAttack;
    if(tag=="Bullet.Tag.Feature.Deflect.RedEye")return RedEye;
    if(tag=="Bullet.Tag.Feature.Deflect.BlueEye")return BlueEye;
    return -1;
}
int MergeAttackColor(int prior,int incoming) {
    if(prior==PowerAttack||incoming==PowerAttack)return PowerAttack;
    if(prior==RedEye||incoming==RedEye)return RedEye;
    if(prior==BlueEye||incoming==BlueEye)return BlueEye;
    return prior;
}
int ResponsePolicy(int flags, int type) {
    if (type == BlueEye) return flags & kBlue ? 1 : 0;
    if (type == PowerAttack) return 0;  // 用户 2026-11-06：去掉黑光闪避（黑光交给玩家）
    if (type == RedEye) return flags & kRed ? 2 : 0;
    if (!NormalType(type) || !(flags & kNormalFlags)) return 0;
    if (type == CantDeflect) return 2;
    // 用户 2026-11-06：自动格挡 / 闪避都开时格挡优先，按混合比例（默认格挡 85%、闪避 15%）；挡不了的照样闪
    if ((flags & kNormalDeflect) && (flags & kNormalDodge)) return 3;
    return flags & kNormalDeflect ? 1 : flags & kNormalDodge ? 2 : 3;
}
bool OrdinaryThreatPermission(int flags, int type, bool framed, bool timelineOwned, bool preciseContact) {
    return (flags & kNormalFlags) && !ColoredType(type) &&
        (preciseContact || (!framed && !timelineOwned));
}
int ChooseNormalResponse(int mode, bool guardable, bool scarce, bool combo, bool held, bool unstable, int roll, int guardPercent) {
    if (!guardable || scarce) return 2;
    if (mode == 1) return 1;
    if (mode == 2) return combo || held ? 1 : 2;
    if (mode == 3) return combo || held || unstable || roll < guardPercent ? 1 : 2;
    return 0;
}
bool ScarceShachi(bool wasScarce, double current, double maximum) {
    if (!std::isfinite(current) || !std::isfinite(maximum) || current < 0 || maximum <= 0) return false;
    return current <= maximum*(wasScarce ? .30 : .15);
}

// 学到的 early 是为“提前举防并举住”准备的，不能拉长闪避的无敌帧。
double ResponseLead(int type, int response, double early, double guardLead, double dodgeLead, double redLead, double gap) {
    if (response == 1) return guardLead + std::max(0.0, early) + std::clamp(gap, 0.0, 0.06);
    return (type == RedEye ? redLead : dodgeLead) + std::clamp(gap, 0.0, 0.04);
}
bool FrameSchedulesThreat(int handle, int type, bool contact, double hit, double now) {
    return handle >= 0 && type >= CantDeflect && type <= HoldDeflect && !contact &&
        std::isfinite(hit) && hit >= now - 0.02 && hit <= now + 2.0;
}

// Native delay-projectile frames broadcast a real counter window with this
// exact float sentinel promoted to double, rather than an impact timestamp.
constexpr double kWindowHitTime = static_cast<double>(-3.4e38f);
constexpr int64_t kWindowStamp = std::numeric_limits<int64_t>::min();
bool WindowCounterFrame(int handle, int type, bool dodge, bool validCounter, double hit) {
    return handle >= 0 && type == RedEye && dodge && validCounter && hit == kWindowHitTime;
}
bool AttackExpired(int64_t stamp, double lastWindowSeen, double now) {
    return stamp == kWindowStamp ? now-lastWindowSeen > 2 : stamp < (now-2)*1000000;
}

template<class T> T Copied(const uint8_t* data) {
    T value{}; memcpy(&value, data, sizeof value); return value;
}

// These helpers are also compiled by the isolated layout/policy fixture.
struct ReceiveSlots {
    uint8_t* data = nullptr;
    int count = 0;
    std::array<uint32_t, 8> allocated{};
    bool Contains(int index) const {
        return index >= 0 && index < count && (allocated[index / 32] & (1u << (index % 32)));
    }
};
bool ReadReceiveSlots(uint8_t* map, ReceiveSlots& output) {
    std::array<uint8_t, 0x38> header{};
    if (!map || !ue::Read(map, header.data(), header.size())) return false;
    const int count = Copied<int32_t>(header.data() + 8);
    const int capacity = Copied<int32_t>(header.data() + 12);
    const int bits = Copied<int32_t>(header.data() + 0x28);
    const int bitCapacity = Copied<int32_t>(header.data() + 0x2c);
    const int free = Copied<int32_t>(header.data() + 0x34);
    if (count < 0 || count > 256 || capacity < count || bits != count || bitCapacity < bits || free < 0 || free > count) return false;
    ReceiveSlots candidate;
    candidate.count = count;
    candidate.data = Copied<uint8_t*>(header.data());
    if (count) {
        if (!candidate.data) return false;
        auto* bitData = bitCapacity > 128 ? Copied<uint8_t*>(header.data() + 0x20) : map + 0x10;
        if (!bitData || !ue::Read(bitData, candidate.allocated.data(), ((count + 31) / 32) * 4)) return false;
        int allocated = 0;
        for (int i = 0; i < count; ++i) allocated += candidate.Contains(i) ? 1 : 0;
        if (allocated != count - free) return false;
    }
    output = candidate;
    return true;
}
// Milliseconds alone merge independent simultaneous hits. Include the real
// handle AND the source weak reference (index/serial), with microsecond time.
using AttackId = std::tuple<int32_t, uint64_t, int64_t>;
AttackId Identity(int handle, const uint8_t* frame, double hit) {
    return {handle, Copied<uint64_t>(frame), hit == kWindowHitTime ? kWindowStamp : std::llround(hit * 1000000)};
}
bool SameMirror(const AttackId& attack, const AttackId& mirror) {
    return std::get<1>(attack) == std::get<1>(mirror) && std::get<2>(attack) == std::get<2>(mirror);
}
struct Incoming {
    AttackId id;
    std::array<uint8_t, 0x50> frame{};
    int handle = 0, type = 0, action = 0;
    double hit = 0;
    double early = 0;
    bool guard = false;  // 远处飞剑等可格挡攻击，交替模式优先举住
    bool volley = false; // 远处扔出的一串（飞剑连发）：一次预警后陆续打到，防要多举一会儿
    bool windowCounter = false;
    double tail = 0;     // 学到的连发尾巴长度：举防举到命中后这么久
};
auto ResponseOrder(const Incoming& frame, double now, double quantum) {
    const auto bucket = static_cast<int64_t>(std::floor((frame.hit - now) / quantum));
    const int danger = frame.type == RedEye || frame.type==PowerAttack ? 0 : frame.type == BlueEye ? 1 : 2;
    return std::tuple{bucket, danger, frame.hit, frame.id};
}

// 飞行时间与近战预警偏差是不同的量；慢弹/延迟爆炸不能在重载时截成 0.6 秒。
double TimingShift(const std::string& key, double value) {
    return key.starts_with("弹:") ? std::clamp(value, 0.08, 3.0) : std::clamp(value, -0.6, 0.6);
}

// 相对运动与玩家竖直胶囊外接圆柱的交集。只看径向接近速度会把横穿/掠过算成命中，
// 也会忽略玩家自己的移动。返回首次进入身体范围的时间；负数表示不会接触。
double ProjectileContact(const double delta[3], const double velocity[3], double radius, double height) {
    for (int i = 0; i < 3; ++i)
        if (!std::isfinite(delta[i]) || !std::isfinite(velocity[i])) return -1;
    if (!(radius > 0 && height > 0)) return -1;
    if (delta[0]*velocity[0] + delta[1]*velocity[1] + delta[2]*velocity[2] >= 0) return -1;
    double enter = 0, leave = 30;
    const double speed2 = velocity[0] * velocity[0] + velocity[1] * velocity[1];
    const double dot = delta[0] * velocity[0] + delta[1] * velocity[1];
    const double outside = delta[0] * delta[0] + delta[1] * delta[1] - radius * radius;
    if (speed2 < 1) { if (outside > 0) return -1; }
    else {
        const double disc = dot * dot - speed2 * outside;
        if (disc < 0) return -1;
        const double root = std::sqrt(disc);
        enter = std::max(enter, (-dot - root) / speed2);
        leave = std::min(leave, (-dot + root) / speed2);
    }
    if (std::abs(velocity[2]) < 1) { if (std::abs(delta[2]) > height) return -1; }
    else {
        const double a = (-height - delta[2]) / velocity[2], b = (height - delta[2]) / velocity[2];
        enter = std::max(enter, std::min(a, b));
        leave = std::min(leave, std::max(a, b));
    }
    return enter <= leave ? enter : -1;
}

bool TimedRetryReady(double now, double due, double lead, double lastTry, bool finished) {
    return !finished && now >= due - lead && now <= due + 0.1 && now - lastTry >= 0.025;
}

double TimelineRate(double current, double previous, double elapsed) {
    if (!std::isfinite(current) || !std::isfinite(previous) || !(elapsed > 0 && elapsed < 0.25)) return 0;
    const double rate = (current-previous)/elapsed;
    return rate > 0.01 && rate <= 20 ? rate : 0; // 蓄力停帧/暂停时不拿墙上时间推算
}

// Native duration actions consume StartBulletTimes against their local elapsed
// time. In particular CantDeflect can start at 0 with RuntimePreHitPointDuration=0.
// Read the prepared runtime schedule, never synthesize a delay from the template.
bool ReadImpactSchedule(const uint8_t* header, std::vector<float>& output) {
    std::array<uint8_t,16> raw{};
    if (!header || !ue::Read(header,raw.data(),raw.size())) return false;
    auto* data = Copied<const uint8_t*>(raw.data());
    const int count = Copied<int32_t>(raw.data()+8), capacity = Copied<int32_t>(raw.data()+12);
    if (!data || count <= 0 || count > 64 || capacity < count || capacity > 1024) return false;
    std::vector<float> points(count);
    if (!ue::Read(data,points.data(),count*sizeof(float))) return false;
    float previous = -1;
    for (float point : points) {
        if (!std::isfinite(point) || point < 0 || point > 60 || point < previous) return false;
        previous = point;
    }
    output = std::move(points);
    return true;
}
double TimelineContact(double start, double point, double cursor, double rate) {
    if (!std::isfinite(start) || !std::isfinite(point) || !std::isfinite(cursor) || !std::isfinite(rate) ||
        start < 0 || point < 0 || rate <= 0 || rate > 20) return NAN;
    return (start+point-cursor)/rate;
}

// A duration bullet is a collision interval, not a single pre-hit timestamp.
// Keep the native pairs: a later segment must not inherit the first one's end.
bool ReadCollisionWindows(const uint8_t* starts, const uint8_t* ends, std::vector<std::pair<float,float>>& output) {
    std::vector<float> a, b;
    if (!ReadImpactSchedule(starts,a) || !ReadImpactSchedule(ends,b) || a.size() != b.size()) return false;
    std::vector<std::pair<float,float>> windows;
    for (size_t i = 0; i < a.size(); ++i) {
        if (b[i] < a[i]) return false;
        windows.emplace_back(a[i],b[i]);
    }
    output = std::move(windows);
    return true;
}
bool CollisionWindowDue(double begin, double end, double lead) {
    return std::isfinite(begin) && std::isfinite(end) && begin <= end && end >= 0 && begin <= lead;
}
bool FrameWithinCollision(double hit, double begin, double end) {
    return std::isfinite(hit) && std::isfinite(begin) && std::isfinite(end) && begin <= end &&
        hit >= begin-.02 && hit <= end+.02;
}
bool CollisionRetry(double now, double tried, double accepted, bool guarded, bool guarding, bool dashing) {
    if (now-tried < .04) return false;
    if (accepted < 0) return true;
    if (guarded) return !guarding;
    return !dashing && now-accepted >= .25;
}
bool DashBlocksResponse(bool running, bool cancelPhase) { return running && !cancelPhase; }
bool ContactPermission(int flags, int type) {
    return (flags & kNormalFlags) && (type == -1 || NormalType(type));
}
bool PreciseContactPermission(int flags, int type) {
    if (type==BlueEye) return (flags&kBlue)!=0;
    if (type==RedEye||type==PowerAttack) return (flags&kRed)!=0;
    return ContactPermission(flags,type);
}

// Particle actors are emitters: their origin/spawn age is not the contact
// position/time of any one ray. Even an old learned row must not drive them.
bool SpawnTimingAllowed(bool actor, bool particle, bool moving, bool positioned) {
    return actor && !particle && !(moving && positioned);
}
bool LateralEscape(const double direction[3], const double sweep[3], int side, double output[3]) {
    auto usable = [](const double* v) {
        return v && std::all_of(v,v+3,[](double x){return std::isfinite(x);}) && std::hypot(v[0],v[1]) > .001;
    };
    const double* axis = usable(direction) ? direction : usable(sweep) ? sweep : nullptr;
    if (!axis) return false;
    const double length = std::hypot(axis[0],axis[1]), sign = side < 0 ? -1.0 : 1.0;
    output[0] = -axis[1]/length*sign; output[1] = axis[0]/length*sign; output[2] = 0;
    return true;
}

using NativeAttack = bool (__fastcall*)(ue::UObject*,ue::UObject*,const uint64_t*,const double*,const double*,const double*,const double*);
// Captured current-build ABI fingerprints, not relocated addresses. The
// thunk passes a POINTER to FName and four double FVector pointers. Only
// rel32 call displacements may vary. Any other build/layout fails closed.
constexpr uint8_t kAttackThunkTail[] = {0x4d,0x8b,0xce,0x4d,0x8b,0xc7,0x48,0x0f,0x45,0xc8,0x48,0x8b,0x43,0x20,0x48,0x89,0x4c,0x24,0x30,0x48,0x85,0xc0,0x48,0x89,0x74,0x24,0x28,0x49,0x8b,0xcd,0x40,0x0f,0x95,0xc7,0x48,0x89,0x6c,0x24,0x20,0x48,0x03,0xf8,0x48,0x89,0x7b,0x20,0xe8,0x34,0x90,0x06,0x00,0x48,0x8b,0x9c,0x24,0xe0,0x00,0x00,0x00,0x41,0x88,0x04,0x24,0x48,0x81,0xc4,0xa0,0x00,0x00,0x00,0x41,0x5f,0x41,0x5e,0x41,0x5d,0x41,0x5c,0x5f,0x5e,0x5d,0xc3};
constexpr uint8_t kAttackBody[] = {0x48,0x89,0x5c,0x24,0x08,0x48,0x89,0x6c,0x24,0x10,0x48,0x89,0x74,0x24,0x18,0x48,0x89,0x7c,0x24,0x20,0x41,0x56,0x48,0x83,0xec,0x50,0x80,0xb9,0xd0,0x00,0x00,0x00,0x00,0x49,0x8b,0xe9,0x4d,0x8b,0xf0,0x48,0x8b,0xda,0x48,0x8b,0xf1,0x74,0x67,0x48,0x85,0xd2,0x74,0x62,0x48,0x8b,0xba,0xa8,0x00,0x00,0x00,0x48,0x85,0xff,0x74,0x56,0xb2,0x01,0x48,0x8b,0xcf,0xe8,0xa6,0x74,0xe0,0xfe,0x48,0x8b,0x0e,0x4c,0x8b,0xcb,0x4c,0x8b,0xc0,0x48,0x8b,0xd7,0x4c,0x8b,0x91,0x08,0x03,0x00,0x00,0x48,0x8b,0x8c,0x24,0x90,0x00,0x00,0x00,0x48,0x89,0x4c,0x24,0x40,0x48,0x8b,0x8c,0x24,0x88,0x00,0x00,0x00,0x48,0x89,0x4c,0x24,0x38,0x48,0x8b,0x8c,0x24,0x80,0x00,0x00,0x00,0x48,0x89,0x4c,0x24,0x30,0x48,0x8b,0xce,0x48,0x89,0x6c,0x24,0x28,0x4c,0x89,0x74,0x24,0x20,0x41,0xff,0xd2,0xeb,0x02,0x32,0xc0,0x48,0x8b,0x5c,0x24,0x60,0x48,0x8b,0x6c,0x24,0x68,0x48,0x8b,0x74,0x24,0x70,0x48,0x8b,0x7c,0x24,0x78,0x48,0x83,0xc4,0x50,0x41,0x5e,0xc3};
int FindAttackCall(const uint8_t* bytes, size_t size) {
    if (!bytes) return -1;
    int found = -1;
    for (size_t at=0;at+sizeof(kAttackThunkTail)<=size;++at) {
        bool match=true;
        for (size_t i=0;i<sizeof(kAttackThunkTail);++i)
            if (!(i>=47 && i<51) && bytes[at+i]!=kAttackThunkTail[i]) { match=false; break; }
        if (match) { if (found>=0) return -1; found=static_cast<int>(at+46); }
    }
    return found;
}
bool AttackBodyMatches(const uint8_t* bytes, size_t size) {
    if (!bytes || size!=sizeof(kAttackBody)) return false;
    for (size_t i=0;i<size;++i) if (!(i>=70 && i<74) && bytes[i]!=kAttackBody[i]) return false;
    return true;
}

// 已验证的 FTransform(double) 布局：Quat / Translation / padding / Scale / padding。
// 在攻击的局部空间判断球、胶囊、盒；落点不依赖发射者的位置或朝向。
bool AreaContains(const double me[3], const double transform[12], int shape, double radius, double halfHeight,
                  const double extent[3]) {
    for (int i = 0; i < 12; ++i) if (!std::isfinite(transform[i])) return false;
    const double norm = transform[0]*transform[0] + transform[1]*transform[1] + transform[2]*transform[2] + transform[3]*transform[3];
    if (std::abs(norm - 1) > 0.02) return false;
    double v[3] = {me[0]-transform[4], me[1]-transform[5], me[2]-transform[6]};
    const double q[3] = {-transform[0], -transform[1], -transform[2]};
    const double cross[3] = {2*(q[1]*v[2]-q[2]*v[1]), 2*(q[2]*v[0]-q[0]*v[2]), 2*(q[0]*v[1]-q[1]*v[0])};
    const double rotated[3] = {v[0]+transform[3]*cross[0]+q[1]*cross[2]-q[2]*cross[1],
        v[1]+transform[3]*cross[1]+q[2]*cross[0]-q[0]*cross[2], v[2]+transform[3]*cross[2]+q[0]*cross[1]-q[1]*cross[0]};
    double padding = 0;
    for (int i = 0; i < 3; ++i) {
        const double scale = std::abs(transform[8+i]);
        if (scale < 0.01 || scale > 100) return false;
        v[i] = rotated[i] / scale;
        padding = std::max(padding, 100.0 / scale); // 玩家身体的保守包围，边缘不漏
    }
    if (shape == 2) {
        for (int i = 0; i < 3; ++i) if (!std::isfinite(extent[i]) || extent[i] <= 0 || extent[i] > 5000) return false;
        return std::abs(v[0]) <= extent[0]+padding && std::abs(v[1]) <= extent[1]+padding && std::abs(v[2]) <= extent[2]+padding;
    }
    if (shape < 0 || shape > 1 || !(radius > 0 && radius <= 5000)) return false;
    if (shape == 1 && (!std::isfinite(halfHeight) || halfHeight < radius || halfHeight > 5000)) return false;
    if (shape == 1) v[2] = std::max(0.0, std::abs(v[2]) - std::max(0.0, halfHeight-radius));
    return std::hypot(v[0], v[1], v[2]) <= radius + padding;
}

// CollisionShapeWrapper wraps UE FCollisionShape: type + three float union
// members, not CollisionShapeProfile's enum/Vector(double) layout.
bool DecodeNativeShape(const uint8_t* raw, int& shape, double& radius, double& height, double extent[3]) {
    const int nativeType = Copied<int32_t>(raw);
    const double a = Copied<float>(raw+4), b = Copied<float>(raw+8), c = Copied<float>(raw+12);
    if (!std::isfinite(a) || a <= 0 || a > 5000) return false;
    if (nativeType == 1) {
        if (!std::isfinite(b) || !std::isfinite(c) || b <= 0 || c <= 0 || b > 5000 || c > 5000) return false;
        shape = 2; extent[0] = a; extent[1] = b; extent[2] = c;
    } else if (nativeType == 2) { shape = 0; radius = a; }
    else if (nativeType == 3) {
        if (!std::isfinite(b) || b < a || b > 5000) return false;
        shape = 1; radius = a; height = b;
    } else return false;
    return true;
}

// 处决闸门（用户 2026-10-31）：处决动作期间玩家本来就无敌，自动闪避 / 格挡 / 攻击会把处决打断；处决结束后，
// 被处决敌人留下的攻击帧、学到的出招时刻还会触发"没人打却在后闪"（大刀处决实测）。处决中以及结束后 kGrace 秒内一律不出手。
// Also use native pending state, actual Execution ability tags/base classes,
// and pre-activation observers. A cached per-frame state alone misses an
// execution that begins before a same-frame native collision callback.
bool ExecutionTag(std::string_view tag) {
    return tag=="Ability.Action.Execution"||tag.starts_with("Ability.Action.Execution.");
}
bool ExecutionBase(std::string_view name) {
    if(name=="GA_ExecutionAbilityBase_C"||name=="GA_SimpleExecution_Base_C"||name=="GA_ExactExecutionBase")return true;
    // 用户 2026-11-06：处决时仍会自己后闪。连续处决、助跑处决、空中 / 抓取处决、各兵器专属处决不一定继承上面三个基类：
    // 父类链上类名含 Execution / ExecuteSeq 都算处决；被处决方（Executed）、饰品 / 被动类（常驻激活，会永久挡住自动应对）除外
    if(!name.starts_with("GA_"))return false;
    if(name.find("Executed")!=std::string_view::npos||name.find("Accessory")!=std::string_view::npos||
       name.find("Passive")!=std::string_view::npos||name.find("SwitchCool")!=std::string_view::npos)return false;
    return name.find("Execution")!=std::string_view::npos||name.find("ExecuteSeq")!=std::string_view::npos;
}
bool RetireExecutionFrame(double hit,double now){return std::isfinite(hit)&&hit>=now-2&&hit<=now;}
struct ExecGate {
    static constexpr double kGrace = 0.6;
    double until = 0;
    bool blocked = false;
    int suppressed = 0;
    // 返回 +1 = 刚进入、-1 = 刚恢复、0 = 无变化
    int Update(double now, bool executing) {
        if (executing) until = now + kGrace;
        const bool next = now < until;
        const int edge = next == blocked ? 0 : next ? 1 : -1;
        if (edge > 0) suppressed = 0;
        blocked = next;
        return edge;
    }
    bool Block() { if (blocked) ++suppressed; return blocked; }
    int Pulse(double now,double duration) { until=std::max(until,now+duration);return Update(now,false); }
};

#ifndef PBZ_AUTO_RESPONSE_TEST

// 反射函数调用参数缓冲：按 UFunction 的参数属性算偏移和总大小
struct Params {
    ue::UObject* fn = nullptr;
    std::vector<uint8_t> buf;
    explicit Params(ue::UObject* f) : fn(f) {
        size_t size = 0;
        for (auto* p = ue::Get<uint8_t*>(f + ue::Layout::StructChildProps); p; p = ue::Get<uint8_t*>(p + ue::Layout::FieldNext))
            size = std::max(size, static_cast<size_t>(ue::Get<int32_t>(p + ue::Layout::PropOffset) + ue::Get<int32_t>(p + 0x34) * std::max(1, ue::Get<int32_t>(p + 0x30))));
        buf.assign(size + 16, 0);
    }
    int Off(std::string_view name) const {
        for (auto* p = ue::Get<uint8_t*>(fn + ue::Layout::StructChildProps); p; p = ue::Get<uint8_t*>(p + ue::Layout::FieldNext))
            if (ue::Name(ue::Get<uint32_t>(p + ue::Layout::FieldName)) == name) return ue::Get<int32_t>(p + ue::Layout::PropOffset);
        return -1;
    }
};

struct ObjectRef {
    ue::UObject *object = nullptr, *cls = nullptr, *outer = nullptr;
    int index = -1;
    uint64_t name = 0;
    explicit ObjectRef(ue::UObject* o = nullptr) : object(o) {
        if (!o) return;
        cls = ue::ClassOf(o); outer = ue::Get<ue::UObject*>(o + ue::Layout::ObjOuter);
        index = ue::Get<int32_t>(o + ue::Layout::ObjIndex, -1);
        name = ue::Get<uint64_t>(o + ue::Layout::ObjName);
    }
    bool Alive() const {
        return object && index >= 0 && ue::Obj(index) == object && ue::ClassOf(object) == cls &&
            ue::Get<ue::UObject*>(object + ue::Layout::ObjOuter) == outer && ue::Get<uint64_t>(object + ue::Layout::ObjName) == name;
    }
};

class Engine {
public:
    int flags = 0;

    bool Resolve(std::string* why) {
        if (!pe::Ready()) { *why = "ProcessEvent 不可用"; return false; }
        passiveCls_ = ue::FindClass("BP_PlayerDeflectPassive_C");
        deflectCls_ = ue::FindClass("DeflectComponent");
        durationCls_ = ue::FindClass("LGAAction_DurationBulletWithDeflectBase");
        projCls_ = ue::FindClass("GAA_DelayDeflectProjectileBullet_C");
        burstCls_ = ue::FindClass("LGAAction_BulletBurstWithDeflectBase");
        auto* statics = ue::FindClass("GameplayStatics");
        auto* chr = ue::FindClass("PBZCharacter");
        for(auto [object,name]:std::initializer_list<std::pair<ue::UObject*,const char*>>{
                {passiveCls_,"BP_PlayerDeflectPassive_C"},{deflectCls_,"DeflectComponent"},
                {durationCls_,"LGAAction_DurationBulletWithDeflectBase"},{statics,"GameplayStatics"},{chr,"PBZCharacter"}})
            if(!object){*why=std::string("等待战斗资源：")+name;return false;}
        offComp_ = ue::PropOffset(passiveCls_, "Deflect Component");
        offFrames_ = ue::PropOffset(deflectCls_, "ReceiveFrames");
        offPerfect_ = ue::PropOffset(passiveCls_, "Perfect Window");
        offNormal_ = ue::PropOffset(passiveCls_, "Normal Window");
        offClick_ = ue::PropOffset(passiveCls_, "Last Deflect Click");
        offCounter_ = ue::PropOffset(passiveCls_, "Current Perfect Window Counter");
        offDurType_ = ue::PropOffset(durationCls_, "DeflectBulletType");
        offProjType_ = projCls_ ? ue::PropOffset(projCls_, "Deflect Bullet Type") : -1;
        offBurstType_ = burstCls_ ? ue::PropOffset(burstCls_, "DeflectType") : -1;
        offAsc_ = ue::PropOffset(chr, "AbilitySystemComponent");
        fnRelease_ = pe::FindFunction(passiveCls_, "On Stance Key Release");
        fnTime_ = pe::FindFunction(statics, "GetTimeSeconds");
        fnKeyDown_ = pe::FindFunction(passiveCls_, "On Stance Key Down");
        fnGiveWindow_ = pe::FindFunction(passiveCls_, "Give Perfect Window");
        fnActivateStance_ = pe::FindFunction(passiveCls_, "Activate Stance Ability");
        fnAvatar_ = pe::FindFunction(passiveCls_, "GetAvatarActorFromActorInfo");
        fnLocal_ = pe::FindFunction(ue::FindClass("Pawn"), "IsLocallyControlled");
        offStanceTag_ = ue::PropOffset(passiveCls_, "Stance Ability Tag");
        sgameCls_ = ue::FindClass("SGameInputComponent");
        aicCls_ = ue::FindClass("AbilityInputComponent");
        fnForceTag_ = aicCls_ ? pe::FindFunction(aicCls_, "ForceActivateExternalActionAbilityByTag") : nullptr;
        fnForceClass_ = aicCls_ ? pe::FindFunction(aicCls_,"ForceActivateExternalActionAbilityByClass") : nullptr;
        perfectBaseCls_ = ue::FindClass("BP_PlayerPerfectDodge_Base_C");
        fnTrigger_ = sgameCls_ ? pe::FindFunction(sgameCls_, "ForceTrigger") : nullptr;
        offAicAsc_ = aicCls_ ? ue::PropOffset(aicCls_, "AbilitySystem") : -1;
        offSgameAsc_ = sgameCls_ ? ue::PropOffset(sgameCls_, "AbilitySystem") : -1;
        dodgePassiveCls_ = ue::FindClass("BP_PerfectDodgePassive_C");
        ue::ForEach([&](int, ue::UObject* o) {
            if (ue::ObjName(o) == "Default__GameplayStatics") { staticsCdo_ = o; return false; }
            return true;
        });
        for(auto [offset,name]:std::initializer_list<std::pair<int,const char*>>{
                {offComp_,"Deflect Component"},{offFrames_,"ReceiveFrames"},{offPerfect_,"Perfect Window"},
                {offDurType_,"DeflectBulletType"},{offAsc_,"AbilitySystemComponent"},{offStanceTag_,"Stance Ability Tag"},
                {offAicAsc_,"AbilityInputComponent.AbilitySystem"},{offSgameAsc_,"SGameInputComponent.AbilitySystem"}})
            if(offset<0){*why=std::string("战斗接口不匹配：")+name;return false;}
        for(auto [function,name]:std::initializer_list<std::pair<ue::UObject*,const char*>>{
                {fnAvatar_,"GetAvatarActorFromActorInfo"},{fnLocal_,"IsLocallyControlled"},
                {fnKeyDown_,"On Stance Key Down"},{fnGiveWindow_,"Give Perfect Window"},
                {fnActivateStance_,"Activate Stance Ability"},{fnRelease_,"On Stance Key Release"},
                {fnForceTag_,"ForceActivateExternalActionAbilityByTag"},{fnTime_,"GetTimeSeconds"}})
            if(!function){*why=std::string("战斗接口不匹配：")+name;return false;}
        if(!staticsCdo_){*why="等待战斗资源：Default__GameplayStatics";return false;}
        return true;
    }

    void Tick(float dt) {
        if (!flags) return;
        // 缓存的被动/玩家失效时才全表重找（全表扫描约几十毫秒，不能频繁做）
        since_ += dt;
        const bool valid = Valid();
        if (!valid && wasValid_) { log::Info("自动应对：玩家组件失效，重新查找"); since_ = 1e9f; }
        wasValid_ = valid;
        if (!valid && since_ > 2.0f) {  // 找不到玩家（菜单 / 加载 / 组件重建）时全表重找最多 2 秒一次
            since_ = 0; FindActors(); if (Valid()) { CollectDodgeClasses(); log::Info("自动应对：已重新找到玩家"); wasValid_ = true; } }
        if (!Valid()) return;
        ScanAbilitiesStep();
        auto* comp = ue::Get<uint8_t*>(passive_ + offComp_);
        if (!comp) return;
        const double now = Now();
        if (!std::isfinite(now) || now <= 0 || now > 1e9) return;
        if (lastNow_ > 0 && now + .1 < lastNow_) ResetDiagnostics();
        UpdateExecution(now);
        if(executionNeedsDiscard_)FlushExecutionRequests();
        if(exec_.blocked){lastNow_=now;gap_=0;DiscardExecutionFrames(now);Threats(now,true);return;}
        // 帧间隔：掉帧时把出手窗口放宽一帧，否则一次长帧（>窗口）会整个跳过某一击
        gap_ = lastNow_ > 0 ? std::clamp(now - lastNow_, 0.0, 0.25) : 0;
        lastNow_ = now;
        // Release before any empty-frame return. Otherwise a consumed last
        // attack leaves the stance held indefinitely.
        if (releaseAt_ > 0 && now >= releaseAt_) ReleaseAutoStance();
        timelineBullets_.clear(); collisionFrames_.clear();
        if (monitoring_) CheckHurt(now);
        UpdateShachi();
        std::vector<Incoming> due;
        std::vector<double> upcoming;  // 本帧所有待应对来袭的命中时刻（判断连击）
        ReceiveSlots slots;
        if (!ReadReceiveSlots(comp + offFrames_, slots)) return;
        for (int i = 0; i < slots.count; ++i) {
            if (!slots.Contains(i)) continue;
            uint8_t elem[0x60];
            if (!ue::Read(slots.data + 0x60 * i, elem, sizeof elem)) continue;
            const int key = *reinterpret_cast<int32_t*>(elem);
            const uint8_t* frame = elem + 8;
            double hit = *reinterpret_cast<const double*>(frame + 0x30);
            const bool windowCounter = hit == kWindowHitTime && WindowCounterFrame(key,
                TypeOf(Copied<int32_t>(frame)),frame[0x38] != 0,
                PerfectClass(Copied<ue::UObject*>(frame+0x40)),hit);
            if (!windowCounter && (!std::isfinite(hit) || hit < now - 2 || hit > now + 2)) continue;
            const auto id = Identity(key, frame, hit);
            if (windowCounter) windowSeen_[id] = now;
            if (!windowCounter && key >= 0 && redLog_ > 0) {  // 诊断：红光帧每帧状态（提前量 / 命中标志 / 反制能力）
                int t = -1;
                if (auto it = types_.find(id); it != types_.end()) t = it->second;
                else t = TypeOf(*reinterpret_cast<const int32_t*>(frame));
                if (t == RedEye) {
                    --redLog_;
                    auto* counter = Copied<ue::UObject*>(frame + 0x40);
                    log::Info("红光帧 {} 提前 {:.3f}s 命中标志={} 反制={}", key, hit - now, int(frame[0x39]), counter ? ue::ObjName(counter) : "无");
                }
            }
            if (frame[0x39]) {  // 已命中：自动应对过的记一笔（诊断：应对了却仍被打中）
                auto pending = pending_.find(id);
                if (key < 0) pending = std::find_if(pending_.begin(), pending_.end(), [&](const auto& x) { return SameMirror(x, id); });
                if (pending != pending_.end()) {
                    const auto actual = *pending;
                    pending_.erase(pending);
                    const bool accepted = accepted_.erase(actual) > 0;
                    ++contact_; if (!accepted) ++notAccepted_;
                    if (logBudget_-- > 0) log::Info("命中标志 帧 {}（{}；标志不等于实际受伤）", std::get<0>(actual), accepted ? "请求已接受" : "尝试未获接受");
                }
                continue;
            }
            if (key < 0) continue;  // mirror is not a second actionable attack
            if (!windowCounter && (!(hit > now - 0.15) || hit > now + 2.0)) continue;  // 过期 / 太远
            if (done_.count(id)) continue;
            // 攻击动作发出后（如弓箭离弦）动作对象可能失效：记住首次读到的类型
            int type = TypeOf(*reinterpret_cast<const int32_t*>(frame));
            if (auto t = types_.find(id); t != types_.end()) type = t->second;
            else if (type >= 0) types_[id] = type;
            else {  // 未知来源类：监测记一笔（找漏网的特效 / 范围攻击），本身不处理
                if (!firstSeen_.count(id)) {
                    firstSeen_.insert(id);
                    Remember(id, key, frame, hit, now, type, 0);
                }
                continue;
            }
            const int act = Decide(type);
            if (windowCounter) {
                if (!firstSeen_.count(id)) {
                    firstSeen_.insert(id);
                    Remember(id,key,frame,now,now,type,act,false);
                    if (act) ++observed_[type];
                    if (logBudget_-- > 0) log::Info("红光原生窗口 帧 {} counter={}（无预计命中时间，仅请求原生反制）",key,ue::ObjName(Copied<ue::UObject*>(frame+0x40)));
                }
                if (!act) Done(id);
                else {
                    Incoming f; f.id=id; memcpy(f.frame.data(),frame,f.frame.size());
                    f.handle=key; f.type=type; f.action=act; f.hit=now; f.windowCounter=true;
                    due.push_back(f);
                }
                continue; // Never learn or apply timestamp shifts for an untimed window.
            }
            // 边打边学：同一种怪的同一类攻击，实际命中常比游戏给的预计时间早 / 晚（快慢刀、飞剑飞行）。按学到的偏差修正
            if (auto sh = shiftById_.find(id); sh != shiftById_.end()) hit -= sh->second;
            else {
                const auto who = AttackerKey(Copied<int32_t>(frame), type);
                const double shift = LearnedShift(who);
                shiftById_[id] = shift; earlyById_[id] = LearnedEarly(who); lateById_[id] = LearnedLate(who); hit -= shift;
            }
            const double early = earlyById_.count(id) ? earlyById_[id] : 0.0;
            const double lead = hit - now;
            if (!firstSeen_.count(id)) {
                firstSeen_.insert(id);
                Remember(id, key, frame, hit, now, type, act);
                if (act && type >= 0 && type < 6) ++observed_[type];
                if (logBudget_-- > 0) log::Info("来袭 {} 类型 {} 提前 {:.2f}s → {}", key, type, lead, act == 1 ? "格挡" : act == 2 ? "闪避" : act == 3 ? "交替" : "不处理");
            }
            // 红蓝光要"接住"：等攻击快打到时才动作，太早会闪跑 / 空举防，没有反制派生。
            if (act) upcoming.push_back(hit);
            const bool parryable = Guardable(type);
            // 先收齐来袭（包括后续连击），再按最终选定的动作决定窗口。
            const double window = std::max(ResponseLead(type, 1, parryable ? early : 0.0, deflectLead_, dodgeLead_, redLead_, gap_),
                                           ResponseLead(type, 2, 0, deflectLead_, dodgeLead_, redLead_, gap_));
            const bool distant = act && lead >= -0.02 && lead <= window && TooFar(Copied<int32_t>(frame)) && !ProjectileNear();
            if (distant && !parryable && type != RedEye) {  // 红光远处出手（甩飞剑）也要完美闪避反制，不能等
                // 发起者远（远处空挥或扔飞剑）：先不动；不再整条作废——有敌方飞行物飞到身边时照常应对
                if (farLogged_.insert(id).second && logBudget_-- > 0) log::Info("来袭 {} 发起者距离过远，先不应对（飞行物靠近时再应对）", key);
            } else if (act && lead >= -0.02 && lead <= window) {
                Incoming f; f.id = id; memcpy(f.frame.data(), frame, f.frame.size());
                f.handle = key; f.type = type; f.action = act; f.hit = hit;
                f.early = parryable ? early : 0.0;
                f.guard = parryable && distant; // 远处飞剑可举住；学习余量本身不强制改变交替比例
                f.volley = parryable && distant;
                f.tail = parryable && lateById_.count(id) ? lateById_[id] : 0.0;
                due.push_back(f);
            } else if (!act) Done(id);
            else if (lead < -0.02) {  // 错过：从没进过出手窗口（长帧 / 本帧在处理别的来袭）
                Done(id);
                if (logBudget_-- > 0) log::Info("错过 帧 {} 类型 {} 命中后 {:.3f}s 才看到，上帧间隔 {:.3f}s", key, type, -lead, gap_);
            }
        }
        std::erase_if(due, [&](Incoming& f) {
            if (NormalType(f.type)) {
                const bool combo = std::any_of(upcoming.begin(), upcoming.end(), [&](double h) { return h > f.hit + 1e-4 && h <= f.hit + kComboGap; });
                auto [it, fresh] = mixChoice_.try_emplace(f.id, 0);
                if (fresh) it->second = NextPercent(); // 每击固定，等待窗口期间不重抽
                f.action = ChooseNormalResponse(f.action, Guardable(f.type), shachiScarce_, combo, ownsStance_, f.guard, it->second, mixDeflect_);
            }
            return f.hit - now > ResponseLead(f.type, f.action, f.early, deflectLead_, dodgeLead_, redLead_, gap_);
        });
        // Hash-slot order is not a threat order. Use a strict ordering (not an
        // epsilon comparator) and never issue competing actions in one tick.
        const double quantum = std::clamp(static_cast<double>(dt), 1.0 / 120, .02);
        std::sort(due.begin(), due.end(), [&](const auto& a, const auto& b) { return ResponseOrder(a, now, quantum) < ResponseOrder(b, now, quantum); });
        // A real sweep may touch before/after its nominal pre-hit point. Let
        // the native collision interval cover its own frames; colors retain
        // priority and never gain authority from the ordinary path.
        const bool timelineResponse = Anticipate(now, std::any_of(due.begin(),due.end(),[](const Incoming& f) { return !NormalType(f.type); }));
        std::erase_if(due,[&](const Incoming& f) { return done_.count(f.id) || collisionFrames_.count(f.id) || (timelineResponse && NormalType(f.type)); });
        // 反制 / 完美闪避后续动作期间不打断——但快打到（≤60ms）时保命优先
        if (!due.empty() && ((!due.front().windowCounter && due.front().hit - now <= kImminent) || !ProtectedResponseRunning())) {
            const auto& f = due.front();
            if (pending_.insert(f.id).second) { ++requested_; ++attempted_[f.type]; }
            for (auto& r : recent_) if (r.id == f.id) ++r.tries;
            const bool deflectable = Guardable(f.type);
            // 普攻三种模式：闪避为主（真空期格挡接）/ 格挡为主（格挡被拒用闪避躲）/
            // 交替：75% 格挡为基准；密集连击优先衔接，杀气低/不可挡时优先闪避。
            const int want = f.action;
            const bool ordinary = NormalType(f.type);
            const bool imminent = f.hit - now <= kImminent;
            urgent_ = imminent && !f.windowCounter;
            bool succeeded = false;
            auto dodge = [&] {
                if (!Dodge(f.handle, f.type, f.frame.data(), f.hit - now, now, f.windowCounter)) return false;
                succeeded = true;
                Done(f.id); accepted_.insert(f.id); ++acceptedCount_; ++admitted_[f.type]; lastNormal_ = 2;
                if (ordinary) ++ordinaryDashes_;
                return true;
            };
            auto deflect = [&] {
                auto prior = stanceAttempts_.find(f.id);
                if (prior != stanceAttempts_.end() && now-prior->second < 0.04) return false;
                stanceAttempts_[f.id] = now;
                const bool ok = Deflect(f.handle, f.frame.data(), f.hit, now);
                if (ok || f.type == BlueEye) Done(f.id);
                // 飞剑连发：实测一次预警后 0.5~0.9 秒内陆续打到好几下，举防举到预计命中后 0.9 秒
                if (ok && ordinary && f.volley) releaseAt_ = std::max(releaseAt_, f.hit + kVolleyHold);
                if (ok && ordinary && f.tail > 0) releaseAt_ = std::max(releaseAt_, f.hit + f.tail);
                if (ok) {
                    accepted_.insert(f.id); ++acceptedCount_; ++admitted_[f.type];
                    lastNormal_ = 1; succeeded = true; if (ordinary) ++ordinaryGuards_;
                }
                return ok;
            };
            if (want == 1) {
                // 格挡没举起来（出招 / 硬直中）且快打到：闪避兜底
                if (!deflect() && ordinary && imminent && dodge() && logBudget_-- > 0) log::Info("格挡被拒改闪避 帧 {}", f.handle);
            } else if (!dodge() && ordinary && imminent && deflectable && !shachiScarce_) {
                if (logBudget_-- > 0) log::Info("闪避被拒改格挡 帧 {}", f.handle);
                if (!deflect() && logBudget_-- > 0) log::Info("应对失败 帧 {}：闪避、格挡都被游戏拒绝（玩家正处于不可取消的动作）", f.handle);
            }
            urgent_ = false;
            // 自愈：连续 3 击在最后时刻仍全部失败，多半是玩家组件 / 能力实例换了（换武器、影之武装切换等）——立即重找
            if (succeeded) failStreak_ = 0;
            else if (!f.windowCounter && imminent && ++failStreak_ >= 3) {
                failStreak_ = 0;
                log::Info("自动应对：连续 3 击应对失败，重新查找玩家组件与能力");
                FindActors();
                if (Valid()) CollectDodgeClasses();
                return;
            }
        }
        Threats(now, !due.empty() || timelineResponse);
        if (learnedDirty_ && now - learnedDirtyAt_ >= 10) { learnedDirtyAt_ = now; SaveLearned(); }
        if (flags & kAutoAttack) AutoAttack(now, upcoming, due.empty());
        if (now >= nextSummary_) {
            nextSummary_ = now + 5;
            log::Info("自动应对统计 请求={} 请求接受={} 接触标志={} 其中未接受={} 补充攻击={}/应对接受{} 受伤记录={}（无帧 {}）", requested_, acceptedCount_, contact_, notAccepted_,
                      threatSeen_, threatDodged_, hurt_, hurtNoFrame_);
            log::Info("自动应对分类 普攻见/请求/接受={}/{}/{} 蓝光={}/{}/{} 红光={}/{}/{} 黑光={}/{}/{}；接受不代表完美", observed_[Normal], attempted_[Normal], admitted_[Normal], observed_[BlueEye], attempted_[BlueEye], admitted_[BlueEye], observed_[RedEye], attempted_[RedEye], admitted_[RedEye], observed_[PowerAttack],attempted_[PowerAttack],admitted_[PowerAttack]);
            log::Info("自动应对普通策略 格挡接受={} 闪避接受={} 杀气偏低={}（含兜底/不可挡；75/25仅为常规交替基准，不代表完美率）", ordinaryGuards_, ordinaryDashes_, shachiScarce_);
        }
        // Expire diagnostics even when every attempt was rejected. Old code
        // only pruned after 512 accepted actions, allowing unbounded growth.
        const auto expired = [&](const AttackId& id) {
            auto it=windowSeen_.find(id);
            return AttackExpired(std::get<2>(id),it==windowSeen_.end() ? -10 : it->second,now);
        };
        std::erase_if(done_, expired); std::erase_if(firstSeen_, expired);
        std::erase_if(pending_, expired); std::erase_if(accepted_, expired);
        std::erase_if(mixChoice_, [&](const auto& entry) { return expired(entry.first); });
        std::erase_if(shiftById_, [&](const auto& entry) { return expired(entry.first); });
        std::erase_if(earlyById_, [&](const auto& entry) { return expired(entry.first); });
        std::erase_if(lateById_, [&](const auto& entry) { return expired(entry.first); });
        std::erase_if(stanceAttempts_, [&](const auto& entry) { return expired(entry.first); });
        std::erase_if(contacts_,[&](const auto& entry) { return now-entry.second.seen > 1 || !entry.second.bullet.Alive(); });
        std::erase_if(learnedIds_, expired);
        std::erase_if(farLogged_, expired);
        std::erase_if(types_, [&](const auto& entry) { return expired(entry.first); });
        std::erase_if(windowSeen_,[&](const auto& entry) { return now-entry.second > 2; });
    }

    // 时机与输入动作可在 %LOCALAPPDATA%\PBZLab\core.ini [auto] 调（开启功能时读取），便于实机校准
    void LoadConfig() {
        wchar_t d[MAX_PATH] = {};
        GetEnvironmentVariableW(L"LOCALAPPDATA", d, MAX_PATH);
        const auto ini = std::wstring(d) + L"\\PBZLab\\core.ini";
        dodgeLead_ = GetPrivateProfileIntW(L"auto", L"dodge_lead_ms", 80, ini.c_str()) / 1000.0;
        redLead_ = GetPrivateProfileIntW(L"auto", L"red_lead_ms", 200, ini.c_str()) / 1000.0;  // 实测：35ms 时游戏已判"太晚"拒绝；从 0.2s 起每帧试，由游戏自己的完美判定决定何时接受
        priority_ = GetPrivateProfileIntW(L"auto", L"dodge_priority", 1000, ini.c_str());
        priority_ = std::clamp(priority_, 1, 1000000);
        deflectLead_ = GetPrivateProfileIntW(L"auto", L"deflect_lead_ms", 45, ini.c_str()) / 1000.0;
        projLead_ = GetPrivateProfileIntW(L"auto", L"projectile_lead_ms", 250, ini.c_str()) / 1000.0;
        mixDeflect_ = std::clamp(static_cast<int>(GetPrivateProfileIntW(L"auto", L"mix_deflect_pct", 85, ini.c_str())), 0, 100);
        if (!(projLead_ > 0 && projLead_ <= 1)) projLead_ = .25;
        if (!(dodgeLead_ > 0 && dodgeLead_ <= .5)) dodgeLead_ = .08;
        if (!(redLead_ > 0 && redLead_ <= .5)) redLead_ = .035;
        if (!(deflectLead_ > 0 && deflectLead_ <= .2)) deflectLead_ = .045;
        wchar_t buf[64];
        GetPrivateProfileStringW(L"auto", L"dodge_input", L"IA_Dodge", buf, 64, ini.c_str());
        const std::string dodgeName = Utf8(buf);
        GetPrivateProfileStringW(L"auto", L"deflect_input", L"IA_Deflect", buf, 64, ini.c_str());
        const std::string deflectName = Utf8(buf);
        iaDodge_ = iaDeflect_ = iaAttack_ = nullptr;
        GetPrivateProfileStringW(L"auto", L"attack_input", L"IA_AttackL_Press", buf, 64, ini.c_str());
        const std::string attackName = Utf8(buf);
        attackGap_ = std::clamp(static_cast<int>(GetPrivateProfileIntW(L"auto", L"attack_interval_ms", 250, ini.c_str())), 80, 1000) / 1000.0;
        attackRange_ = std::clamp(static_cast<int>(GetPrivateProfileIntW(L"auto", L"attack_range_cm", 450, ini.c_str())), 100, 3000);
        auto* iaCls = ue::FindClass("InputAction");
        ue::ForEach([&](int, ue::UObject* o) {
            if (ue::ClassOf(o) != iaCls) return true;
            const auto n = ue::ObjName(o);
            if (n == dodgeName) iaDodge_ = o;
            if (n == deflectName) iaDeflect_ = o;
            if (n == attackName) iaAttack_ = o;
            return !(iaDodge_ && iaDeflect_ && iaAttack_);
        });
        log::Info("自动应对：红光提前 {:.3f}s；闪避 {} 提前 {:.2f}s{}，格挡 {} 提前 {:.3f}s{}", redLead_, dodgeName, dodgeLead_, iaDodge_ ? "" : "（未找到）",
                  deflectName, deflectLead_, iaDeflect_ ? "" : "（未找到）");
    }

private:
    static std::string Utf8(const wchar_t* text) {
        const int bytes = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
        if (bytes <= 1) return {};
        std::string result(bytes, '\0');
        WideCharToMultiByte(CP_UTF8, 0, text, -1, result.data(), bytes, nullptr, nullptr);
        result.pop_back(); return result;
    }
    void Done(const AttackId& id) { done_.insert(id); }

    // 0 = 不处理，1 = 格挡，2 = 闪避
    static constexpr double kImminent = 0.06, kComboGap = 0.45, kVolleyHold = 0.9;
    static constexpr int kUrgentPriority = 1000000;
    bool urgent_ = false;
    int failStreak_ = 0;
    bool wasValid_ = false;
    double protectedSince_ = 0;  // 本次应对已快打到：ForceTag 被拒时用最高优先级重试
    int Decide(int type) const {
        return ResponsePolicy(flags, type);
    }

    int TypeOf(int32_t actionIndex) const {
        if (actionIndex <= 0 || actionIndex >= ue::Num()) return -1;
        auto* a = ue::Obj(actionIndex);
        if (!a) return -1;
        int value = -1;
        if (ue::IsA(a, durationCls_)) value = ue::Get<uint8_t>(a + offDurType_, 255);
        else if (projCls_ && offProjType_ >= 0 && ue::IsA(a, projCls_)) value = ue::Get<uint8_t>(a + offProjType_, 255);
        else if (burstCls_ && offBurstType_ >= 0 && ue::IsA(a, burstCls_)) value = ue::Get<uint8_t>(a + offBurstType_, 255);
        if (value >= CantDeflect && value <= HoldDeflect) return value;
        return -1;  // unknown source is not evidence of a normal attack
    }

    double Now() {
        Params p(fnTime_);
        const int ctx = p.Off("WorldContextObject"), ret = p.Off("ReturnValue");
        if (ctx < 0 || ret < 0) return -1;
        *reinterpret_cast<void**>(p.buf.data() + ctx) = player_;
        pe::Call(staticsCdo_, fnTime_, p.buf.data());
        return *reinterpret_cast<double*>(p.buf.data() + ret);
    }

    bool Valid() const {
        return playerRef_.Alive() && passiveRef_.Alive() && dodgePassiveRef_.Alive() && sgameRef_.Alive() && aicRef_.Alive() &&
            ue::Get<ue::UObject*>(player_ + offAsc_) &&
            ue::Get<ue::UObject*>(aic_ + offAicAsc_) == ue::Get<ue::UObject*>(player_ + offAsc_) &&
            ue::Get<ue::UObject*>(sgame_ + offSgameAsc_) == ue::Get<ue::UObject*>(player_ + offAsc_);
    }

    void FindActors() {
        ReleaseAutoStance(); ResetDiagnostics();
        ownsStance_=false;releaseAt_=0; // Never carry an old actor's input ownership into a new actor.
        ClearExecutionWatch();
        stateComp_ = fnInChild_ = nullptr; // A still-live component can belong to the previous world/player.
        passive_ = nullptr;
        player_ = nullptr;
        dodgePassive_ = nullptr;
        sgame_ = aic_ = nullptr;
        dodgeClasses_.clear(); stanceAbilities_.clear(); protectedAbilities_.clear(); dashAbilities_.clear();
        auto* playerCls = ue::FindClass("PBZCharacter_Player");
        ue::ForEach([&](int, ue::UObject* o) {
            if (ue::Get<uint32_t>(o + 0x08) & 0x30) return true;
            if (ue::ClassOf(o) != passiveCls_) return true;
            Params avatar(fnAvatar_); const int ret = avatar.Off("ReturnValue");
            if (ret < 0) return true;
            pe::Call(o, fnAvatar_, avatar.buf.data());
            auto* pawn = Copied<ue::UObject*>(avatar.buf.data() + ret);
            if (!pawn || !playerCls || !ue::IsA(pawn, playerCls)) return true;
            Params local(fnLocal_); const int lr = local.Off("ReturnValue");
            if (lr < 0) return true;
            pe::Call(pawn, fnLocal_, local.buf.data());
            if (!local.buf[lr]) return true;
            passive_ = o; player_ = pawn; playerCls_ = ue::ClassOf(pawn);
            return false;
        });
        if (player_) {  // 玩家输入组件 = AbilitySystem 指向玩家 ASC 的那个
            auto* asc = ue::Get<ue::UObject*>(player_ + offAsc_);
            ue::ForEach([&](int, ue::UObject* o) {
                if (ue::Get<uint32_t>(o + 0x08) & 0x30) return true;
                const auto c = ue::ClassOf(o);
                const auto outer = ue::Get<ue::UObject*>(o + ue::Layout::ObjOuter);
                if (c == dodgePassiveCls_ && outer == player_) dodgePassive_ = o;
                if (ue::IsA(o, sgameCls_) && ue::Get<ue::UObject*>(o + offSgameAsc_) == asc) sgame_ = o;
                if (ue::IsA(o, aicCls_) && outer == player_ && ue::Get<ue::UObject*>(o + offAicAsc_) == asc) aic_ = o;
                return !(sgame_ && aic_ && dodgePassive_);
            });
            if (!sgame_) log::Info("自动应对：未找到玩家输入组件");
            if (!aic_) log::Info("自动应对：未找到玩家动作输入组件");
        }
        playerRef_ = ObjectRef(player_); passiveRef_ = ObjectRef(passive_); dodgePassiveRef_ = ObjectRef(dodgePassive_);
        sgameRef_ = ObjectRef(sgame_); aicRef_ = ObjectRef(aic_);
    }

    // 快打到时普通优先级被拒（玩家正在出招的不可取消段）：用最高优先级再强制一次
    bool ForceTag(const std::array<uint32_t, 2>& tag, ue::UObject* input) {
        if (ForceTagAt(tag, input, priority_)) return true;
        return urgent_ && ForceTagAt(tag, input, kUrgentPriority);
    }
    bool ForceTagAt(const std::array<uint32_t, 2>& tag, ue::UObject* input, int priority) {
        if (Blocked()) return false;
        if (!aic_ || !fnForceTag_ || !tag[0] || !input) return false;
        Params p(fnForceTag_);
        const int t = p.Off("AbilityTag"), ia = p.Off("InputAction"), pr = p.Off("ActionPriority"), r = p.Off("ReturnValue");
        if (t < 0 || ia < 0 || pr < 0 || r < 0) return false;
        memcpy(p.buf.data() + t, tag.data(), sizeof tag);
        memcpy(p.buf.data() + ia, &input, sizeof input);
        memcpy(p.buf.data() + pr, &priority, sizeof priority);
        pe::Call(aic_, fnForceTag_, p.buf.data());
        return p.buf[r] != 0;
    }
    bool PerfectClass(ue::UObject* cls) {
        if (!perfectBaseCls_ || !ObjectRef(cls).Alive() || !ue::ClassName(cls).ends_with("Class")) return false;
        for (int depth=0;cls && depth<32;++depth) {
            if (cls==perfectBaseCls_) return true;
            cls=ue::Get<ue::UObject*>(cls+ue::Layout::StructSuper);
        }
        return false;
    }
    bool ForceCounterClass(ue::UObject* cls) {
        if (Blocked()) return false;
        if (!PerfectClass(cls) || !fnForceClass_ || !aicRef_.Alive() || !iaDodge_) return false;
        Params p(fnForceClass_);
        const int c=p.Off("Class"),input=p.Off("InputAction"),priority=p.Off("ActionPriority"),ret=p.Off("ReturnValue");
        if (c<0 || input<0 || priority<0 || ret<0) return false;
        memcpy(p.buf.data()+c,&cls,sizeof cls); memcpy(p.buf.data()+input,&iaDodge_,sizeof iaDodge_);
        for (int value : {priority_,kUrgentPriority}) {
            if(Blocked())return false;
            memcpy(p.buf.data()+priority,&value,sizeof value);
            pe::Call(aic_,fnForceClass_,p.buf.data());
            if (p.buf[ret]) {
                // The counter may be granted between incremental scans. Track
                // its native running timeline without a full object-table scan.
                auto* asc=ue::Get<ue::UObject*>(player_+offAsc_);
                auto* tasks=asc && offKnownTasks_>=0 ? ue::Get<uint8_t*>(asc+offKnownTasks_) : nullptr;
                const int count=asc && offKnownTasks_>=0 ? ue::Get<int32_t>(asc+offKnownTasks_+8,-1) : -1;
                const int capacity=asc && offKnownTasks_>=0 ? ue::Get<int32_t>(asc+offKnownTasks_+12,-1) : -1;
                if (tasks && count>=0 && count<=256 && capacity>=count && capacity<=4096 && offTaskAbility_>=0) for (int i=0;i<count;++i) {
                    auto* task=ue::Get<ue::UObject*>(tasks+8*i);
                    if (!ObjectRef(task).Alive() || !timelineTaskCls_ || !ue::IsA(task,timelineTaskCls_)) continue;
                    auto* ability=ue::Get<ue::UObject*>(task+offTaskAbility_);
                    if (ObjectRef(ability).Alive() && ue::IsA(ability,cls) && ue::Get<ue::UObject*>(ability+ue::Layout::ObjOuter)==player_)
                        counterRunning_=ObjectRef(ability);
                }
                return true;
            }
            if (!urgent_) break;
        }
        return false;
    }

    bool StanceRunning() {
        for (const auto& ref : stanceAbilities_) {
            if (!ref.Alive()) continue;
            auto* ability = ref.object;
            if (ue::Get<ue::UObject*>(ability + ue::Layout::ObjOuter) != player_) continue;
            auto* fn = pe::FindFunction(ue::ClassOf(ability), "IsAbilityRunning");
            if (!fn) continue;
            Params p(fn); const int ret = p.Off("ReturnValue");
            if (ret < 0) continue;
            pe::Call(ability, fn, p.buf.data());
            if (p.buf[ret]) return true;
        }
        return false;
    }

    bool Running(const ObjectRef& ref) {
        if (!ref.Alive()) return false;
        auto* ability = ref.object;
        if (ue::Get<ue::UObject*>(ability + ue::Layout::ObjOuter) != player_ && !ExecutionOwned(ability)) return false;
        auto* fn = pe::FindFunction(ue::ClassOf(ability), "IsAbilityRunning");
        if (!fn) return false;
        Params p(fn); const int ret = p.Off("ReturnValue");
        if (ret < 0) return false;
        pe::Call(ability, fn, p.buf.data()); return p.buf[ret] != 0;
    }
    bool DashBlocksResponse() {
        for (const auto& ref : dashAbilities_) {
            if (!Running(ref)) continue;
            const int phase=ue::PropOffset(ref.cls,"Action Phase");
            const bool cancellable=phase>=0 && ue::Name(ue::Get<uint32_t>(ref.object+phase))=="CharacterState.Action.Phase.Cancel";
            if (pbz::DashBlocksResponse(true,cancellable)) return true;
        }
        return false;
    }
    bool ProtectedResponseRunning() {
        // Preserve the game's own counter/perfect-dodge continuation instead
        // of cancelling it with the next normal attack's forced response.
        // 连续挡超过 1.5 秒视为状态卡住（能力没正常结束），不再理会，免得之后的攻击都只能在最后 60ms 才应对
        const auto protect = [&](const ObjectRef& ability) {
            if (!Running(ability)) return false;
            const int off = ue::PropOffset(ability.cls, "Action Phase");
            if (off >= 0 && ue::Name(ue::Get<uint32_t>(ability.object + off)) == "CharacterState.Action.Phase.Cancel") return false;
            if (!protectedSince_) protectedSince_ = lastNow_;
            return lastNow_ - protectedSince_ < 1.5;
        };
        if (protect(counterRunning_)) return true;
        for (const auto& ability : protectedAbilities_) if (protect(ability)) return true;
        protectedSince_ = 0;
        return false;
    }

    // Replay the genuine click path, not the randomized novice-assist helper.
    // Give Perfect Window computes the window and broadcasts its update to
    // the stance ability. We never write either timestamp ourselves.
    bool Deflect(int key, const uint8_t* frame, double hit, double now) {
        if (Blocked()) return false;
        if (!fnKeyDown_) return false;
        // One native perfect window can receive several close hits. Repeated
        // release/click would punish that window and cancel its continuation.
        if (ownsStance_ && releaseAt_ > now && ue::Get<double>(passive_ + offPerfect_) >= hit && StanceRunning()) {
            releaseAt_ = std::max(releaseAt_, hit + .06);
            if (logBudget_-- > 0) log::Info("沿用原生格挡窗 帧 {} hit={:.6f}", key, hit);
            return true;
        }
        if (ownsStance_) ReleaseAutoStance();
        pe::Call(passive_, fnKeyDown_, nullptr);
        if(Blocked())return false;
        ownsStance_ = !(GetAsyncKeyState(VK_RBUTTON) & 0x8000);
        pe::Call(passive_, fnGiveWindow_, nullptr);
        if(Blocked())return false;
        Params activation(fnActivateStance_);
        pe::Call(passive_, fnActivateStance_, activation.buf.data());
        bool running = StanceRunning();
        if (!running) {
            std::array<uint32_t, 2> tag{};
            if (ue::Read(passive_ + offStanceTag_, tag.data(), sizeof tag)) ForceTag(tag, iaDeflect_);
            running = StanceRunning();
        }
        releaseAt_ = std::max(releaseAt_, hit + 0.06);
        probeUntil_ = now + 0.4;
        if (logBudget_-- > 0) log::Info("自动格挡请求 帧 {} hit={:.6f}（提前 {:.3f}s，原生窗余 {:.3f}s，stance={}；完美/反制另验）", key, hit, hit - now, ue::Get<double>(passive_ + offPerfect_) - now, running);
        (void)frame;
        return running;
    }

    // 闪避：AbilityInputComponent.ForceActivateExternalActionAbilityByClass(完美闪避, IA_Dodge, 优先级) ——
    // 游戏自己"打断当前动作并激活"的接口（ForceTrigger 会进输入缓冲，出招时要等收招才闪）。
    // 是否完美仍由完美闪避能力自己的 K2_CanActivateAbility 判定（不强制，强制会出现无反制的无敌状态）。返回是否激活
    bool Dodge(int key, int type, const uint8_t* frame, double lead, double now, bool counterOnly = false) {
        if(Blocked())return false;
        if (!Decide(type) || type == BlueEye) return false;
        if (!aic_ || !fnForceTag_) return false;
        if (dodgeClasses_.empty()) CollectDodgeClasses();
        probeUntil_ = now + 0.05;
        // Ordinary Shift uses BP_PlayerDodge_Base (AdvancedDash). Its
        // OnEvadeDamage(..., Perfect) owns the afterimage/perfect judgement.
        // Counter-style PerfectDodge is a different ability used for red hits.
        auto tag = type == RedEye ? perfectDodgeTag_ : dashTag_;
        if (type != RedEye && frame) SideStep(ActionOwner(Copied<int32_t>(frame)));
        // Only a genuine, still actionable red ReceiveFrame can select its
        // native counter class. Never synthesize a frame/grant, write its
        // Current Frame or bypass the ability's CanActivate checks.
        auto* counter = frame && key>=0 && type==RedEye ? Copied<ue::UObject*>(frame+0x40) : nullptr;
        const bool specific=counter && ForceCounterClass(counter);
        bool accepted = specific || (!counterOnly && ForceTag(tag, iaDodge_));
        // 红光反制被原生资格拒绝且接近命中：尝试普通Dash；接受仍不代表反制/免伤。
        if (!accepted && !counterOnly && type == RedEye && lead <= kImminent && dashTag_[0]) { tag = dashTag_; accepted = ForceTag(tag, iaDodge_); }
        if (accepted && logBudget_-- > 0) {
            if (counterOnly) log::Info("红光窗口专用反制请求已接受 帧 {} counter={}（原生资格通过；接住回扔另验）",key,ue::ObjName(counter));
            else log::Info("自动闪避请求已接受 type={} tag={} 帧 {} hit={:.6f}（提前 {:.3f}s，counter={} 指定能力={}；完美另验）",
                type, ue::Name(tag[0]), key, frame ? Copied<double>(frame + 0x30) : now+lead, lead, counter && ObjectRef(counter).Alive() ? ue::ObjName(counter) : "无",specific);
        }
        return accepted;
    }

    // 玩家身上 GA_Hun_<武器>_PerfectDodge_C 的实例类（不含对飞行物版本）
    // 玩家能力清单（格挡 / 完美闪避类 / 动作标签）。换武器、换装备会授予新的能力实例，所以：
    // 开启时完整收集一次，之后每帧增量扫描一段对象表，扫完一轮就用新清单替换（不卡顿，换装约 2 秒内生效）
    struct AbilityLists {
        std::vector<ue::UObject*> stance, protect, dodge, dashRunning, execution;
        std::array<uint32_t, 2> dash{}, perfect{};
    };
    bool IsExecutionAbility(ue::UObject* ability) const {
        if(!ObjectRef(ability).Alive())return false;
        if(!gameplayAbilityCls_||!ue::IsA(ability,gameplayAbilityCls_))return false;
        auto* cls=ue::ClassOf(ability);
        for(int depth=0;cls&&depth<64;++depth,cls=ue::Get<ue::UObject*>(cls+ue::Layout::StructSuper))
            if(ExecutionBase(ue::ObjName(cls)))return true;
        if(tagsOffset_>=0){
            auto* tags=ue::Get<uint8_t*>(ability+tagsOffset_);const int count=ue::Get<int32_t>(ability+tagsOffset_+8,-1);
            if(tags&&count>0&&count<=64)for(int i=0;i<count;++i)if(ExecutionTag(ue::Name(ue::Get<uint32_t>(tags+8*i))))return true;
        }
        return false;
    }
    bool ExecutionOwned(ue::UObject* ability) {
        if(!IsExecutionAbility(ability)||!playerRef_.Alive())return false;
        if(ue::Get<ue::UObject*>(ability+ue::Layout::ObjOuter)==player_)return true;
        auto* fn=pe::FindFunction(ue::ClassOf(ability),"GetAvatarActorFromActorInfo");
        if(!fn)return false;
        Params p(fn);const int ret=p.Off("ReturnValue");
        if(ret<0||ret+8>int(p.buf.size()))return false;
        pe::Call(ability,fn,p.buf.data());return Copied<ue::UObject*>(p.buf.data()+ret)==player_;
    }
    void ConsiderAbility(ue::UObject* o, AbilityLists& l) {
        if (!o || (ue::Get<uint32_t>(o + 0x08) & 0x30)) return;
        if(ue::Get<ue::UObject*>(o+ue::Layout::ObjOuter)!=player_){
            if(!gameplayAbilityCls_||!ue::IsA(o,gameplayAbilityCls_))return;
            if(ExecutionOwned(o)){if(l.execution.size()<64)l.execution.push_back(o);WatchExecutionAbility(o);}
            return;
        }
        const auto cn = ue::ClassName(o);
        if(IsExecutionAbility(o)){
            if(l.execution.size()<64)l.execution.push_back(o);
            WatchExecutionAbility(o);
        }
        // Frame-granted counters (grenade catches etc.) do not necessarily
        // end in _PerfectDodge_C. Preserve their native continuation too.
        if (PerfectClass(ue::ClassOf(o))) l.protect.emplace_back(o);
        if (!cn.starts_with("GA_Hun_") && cn != "BP_Hun_PerfectDodge_ForProjectile_C") return;
        if (cn.find("_Stance") != std::string::npos) l.stance.emplace_back(o);  // 新武器为 _Stance_Normal_C
        if ((cn.starts_with("GA_Hun_BEx_") || cn.starts_with("GA_Hun_BDEx_") || cn == "BP_Hun_PerfectDodge_ForProjectile_C" ||
            cn.ends_with("_PerfectDodge_C")) && std::find(l.protect.begin(),l.protect.end(),o)==l.protect.end()) l.protect.emplace_back(o);
        if (cn.ends_with("_PerfectDodge_C") && std::find(l.dodge.begin(), l.dodge.end(), ue::ClassOf(o)) == l.dodge.end()) l.dodge.push_back(ue::ClassOf(o));
        if (tagsOffset_ >= 0) {
            auto* data = ue::Get<uint8_t*>(o + tagsOffset_);
            const int n = ue::Get<int32_t>(o + tagsOffset_ + 8);
            if (data && n > 0 && n < 64) for (int i = 0; i < n; ++i) {
                std::array<uint32_t, 2> tag{};
                if (!ue::Read(data + 8 * i, tag.data(), sizeof tag)) continue;
                const auto name = ue::Name(tag[0]);
                if (name == "Ability.Action.Hun.Dash") { l.dash = tag; l.dashRunning.push_back(o); }
                if (name == "Ability.Action.Hun.PerfectDodge") l.perfect = tag;
            }
        }
    }
    void CommitAbilities(AbilityLists& l, bool log) {
        const bool changed = l.stance.size() != stanceAbilities_.size() || l.dodge != dodgeClasses_ || l.perfect != perfectDodgeTag_ || l.dash != dashTag_;
        stanceAbilities_.clear(); protectedAbilities_.clear();
        executionAbilities_.clear();for(auto* o:l.execution)executionAbilities_.emplace_back(o);
        for (auto* o : l.stance) stanceAbilities_.emplace_back(o);
        for (auto* o : l.protect) protectedAbilities_.emplace_back(o);
        dodgeClasses_ = l.dodge;
        dashAbilities_.clear();
        for (auto* o : l.dashRunning) dashAbilities_.emplace_back(o);
        if (l.dash[0]) dashTag_ = l.dash;
        if (l.perfect[0]) perfectDodgeTag_ = l.perfect;
        if (log || changed)
            log::Info("自动应对能力：反制闪避类={} stance={}，DashTag={} PerfectTag={}{}", dodgeClasses_.size(), stanceAbilities_.size(),
                      dashTag_[0] != 0, perfectDodgeTag_[0] != 0, changed && !log ? "（换装后更新）" : "");
    }
    void CollectDodgeClasses() {
        gameplayAbilityCls_=ue::FindClass("GameplayAbility");
        WatchExecutionInput();
        tagsOffset_ = ue::PropOffset(ue::FindClass("GameplayAbility"), "AbilityTags");
        AbilityLists l;
        ue::ForEach([&](int, ue::UObject* o) { ConsiderAbility(o, l); return true; });
        CommitAbilities(l, true);
        scanCursor_ = 0; building_ = {};
    }
    void ScanAbilitiesStep() {
        if (!player_) return;
        ue::UObject* batch[1024];
        const int n = ue::ObjBatch(scanCursor_, 1024, batch);
        for (int i = 0; i < n; ++i) { ConsiderAbility(batch[i], building_); ConsiderInitFn(batch[i]); ConsiderContactFn(batch[i]); }
        scanCursor_ += 1024;
        if (scanCursor_ < ue::Num()) return;
        scanCursor_ = 0;
        CommitAbilities(building_, false);
        building_ = {};
    }

    // ---- 全来源监测：每条来袭帧（含未知来源）+ 玩家每次受伤（PBZCharacter.OnDamage）对照 ----
    struct Seen {
        AttackId id;
        int key = 0, type = -1, act = 0, tries = 0;
        double hit = 0, seenAt = 0;
        std::string source, attacker;
        bool timed = true;
        ObjectRef action;
    };
    std::vector<Seen> recent_;
    int monitorBudget_ = 800, offSetBase_ = -1, offHealth_ = -1, hurt_ = 0, hurtNoFrame_ = 0, marks_ = 0;
    float lastHp_ = -1;
    double lastMark_ = 0;
    bool monitoring_ = false;

    static std::string ClassOfIndex(int32_t index) {
        if (index <= 0 || index >= ue::Num()) return "失效";
        auto* o = ue::Obj(index);
        return o ? ue::ClassName(o) : "失效";
    }
    void Remember(const AttackId& id, int key, const uint8_t* frame, double hit, double now, int type, int act, bool timed = true) {
        Seen s;
        s.id = id; s.key = key; s.type = type; s.act = act; s.hit = hit; s.seenAt = now;
        s.timed = timed;
        s.source = ClassOfIndex(Copied<int32_t>(frame));
        const int sourceIndex = Copied<int32_t>(frame);
        s.action = ObjectRef(sourceIndex > 0 && sourceIndex < ue::Num() ? ue::Obj(sourceIndex) : nullptr);
        s.attacker = AttackerKey(Copied<int32_t>(frame), type);
        if (monitorBudget_-- > 0)
            log::Info("监测 来袭帧 {} 来源 {} 类型 {} 提前 {:.2f}s → {}", key, s.source, type, hit - now,
                      act == 1 ? "格挡" : act == 2 ? "闪避" : act == 3 ? "交替" : type < 0 ? "未知来源，未处理" : "不处理");
        recent_.push_back(std::move(s));
        std::erase_if(recent_, [&](const Seen& x) { return x.hit < now - 3; });
    }
    // 受伤检测：OnDamage 不经过 ProcessEvent（实机观察不到），改为每帧比较玩家 Health（当前值 / 基础值取小）
    float Health() {
        if (offSetBase_ < 0 || offHealth_ < 0) return -1;
        auto* set = ue::Get<uint8_t*>(player_ + offSetBase_);
        if (!set) return -1;
        return std::min(ue::Get<float>(set + offHealth_ + 0x08, -1), ue::Get<float>(set + offHealth_ + 0x0C, -1));
    }
    bool shachiScarce_ = false;
    int offShachi_ = -1, offShachiMax_ = -1;
    void UpdateShachi() {
        auto* set = offSetBase_ >= 0 ? ue::Get<uint8_t*>(player_+offSetBase_) : nullptr;
        if (!set || offShachi_ < 0 || offShachiMax_ < 0) { shachiScarce_ = false; return; }
        const float value = ue::Get<float>(set+offShachi_+0x0c, -1), maximum = ue::Get<float>(set+offShachiMax_+0x0c, -1);
        const bool scarce = ScarceShachi(shachiScarce_, value, maximum);
        if (scarce != shachiScarce_ && (flags & kNormalFlags)) {
            log::Info("自动应对 杀气 {:.0f}/{:.0f}：普通模式{}", value, maximum, scarce ? "改用闪避，暂不格挡兜底" : "恢复设定的格挡/闪避比例");
            if (scarce) ReleaseAutoStance();
        }
        shachiScarce_ = scarce;
    }
    void CheckHurt(double now) {
        const float hp = Health();
        if (hp >= 0) {
            if (lastHp_ > 0 && hp < lastHp_ - 0.5f) { OnHurt(lastHp_ - hp, now); lastHurtAt_ = now; }
            lastHp_ = hp;
        }
        // 开着无限血量 / 半神时血量不掉，看不出挨打：再看"受击硬直"状态（CharacterState.Hitreact）的进入沿，算一次受击
        const bool reacting = InHitReact();
        if (reacting && !wasReacting_ && now - lastHurtAt_ > 0.3) { OnHurt(0, now); lastHurtAt_ = now; }
        wasReacting_ = reacting;
    }
    ExecGate exec_;
    ObjectRef executionRunning_;
    ue::UObject* gameplayAbilityCls_=nullptr;
    std::vector<ObjectRef> executionAbilities_;
    std::vector<int> executionTokens_;
    std::vector<int> executionNativeTokens_;
    std::set<ue::UObject*> executionFunctions_;
    double executionScanAt_=-1;
    double executionTagRetryAt_=0,hitReactTagRetryAt_=0;
    uint64_t executionImmortal_=0,executionState_=0,hitReactState_=0;
    bool executionNeedsDiscard_=false;
    bool InState(uint64_t tag,bool pending=false) {
        if (!tag || !player_) return false;
        if (!Live(stateComp_)||!ObjectRef(fnInChild_).Alive()) {
            stateComp_ = nullptr;
            if (auto* get = pe::FindFunction(ue::ClassOf(player_), "GetActorStateComponent")) {
                Params p(get);const int ret=p.Off("ReturnValue");
                if(ret>=0&&ret+8<=int(p.buf.size())){pe::Call(player_,get,p.buf.data());std::memcpy(&stateComp_,p.buf.data()+ret,8);}
            }
            fnInChild_ = stateComp_ ? pe::FindFunction(ue::ClassOf(stateComp_), "InChildState") : nullptr;
        }
        if (!stateComp_ || !fnInChild_) return false;
        if(ue::Get<ue::UObject*>(stateComp_+ue::Layout::ObjOuter)!=player_)return false;
        auto* fn=pending?pe::FindFunction(ue::ClassOf(stateComp_),"InPendingState"):fnInChild_;
        if(!fn)return false;
        Params p(fn);const int input=p.Off(pending?"StateTag":"ParentStateTag"),ret=p.Off("ReturnValue");
        if(input<0||ret<0||input+8>int(p.buf.size())||ret>=int(p.buf.size()))return false;
        std::memcpy(p.buf.data()+input,&tag,8);pe::Call(stateComp_,fn,p.buf.data());return p.buf[ret]!=0;
    }
    void ExecutionEdge(int edge) {
        if(edge>0){
            // Forget our own obsolete requests, never issue a release into a
            // running cinematic, and never clear the player's native input.
            // Preserve ownership so our old held stance can be released
            // after the cinematic; no key release is sent during execution.
            urgent_=false;failStreak_=0;
            executionNeedsDiscard_=true;
            log::Info("自动应对：处决优先，暂停闪避 / 格挡 / 攻击");
        }
        if(edge<0)log::Info("自动应对：处决保护结束，恢复（期间拦下 {} 次出手请求）",exec_.suppressed);
    }
    void FlushExecutionRequests(){
        // Only the frame entry clears containers: an observer may interrupt
        // a traversal of these containers in the current native call stack.
        pending_.clear();accepted_.clear();contacts_.clear();recent_.clear();
        timelineWatches_.clear();timelineBullets_.clear();collisionFrames_.clear();executionNeedsDiscard_=false;
    }
    void PulseExecution(double duration){
        const double now=Now();if(std::isfinite(now)&&now>0)ExecutionEdge(exec_.Pulse(now,duration));
    }
    void ClearExecutionWatch(){
        for(int token:executionTokens_)pe::Unobserve(token);
        for(int token:executionNativeTokens_)nx::Unobserve(token);
        executionNativeTokens_.clear();
        executionTokens_.clear();executionFunctions_.clear();executionAbilities_.clear();executionRunning_=ObjectRef();executionScanAt_=-1;
    }
    void WatchExecutionAbility(ue::UObject* ability){
        if(!flags||!playerRef_.Alive())return;
        for(const char* name:{"K2_ActivateAbility","K2_OnEndAbility"}){
            auto* fn=pe::FindFunction(ue::ClassOf(ability),name);
            if(!fn||!executionFunctions_.insert(fn).second)continue;
            executionTokens_.push_back(pe::Observe(fn,[this](ue::UObject* object,void*){
                if(!flags||GetCurrentThreadId()!=observerThread_||!ExecutionOwned(object))return;
                // A shared parent event can serve non-execution abilities.
                if(!IsExecutionAbility(object))return;
                executionRunning_=ObjectRef(object);PulseExecution(ExecGate::kGrace);
            }));
        }
    }
    void WatchExecutionInput(){
        auto* cls=ue::FindClass("ExecutionComponent");if(!cls)return;
        for(const char* name:{"PerformExecution","PlayExecution","PlayExactExecution"}){
            auto* fn=pe::FindFunction(cls,name);if(!fn||!executionFunctions_.insert(fn).second)continue;
            const bool request=std::string_view(name)=="PerformExecution";
            executionTokens_.push_back(pe::Observe(fn,[this,cls,request](ue::UObject* object,void*){
                if(!flags||GetCurrentThreadId()!=observerThread_||!playerRef_.Alive()||!ObjectRef(object).Alive()||!ue::IsA(object,cls)||ue::Get<ue::UObject*>(object+ue::Layout::ObjOuter)!=player_)return;
                PulseExecution(request?.18:ExecGate::kGrace);
            }));
            // Puerts/script native dispatch bypasses ProcessEvent. Observe the
            // real UFunction execution too, preserving every native call/result.
            std::string why;
            const int token=nx::Observe(fn,[this,cls,request](ue::UObject* object){
                if(!flags||GetCurrentThreadId()!=observerThread_||!playerRef_.Alive()||!ObjectRef(object).Alive()||!ue::IsA(object,cls)||ue::Get<ue::UObject*>(object+ue::Layout::ObjOuter)!=player_)return;
                PulseExecution(request?.18:ExecGate::kGrace);
            },&why);
            if(token)executionNativeTokens_.push_back(token);
            else log::Info("处决原生观察暂不可用 {}：{}",name,why);
        }
    }
    void UpdateExecution(double now) {
        if((!executionImmortal_||!executionState_)&&now>=executionTagRetryAt_){
            executionTagRetryAt_=now+1;
            if(!executionImmortal_)executionImmortal_=inv::ToFName("CharacterState.Immortal.Execution");
            if(!executionState_)executionState_=inv::ToFName("CharacterState.Execution");
        }
        bool active=InState(executionImmortal_)||InState(executionState_)||InState(executionState_,true)||InState(executionImmortal_,true)||Running(executionRunning_);
        if(!active&&(executionScanAt_<0||now-executionScanAt_>=.05)){
            executionScanAt_=now;
            for(const auto& ability:executionAbilities_)if(Running(ability)){executionRunning_=ability;active=true;break;}
        }
        ExecutionEdge(exec_.Update(now,active));
    }
    bool Blocked() {
        if(playerRef_.Alive()){
            const double now=Now();if(std::isfinite(now)&&now>0)UpdateExecution(now);
        }
        return exec_.Block();
    }
    void DiscardExecutionFrames(double now){
        auto* component=passiveRef_.Alive()?ue::Get<uint8_t*>(passive_+offComp_):nullptr;ReceiveSlots slots;
        if(!component||!ReadReceiveSlots(component+offFrames_,slots))return;
        for(int i=0;i<slots.count;++i)if(slots.Contains(i)){
            uint8_t row[0x60]{};if(!ue::Read(slots.data+0x60*i,row,sizeof row))continue;
            const int key=Copied<int32_t>(row);const auto* frame=row+8;const double hit=Copied<double>(frame+0x30);
            if(key<0||!RetireExecutionFrame(hit,now))continue;
            Done(Identity(key,frame,hit));
        }
        std::erase_if(done_,[&](const AttackId& id){auto it=windowSeen_.find(id);return AttackExpired(std::get<2>(id),it==windowSeen_.end()?-10:it->second,now);});
    }

    bool InHitReact() {
        if(!hitReactState_&&lastNow_>=hitReactTagRetryAt_){hitReactTagRetryAt_=lastNow_+1;hitReactState_=inv::ToFName("CharacterState.Hitreact");}
        return InState(hitReactState_);
    }
    static bool Live(ue::UObject* o) {
        if (!o) return false;
        const int32_t idx = ue::Get<int32_t>(o + ue::Layout::ObjIndex, -1);
        return idx >= 0 && ue::Obj(idx) == o;
    }
    ue::UObject *stateComp_ = nullptr, *fnInChild_ = nullptr;
    bool wasReacting_ = false;
    double lastHurtAt_ = -10;
    void OnHurt(float amount, double now) {
        std::string frames;
        bool explained = false;
        for (auto& r : recent_)
            if (r.hit > now - 1.0 && r.hit < now + 0.3) {
                if (r.timed) frames += std::format("{}[帧{} {} 类型{} 命中差{:+.2f}s 首见提前{:.2f}s 应对{}次]", frames.empty() ? "" : " ", r.key, r.source, r.type,
                                      r.hit - now, r.hit - r.seenAt, r.tries);
                else frames += std::format("{}[帧{} {} 类型{} 原生无时间窗口 应对{}次]",frames.empty() ? "" : " ",r.key,r.source,r.type,r.tries);
                explained = true;
            }
        ++hurt_;
        if (!explained) ++hurtNoFrame_;
        Learn(now);
        if (!explained) { LearnBullet(now); ExplainNoFrame(now); }
        if (monitorBudget_-- > 0)
            log::Info("监测 {}（第 {} 次，无来袭帧 {} 次）；1 秒内来袭帧：{}", amount > 0 ? std::format("受伤 {:.0f}", amount) : std::string("受击（血量锁定）"), hurt_, hurtNoFrame_, explained ? frames : "无（伤害没经过格挡系统）");
        // 记录器开着时导出受伤前 5 秒的调用序列，用来找特效 / 范围攻击的预警信号
        if (pe::Tracing() && now - lastMark_ > 3 && marks_ < 30) {
            lastMark_ = now; ++marks_;
            pe::Mark(explained ? "hurt-frame" : "hurt-noframe");
        }
    }
    // ---- 无来袭帧的攻击：敌方弹体 / 判定（魔天的波、抓取判定等不写 ReceiveFrames）----
    // 监听所有子弹类的 Received_Initialize（各蓝图子类各有一份 UFunction，增量扫描补挂新加载的），记下敌方生成的子弹；
    // 每帧评估：没有来袭帧覆盖时，飞行弹体按距离 / 接近速度预测命中，近身判定（抓取等）在发起者 5 米内即闪避
    struct Threat {
        ObjectRef bullet;
        ObjectRef ownerRef;
        ue::UObject* owner = nullptr;
        double born = 0, lastTry = -10, timedTry = -10, sampledAt = 0;
        bool actor = false, particle = false, timed = false;  // timed：已按学到的时间闪过
        bool positioned = false, moving = false;
        std::array<double, 3> position{}, velocity{};
        ObjectRef data;
        int type = -1, choice = -1;
        bool framed = false;
        const char* response = "未处理";
        bool area = false;
        double startedAt = 0, areaDue = -1, areaDelay = 0, areaLifetime = 0, areaInterval = 0;
        int shape = -1;
        double radius = 0, halfHeight = 0;
        std::array<double, 3> extent{};
        int tries = 0, accepted = 0;
        std::string cls;
    };
    std::vector<Threat> threats_;
    std::vector<ue::UObject*> initFns_;
    std::vector<int> initTokens_;
    std::vector<ue::UObject*> contactFns_;
    std::vector<int> contactTokens_;
    struct ContactAttempt { ObjectRef bullet; double tried = -10, accepted = -1, seen = 0; bool guarded = false; };
    std::map<ue::UObject*,ContactAttempt> contacts_;
    ue::UObject* fnBreakHit_ = nullptr;
    ue::UObject *contactAttackCls_ = nullptr, *fnContactValid_ = nullptr;
    bool contactLayout_ = false, inContact_ = false;
    ue::UObject *particleCls_ = nullptr, *primitiveCls_ = nullptr, *fnComponentOwner_ = nullptr;
    int offObjectSource_ = -1, offObjectTags_ = -1;
    inline static Engine* nativeEngine_ = nullptr;
    inline static NativeAttack nativeOriginal_ = nullptr;
    abshook::Hook nativeHook_{};
    bool nativeCreated_ = false, nativeEnabled_ = false, nativeTried_ = false;
    int nativeLogBudget_ = 100;
    ObjectRef lastNativeBullet_;
    double lastNativeContact_ = -10;
    uint32_t contactName_ = 0;
    uint32_t initName_ = 0;
    ue::UObject *functionCls_ = nullptr, *bulletObjCls_ = nullptr, *bulletActorCls_ = nullptr, *npcCls_ = nullptr;
    ue::UObject *fnActorLoc_ = nullptr, *fnActorVel_ = nullptr, *fnActorRot_ = nullptr;
    static constexpr double kSweepConfirm = 0.12, kSweepRange = 350;
    int offDestroyed_ = -1, offActorStatus_ = -1, offObjectStatus_ = -1, offActorOwner_ = -1, offObjectOwner_ = -1;
    int offActorData_ = -1, offActorSource_ = -1;
    ue::UObject* overlapDataCls_ = nullptr;
    int offActorTags_ = -1, offDataTags_ = -1, offAbilityTimelines_ = -1, offProjectileData_ = -1;
    int offAreaShape_ = -1, offAreaDelay_ = -1, offAreaLife_ = -1, offAreaInterval_ = -1;
    int offCustomTransform_ = -1, offAttach_ = -1, offShapeType_ = -1, offShapeRadius_ = -1, offShapeHeight_ = -1, offShapeExtent_ = -1;
    uint64_t threatSeen_ = 0, threatDodged_ = 0;

    struct TimelineWatch {
        struct Contact { double tried = -10, accepted = -1; bool guarded = false; };
        ObjectRef task, timeline;
        double cursor = 0, at = 0, seen = 0;
        std::set<std::pair<ue::UObject*,int>> attempted, rangeLogged;
        std::map<std::pair<ue::UObject*,int>,Contact> contacts;
        std::map<std::pair<ue::UObject*,int>,int> choices;
        std::set<ue::UObject*> logged;
    };
    std::map<ue::UObject*, TimelineWatch> timelineWatches_;
    std::set<ue::UObject*> timelineBullets_;
    std::set<AttackId> collisionFrames_;
    ue::UObject *timelineTaskCls_ = nullptr, *fnActionTimeline_ = nullptr, *sweepDataCls_ = nullptr;
    ue::UObject *sweepBulletCls_ = nullptr, *sweepModuleCls_ = nullptr, *sceneCls_ = nullptr;
    ue::UObject *capsuleCls_ = nullptr, *sphereCls_ = nullptr, *boxCls_ = nullptr;
    ue::UObject *attachHelperCdo_ = nullptr, *fnAttachShapeTransform_ = nullptr, *fnComponentTransform_ = nullptr, *fnBulletTransform_ = nullptr;
    int offActionBullets_ = -1, offObjectData_ = -1, offSweepModule_ = -1, offModuleBullet_ = -1;
    int offSweepColliders_ = -1, offSweepShapes_ = -1, offCapsuleRadius_ = -1, offCapsuleHeight_ = -1, offSphereRadius_ = -1, offBoxExtent_ = -1;
    bool collisionLayout_ = false, eventLayout_ = false;
    int offKnownTasks_ = -1, offTaskTimeline_ = -1, offTaskCursor_ = -1, offTaskAbility_ = -1;
    int offTimelineActions_ = -1, offStartBulletTimes_ = -1, offEndBulletTimes_ = -1, offDurationData_ = -1, offInlineData_ = -1;
    int offAreaLocation_ = -1, offAreaOrigin_ = -1;
    int timelineLogBudget_ = 80;

    bool ColoredTags(ue::UObject* object, int offset, int& type) {
        if (!object || offset < 0) return false;
        auto* tags = ue::Get<uint8_t*>(object+offset);
        const int count = ue::Get<int32_t>(object+offset+8, -1);
        if (count < 0 || count > 128 || (count && !tags)) return false;
        for (int i = 0; i < count; ++i) {
            const auto name = ue::Name(ue::Get<uint32_t>(tags+8*i));
            type=MergeAttackColor(type,AttackTagType(name));
        }
        return true;
    }
    int DataAttackType(ue::UObject* ability, ue::UObject* data) {
        if (!ability || !data || offAbilityTimelines_ < 0 || offTimelineActions_ < 0) return -1;
        auto* cls = ue::ClassOf(ability);
        auto* timelines = ue::Get<uint8_t*>(cls+offAbilityTimelines_);
        const int count = ue::Get<int32_t>(cls+offAbilityTimelines_+8, -1);
        if (!timelines || count < 0 || count > 64) return -1;
        int matched = -1;
        for (int i = 0; i < count; ++i) {
            auto* timeline = ue::Get<ue::UObject*>(timelines+8*i);
            if (!timeline) continue;
            auto* events = ue::Get<uint8_t*>(timeline+offTimelineActions_);
            const int n = ue::Get<int32_t>(timeline+offTimelineActions_+8, -1);
            if (!events || n < 0 || n > 512) continue;
            for (int ei = 0; ei < n; ++ei) {
                auto* action = ue::Get<ue::UObject*>(events+64*ei+0x10);
                if (!action) continue;
                ue::UObject* actionData = nullptr;
                if (ue::IsA(action, durationCls_) && offDurationData_ >= 0 && offInlineData_ >= 0)
                    actionData = ue::Get<ue::UObject*>(action+offInlineData_) ? ue::Get<ue::UObject*>(action+offInlineData_) : ue::Get<ue::UObject*>(action+offDurationData_);
                else if (projCls_ && ue::IsA(action, projCls_) && offProjectileData_ >= 0) actionData = ue::Get<ue::UObject*>(action+offProjectileData_);
                if (actionData != data) continue;
                const int type = TypeOf(ue::Get<int32_t>(action+ue::Layout::ObjIndex, -1));
                if (type < 0) continue;
                if (matched >= 0 && matched != type) return -1;
                matched = type;
            }
        }
        return matched;
    }
    bool OrdinaryThreat(Threat& t) {
        // Prediction ownership suppresses competing estimates, never an
        // independently validated native contact with the local player.
        if (!OrdinaryThreatPermission(flags,t.type,t.framed,timelineBullets_.count(t.bullet.object)!=0,inContact_)) return false;
        int tagType = -1;
        if (t.actor) ColoredTags(t.bullet.object, offActorTags_, tagType);
        if (t.data.Alive()) ColoredTags(t.data.object, offDataTags_, tagType);
        if (ColoredType(tagType)) { t.type = MergeAttackColor(t.type,tagType); return false; }
        // 未能归类的无帧攻击交由普通模式保命；不通过红/蓝开关获得权限。
        return true;
    }
    bool RespondableThreat(Threat& t) {
        int color=t.type;
        ColoredTags(t.bullet.object,t.actor?offActorTags_:offObjectTags_,color);
        if(t.data.Alive())ColoredTags(t.data.object,offDataTags_,color);
        t.type=MergeAttackColor(t.type,color);
        if(t.type==PowerAttack)return (flags&kRed)!=0;
        return OrdinaryThreat(t);
    }
    void MatchThreatFrames(double now) {
        if (threats_.empty()) return;
        for (auto& t : threats_) t.framed = false;
        auto* component = ue::Get<uint8_t*>(passive_+offComp_);
        ReceiveSlots slots;
        if (!component || !ReadReceiveSlots(component+offFrames_, slots)) return;
        for (int i = 0; i < slots.count; ++i) {
            if (!slots.Contains(i)) continue;
            uint8_t elem[0x60];
            if (!ue::Read(slots.data+0x60*i, elem, sizeof(elem))) continue;
            auto* bullet = Copied<ue::UObject*>(elem+8+8);
            if (!bullet) continue;
            const int handle = Copied<int32_t>(elem);
            const double hit = Copied<double>(elem+8+0x30);
            int type = TypeOf(Copied<int32_t>(elem+8));
            if (std::isfinite(hit) && hit >= now-2 && hit <= now+2) {
                const auto id = Identity(handle, elem+8, hit);
                if (auto it = types_.find(id); it != types_.end()) type = it->second;
            }
            for (auto& t : threats_) if (t.bullet.object == bullet) {
                // 未知来源（如 ReleaseBullet）在主调度里不会动作，不能因此屏蔽轨迹。
                // -1 镜像/已经接触/过期帧也不是仍在等待处理的攻击。
                t.framed |= FrameSchedulesThreat(handle,type,elem[8+0x39] != 0,hit,now);
                if (type >= 0) t.type = type;
            }
        }
    }
    bool RespondThreat(Threat& t, double hit, double now, const double* center = nullptr, bool combo = false, const double* escape = nullptr) {
        if(t.type==PowerAttack)return false;  // 用户 2026-11-06：去掉黑光闪避
        if (!OrdinaryThreat(t)) return false;
        const bool canGuard = Guardable(t.type);
        const int mode = canGuard ? ResponsePolicy(flags,t.type) : 2;
        if (t.choice < 0) t.choice = NextPercent();
        const int want = ChooseNormalResponse(mode, canGuard, shachiScarce_, combo, ownsStance_, false, t.choice, mixDeflect_);
        // Do not cancel an accepted close guard for an estimated future hit
        // when this projectile has a verified real-contact callback. That
        // callback will still dodge an actually incoming unguardable hit.
        if (!inContact_ && want == 2 && hit > now && ownsStance_ && releaseAt_ > now && t.bullet.Alive()) {
            auto* fn = pe::FindFunction(ue::ClassOf(t.bullet.object),"On Trace Hit");
            if (fn && std::find(contactFns_.begin(),contactFns_.end(),fn) != contactFns_.end() && StanceRunning()) {
                t.response = "等待真实接触（保留举防）"; return false;
            }
        }
        bool ok = false;
        if (want == 1) { t.response = "格挡"; ok = Deflect(-1,nullptr,hit,now); }
        if (!ok) {
            t.response = "闪避";
            if (escape) MoveDirection(escape); else if (center) AwayFrom(center); else SideStep(t.owner);
            ok = ForceTag(dashTag_,iaDodge_);
            if (!ok && want == 2 && canGuard && !shachiScarce_) { t.response = "格挡兜底"; ok = Deflect(-1,nullptr,hit,now); }
        }
        if (ok) { if (t.response == std::string_view("闪避")) ++ordinaryDashes_; else ++ordinaryGuards_; }
        return ok;
    }

    bool Transform(ue::UObject* object, ue::UObject* function, const char* result, std::array<double,12>& output) {
        if (!object || !function) return false;
        Params p(function); const int ret = p.Off(result);
        if (ret < 0) return false;
        pe::Call(object,function,p.buf.data());
        memcpy(output.data(),p.buf.data()+ret,sizeof(output));
        const double norm = output[0]*output[0]+output[1]*output[1]+output[2]*output[2]+output[3]*output[3];
        return std::all_of(output.begin(),output.end(),[](double n){return std::isfinite(n);}) && std::abs(norm-1) < .02;
    }
    bool PreparedHitContains(ue::UObject* action, ue::UObject* attacker, ue::UObject* data, bool area,
                             const double me[3], double center[3], int& shapes) {
        shapes = 0;
        if (!collisionLayout_ || offActionBullets_ < 0 || offObjectData_ < 0 || offObjectOwner_ < 0 || offObjectStatus_ < 0) return false;
        auto* bullets = ue::Get<uint8_t*>(action+offActionBullets_);
        const int n = ue::Get<int32_t>(action+offActionBullets_+8,-1), capacity = ue::Get<int32_t>(action+offActionBullets_+12,-1);
        if (!bullets || n <= 0 || n > 64 || capacity < n || capacity > 1024) return false;
        auto contains = [&](const std::array<double,12>& transform, int shape, double radius, double height, const double extent[3]) {
            ++shapes;
            if (!AreaContains(me,transform.data(),shape,radius,height,extent)) return false;
            std::copy_n(transform.data()+4,3,center);
            return true;
        };
        for (int bi = 0; bi < n; ++bi) {
            // TScriptInterface contains UObject first, interface address second.
            auto* bullet = ue::Get<ue::UObject*>(bullets+16*bi);
            if (!ObjectRef(bullet).Alive() || !ue::IsA(bullet,bulletObjCls_) ||
                ue::Get<ue::UObject*>(bullet+offObjectOwner_) != attacker || ue::Get<ue::UObject*>(bullet+offObjectData_) != data) continue;
            const int status = ue::Get<uint8_t>(bullet+offObjectStatus_,255);
            if (status != 1 && status != 2) continue; // prepared/running only; pooled former hit shapes are irrelevant
            int colored = -1; ColoredTags(data,offDataTags_,colored);
            if (ColoredType(colored)) continue;
            if (area) {
                // This path remains limited to the already validated own-origin area.
                // Attached/multiple area origins still require the dedicated origins API.
                if (!fnBulletTransform_ || offAttach_ < 0 || offAreaShape_ < 0 || offShapeType_ < 0 || offShapeRadius_ < 0 ||
                    offShapeHeight_ < 0 || offShapeExtent_ < 0 || ue::Get<ue::UObject*>(bullet+offAttach_)) continue;
                std::array<double,12> transform{};
                if (!Transform(bullet,fnBulletTransform_,"OutTransform",transform)) continue;
                const int type = ue::Get<uint8_t>(data+offAreaShape_+offShapeType_,255);
                const double radius = ue::Get<float>(data+offAreaShape_+offShapeRadius_);
                const double height = ue::Get<float>(data+offAreaShape_+offShapeHeight_);
                double extent[3]{}; ue::Read(data+offAreaShape_+offShapeExtent_,extent,sizeof(extent));
                if (contains(transform,type,radius,height,extent)) return true;
                continue;
            }
            if (!ue::IsA(bullet,sweepBulletCls_)) continue;
            auto* module = ue::Get<ue::UObject*>(bullet+offSweepModule_);
            if (!ObjectRef(module).Alive() || !ue::IsA(module,sweepModuleCls_) || ue::Get<ue::UObject*>(module+offModuleBullet_) != bullet) continue;
            ReceiveSlots slots;
            if (ReadReceiveSlots(module+offSweepShapes_,slots)) for (int si = 0; si < slots.count; ++si) {
                if (!slots.Contains(si)) continue;
                // TMap<CharacterPoseCacheAttachData(24), ArrayWrapper(16)>: pair40 + sparse/hash8.
                auto* header = slots.data+48*si+24;
                auto* entries = ue::Get<uint8_t*>(header);
                const int count = ue::Get<int32_t>(header+8,-1), max = ue::Get<int32_t>(header+12,-1);
                if (!entries || count <= 0 || count > 64 || max < count || max > 1024) continue;
                for (int ei = 0; ei < count; ++ei) {
                    std::array<uint8_t,0x80> shapeRaw{};
                    if (!ue::Read(entries+0xe0*ei,shapeRaw.data(),shapeRaw.size())) continue;
                    int shape = -1; double radius = 0,height = 0,extent[3]{};
                    if (!DecodeNativeShape(shapeRaw.data(),shape,radius,height,extent)) continue;
                    Params p(fnAttachShapeTransform_); const int input = p.Off("Shape"), result = p.Off("OutTransform");
                    if (input < 0 || result < 0) continue;
                    memcpy(p.buf.data()+input,shapeRaw.data(),shapeRaw.size());
                    pe::Call(attachHelperCdo_,fnAttachShapeTransform_,p.buf.data());
                    std::array<double,12> transform{}; memcpy(transform.data(),p.buf.data()+result,sizeof(transform));
                    if (contains(transform,shape,radius,height,extent)) return true;
                }
            }
            auto* colliders = ue::Get<uint8_t*>(module+offSweepColliders_);
            const int count = ue::Get<int32_t>(module+offSweepColliders_+8,-1), max = ue::Get<int32_t>(module+offSweepColliders_+12,-1);
            if (!colliders || count <= 0 || count > 64 || max < count || max > 1024) continue;
            for (int ci = 0; ci < count; ++ci) {
                const int index = ue::Get<int32_t>(colliders+0x70*ci,-1);
                if (index <= 0 || index >= ue::Num()) continue;
                auto* comp = ue::Obj(index);
                if (!ObjectRef(comp).Alive() || !ue::IsA(comp,sceneCls_)) continue;
                std::array<double,12> transform{};
                if (!Transform(comp,fnComponentTransform_,"ReturnValue",transform)) continue;
                int shape = -1; double radius = 0,height = 0,extent[3]{};
                if (ue::IsA(comp,capsuleCls_) && offCapsuleRadius_ >= 0 && offCapsuleHeight_ >= 0) {
                    shape = 1; radius = ue::Get<float>(comp+offCapsuleRadius_); height = ue::Get<float>(comp+offCapsuleHeight_);
                } else if (ue::IsA(comp,sphereCls_) && offSphereRadius_ >= 0) { shape = 0; radius = ue::Get<float>(comp+offSphereRadius_); }
                else if (ue::IsA(comp,boxCls_) && offBoxExtent_ >= 0) { shape = 2; ue::Read(comp+offBoxExtent_,extent,sizeof(extent)); }
                if (shape >= 0 && contains(transform,shape,radius,height,extent)) return true;
            }
        }
        return false;
    }

    // Only the current native task may own these bullets. Once its paired
    // schedule is known, the old spawn-age/range fallback must not race it.
    void OwnTimelineBullets(ue::UObject* action, ue::UObject* attacker, ue::UObject* data) {
        if (offActionBullets_ < 0 || offObjectOwner_ < 0 || offObjectData_ < 0 || offObjectStatus_ < 0) return;
        auto* bullets = ue::Get<uint8_t*>(action+offActionBullets_);
        const int n = ue::Get<int32_t>(action+offActionBullets_+8,-1), cap = ue::Get<int32_t>(action+offActionBullets_+12,-1);
        if (!bullets || n <= 0 || n > 64 || cap < n || cap > 1024) return;
        for (int i = 0; i < n; ++i) {
            auto* bullet = ue::Get<ue::UObject*>(bullets+16*i);
            if (!ObjectRef(bullet).Alive() || !ue::IsA(bullet,bulletObjCls_) ||
                ue::Get<ue::UObject*>(bullet+offObjectOwner_) != attacker || ue::Get<ue::UObject*>(bullet+offObjectData_) != data) continue;
            const int state = ue::Get<uint8_t>(bullet+offObjectStatus_,255);
            if (state == 1 || state == 2) timelineBullets_.insert(bullet);
        }
    }
    // 普通范围/横扫按原生 Start/EndBulletTimes 全区间检查实际碰撞体。
    // 预警点可能在区间中间，不能让它屏蔽更早接触，也不能举防一次就丢弃后半段。
    bool Anticipate(double now, bool busy) {
        if (!(flags & kNormalFlags) || !timelineTaskCls_ || !eventLayout_ || !collisionLayout_ || !fnActionTimeline_ || offKnownTasks_ < 0 ||
            offTaskTimeline_ < 0 || offTaskCursor_ < 0 || offTaskAbility_ < 0 || offTimelineActions_ < 0 ||
            offStartBulletTimes_ < 0 || offEndBulletTimes_ < 0 || offDurationData_ < 0 || offInlineData_ < 0) return false;
        double me[3];
        if (!Vec(player_, fnActorLoc_, me)) return false;
        bool responded = false;
        // 属性扫描分多帧，刚召唤的角色可能尚未进入完整快照；用已有只读登记补齐首击。
        std::set<ue::UObject*> attackers;
        for (auto* actor : enemies::OwnedActors()) attackers.insert(actor);
        for (const auto& target : attr::Targets()) if (target.who == attr::Who::Enemies && attr::Alive(target)) attackers.insert(target.obj);
        for (const auto& threat : threats_) if (threat.ownerRef.Alive()) attackers.insert(threat.owner);
        for (auto* attacker : attackers) {
            if (!npcCls_ || !ue::IsA(attacker,npcCls_)) continue;
            double at[3];
            if (!Vec(attacker, fnActorLoc_, at) || std::hypot(at[0]-me[0], at[1]-me[1]) > 4000) continue;
            auto* asc = ue::Get<ue::UObject*>(attacker+offAsc_);
            if (!asc) continue;
            auto* tasks = ue::Get<uint8_t*>(asc+offKnownTasks_);
            const int n = ue::Get<int32_t>(asc+offKnownTasks_+8, -1);
            if (!tasks || n < 0 || n > 256) continue;
            for (int ti = 0; ti < n; ++ti) {
                auto* task = ue::Get<ue::UObject*>(tasks+8*ti);
                if (!task || !ue::IsA(task, timelineTaskCls_)) continue;
                auto* timeline = ue::Get<ue::UObject*>(task+offTaskTimeline_);
                auto* ability = ue::Get<ue::UObject*>(task+offTaskAbility_);
                if (!timeline || !ability) continue;
                const double cursor = ue::Get<float>(task+offTaskCursor_, -1);
                if (!std::isfinite(cursor) || cursor < 0 || cursor > 3600) continue;
                auto& watch = timelineWatches_[task];
                const bool fresh = !watch.task.Alive() || !watch.timeline.Alive() || watch.timeline.object != timeline || cursor+0.001 < watch.cursor;
                if (fresh) { watch = TimelineWatch(); watch.task = ObjectRef(task); watch.timeline = ObjectRef(timeline); }
                if (fresh && timelineLogBudget_-- > 0) log::Info("监测 时间轴任务 {} 游标={:.3f}（原生任务已发现，含新召唤首击）",ue::ClassName(ability),cursor);
                const double rate = fresh ? 0 : TimelineRate(cursor, watch.cursor, now-watch.at);
                watch.cursor = cursor; watch.at = now; watch.seen = now;
                if (rate <= 0 || busy || responded) continue;
                auto* events = ue::Get<uint8_t*>(timeline+offTimelineActions_);
                const int count = ue::Get<int32_t>(timeline+offTimelineActions_+8, -1);
                if (!events || count < 0 || count > 512) continue;
                for (int ei = 0; ei < count; ++ei) {
                    uint8_t event[0x40];
                    if (!ue::Read(events+sizeof(event)*ei, event, sizeof(event))) continue;
                    const double start = Copied<float>(event)+Copied<float>(event+8);
                    auto* action = Copied<ue::UObject*>(event+0x10);
                    if (!action || !ue::IsA(action, durationCls_)) continue;
                    const int type = ue::Get<uint8_t>(action+offDurType_,255);
                    if (!NormalType(type) || !ResponsePolicy(flags,type)) continue;
                    auto* data = ue::Get<ue::UObject*>(action+offInlineData_);
                    if (!data) data = ue::Get<ue::UObject*>(action+offDurationData_);
                    const bool area = data && overlapDataCls_ && ue::IsA(data, overlapDataCls_);
                    const bool sweep = data && sweepDataCls_ && ue::IsA(data, sweepDataCls_);
                    if (!area && !sweep) continue; // 移动飞行物仍等实际轨迹，不按蓄力结束猜飞行时间
                    if (area && (offAreaLocation_ < 0 || offAreaOrigin_ < 0 || ue::Get<uint8_t>(data+offAreaLocation_+offAreaOrigin_, 255) != 0)) continue;
                    // Bullet activation is not a delayed explosion's damage
                    // tick. Keep delayed/periodic overlaps on AreaThreat's
                    // native StartDelay/OverlapInterval schedule.
                    if (area && (offAreaDelay_ < 0 || offAreaInterval_ < 0 ||
                        ue::Get<float>(data+offAreaDelay_,-1) != 0 || ue::Get<float>(data+offAreaInterval_,-1) != 0)) continue;
                    int colored = -1;
                    ColoredTags(data,offDataTags_,colored);
                    if (ColoredType(colored)) continue;
                    Params ownTimeline(fnActionTimeline_); const int ownResult = ownTimeline.Off("ReturnValue");
                    if (ownResult < 0) continue;
                    pe::Call(action,fnActionTimeline_,ownTimeline.buf.data());
                    if (Copied<ue::UObject*>(ownTimeline.buf.data()+ownResult) != timeline) {
                        if (watch.logged.insert(action).second && timelineLogBudget_-- > 0) log::Info("监测 时间轴判定跳过 {} 数据={} 动作所属时间轴不匹配",ue::ClassName(ability),ue::ObjName(data));
                        continue;
                    }
                    std::vector<std::pair<float,float>> windows;
                    if (!ReadCollisionWindows(action+offStartBulletTimes_,action+offEndBulletTimes_,windows)) continue;
                    Params avatar(fnAvatar_); const int avatarResult = avatar.Off("ReturnValue");
                    if (avatarResult < 0) continue;
                    pe::Call(ability, fnAvatar_, avatar.buf.data());
                    if (Copied<ue::UObject*>(avatar.buf.data()+avatarResult) != attacker) continue;
                    OwnTimelineBullets(action,attacker,data);
                    if (watch.logged.insert(action).second && monitorBudget_-- > 0)
                        log::Info("监测 时间轴判定候选 {} 数据={} 动作起点={:.3f} 碰撞区间={} 首段={:.3f}~{:.3f} 游标={:.3f}", ue::ClassName(ability), ue::ObjName(data), start, windows.size(), windows.front().first, windows.front().second, cursor);
                    for (int pi = 0; pi < static_cast<int>(windows.size()); ++pi) {
                        const auto identity = std::pair{action,pi};
                        const auto [point,endPoint] = windows[pi];
                        const double begin = TimelineContact(start,point,cursor,rate), end = TimelineContact(start,endPoint,cursor,rate);
                        if (!CollisionWindowDue(begin,end,dodgeLead_+std::min(gap_,0.04))) continue;
                        const double lead = std::max(0.0,begin), impact = start+std::max(double(point),cursor-start);
                        if (lead > kImminent && ProtectedResponseRunning()) continue;
                        double center[3]{}; int shapes = 0;
                        const bool inRange = PreparedHitContains(action,attacker,data,area,me,center,shapes);
                        if (watch.rangeLogged.insert(identity).second) {
                            if (monitorBudget_-- > 0) log::Info("监测 时间轴真实碰撞 {} 数据={} 第{}段 碰撞体={} 玩家距发起者={:.1f}米 范围内={}",
                                ue::ClassName(ability),ue::ObjName(data),pi,shapes,std::hypot(at[0]-me[0],at[1]-me[1])/100,inRange);
                        }
                        // Parsed native geometry also decides a miss: a nearby
                        // nominal pre-hit must not trigger through empty space.
                        if (shapes > 0) for (const auto& r : recent_) if (r.type == type && r.act && r.action.object == action && r.action.Alive() &&
                            FrameWithinCollision(std::get<2>(r.id)/1000000.0,now+begin,now+end)) collisionFrames_.insert(r.id);
                        if (!inRange) continue;
                        auto& contact = watch.contacts[identity];
                        auto coverFrames = [&] {
                            for (const auto& r : recent_) if (r.type == type && r.act && r.action.object == action && r.action.Alive() &&
                                FrameWithinCollision(std::get<2>(r.id)/1000000.0,now+begin,now+end)) Done(r.id);
                        };
                        const bool guarding = ownsStance_ && StanceRunning() && !shachiScarce_;
                        if (contact.guarded && guarding) {
                            // Renew only while the actual shape still touches the player;
                            // leaving it, ending the native interval, or disabling stops renewal.
                            releaseAt_ = std::max(releaseAt_,now+std::min(end,.15)+.04);
                            coverFrames();
                            continue;
                        }
                        const bool dashing = DashBlocksResponse();
                        if (!CollisionRetry(now,contact.tried,contact.accepted,contact.guarded,guarding,dashing)) {
                            if (contact.accepted >= 0) coverFrames();
                            continue;
                        }
                        if (watch.attempted.insert(identity).second) ++threatSeen_;
                        auto [choice,freshChoice] = watch.choices.try_emplace(identity,0);
                        if (freshChoice) choice->second = NextPercent();
                        Threat response;
                        response.type = type; response.owner = attacker; response.ownerRef = ObjectRef(attacker);
                        response.data = ObjectRef(data); response.choice = choice->second;
                        const bool combo = contact.accepted >= 0 || (pi+1 < static_cast<int>(windows.size()) && (windows[pi+1].first-point)/rate <= kComboGap);
                        contact.tried = now;
                        urgent_ = lead <= kImminent;
                        const bool ok = RespondThreat(response,now+lead,now,area ? center : nullptr,combo);
                        urgent_ = false;
                        if (monitorBudget_-- > 0) log::Info("监测 时间轴碰撞应对 {} 数据={} type={} 游标={:.3f} 接触游标={:.3f} 实际速率={:.2f} 提前={:.3f}s 剩余={:.3f}s 碰撞体={} → {}{}",
                            ue::ClassName(ability), ue::ObjName(data),type, cursor, impact, rate, lead,end,shapes,response.response, ok ? "已接受" : "被拒");
                        if (ok) {
                            contact.accepted = now; contact.guarded = response.response != std::string_view("闪避");
                            if (contact.guarded) releaseAt_ = std::max(releaseAt_,now+std::min(end,.15)+.04);
                            coverFrames(); ++threatDodged_; responded = true; break;
                        }
                    }
                    if (responded) break;
                }
            }
        }
        std::erase_if(timelineWatches_, [&](const auto& entry) { return now-entry.second.seen > 1 || !entry.second.task.Alive(); });
        return responded;
    }

    // ---- 边打边学 ----
    // 攻击者键：发起攻击的怪的类名 + 攻击类型（普攻 / 蓝光 / 红光…）。同一种怪同一类攻击的命中偏差基本稳定
    ue::UObject* ActionOwner(int32_t actionIndex) {
        if (!npcCls_ || actionIndex <= 0 || actionIndex >= ue::Num()) return nullptr;
        ue::UObject* owner = ue::Obj(actionIndex);
        for (int i = 0; owner && i < 8 && !ue::IsA(owner, npcCls_); ++i) owner = ue::Get<ue::UObject*>(owner + ue::Layout::ObjOuter);
        return owner && ue::IsA(owner, npcCls_) ? owner : nullptr;
    }
    std::string AttackerKey(int32_t actionIndex, int type) {
        ue::UObject* owner = nullptr;
        if (npcCls_ && actionIndex > 0 && actionIndex < ue::Num()) {
            owner = ue::Obj(actionIndex);
            for (int i = 0; owner && i < 8 && !ue::IsA(owner, npcCls_); ++i) owner = ue::Get<ue::UObject*>(owner + ue::Layout::ObjOuter);
        }
        return (owner && npcCls_ && ue::IsA(owner, npcCls_) ? ue::ClassName(owner) : std::string("?")) + "|" + std::to_string(type);
    }
    struct Learned { double shift = 0, early = 0, late = 0, range = 0; int n = 0; };
    std::map<std::string, Learned> learned_;
    bool learnedLoaded_ = false;
    bool learnedDirty_ = false;
    double learnedDirtyAt_ = 0;
    static std::wstring LearnedPath() {
        wchar_t dir[MAX_PATH];
        if (!GetEnvironmentVariableW(L"LOCALAPPDATA", dir, MAX_PATH)) return {};
        return std::wstring(dir) + L"\\PBZLab\\learned-timing.tsv";
    }
    void LoadLearned() {
        learnedLoaded_ = true;
        std::ifstream in(LearnedPath());
        std::string line;
        while (std::getline(in, line)) {
            std::istringstream row(line);
            std::string key; double shift = 0, early = 0; int n = 0;
            if (!(row >> key >> shift >> n)) continue;
            double late = 0, range = 0;
            row >> early >> late >> range;
            if (!std::isfinite(shift) || !std::isfinite(early) || !std::isfinite(late) || !std::isfinite(range) || n < 0) continue;
            if (key=="弹:PBZ_ParticleBullet") { learnedDirty_=true; continue; }
            learned_[key] = {TimingShift(key, shift), std::clamp(early, 0.0, 0.4), std::clamp(late, 0.0, 1.2), std::clamp(range, 0.0, 4000.0), std::min(n, 1000000)};
        }
        if (!learned_.empty()) log::Info("自动应对：载入已学时机 {} 条", learned_.size());
    }
    void SaveLearned() {
        std::ofstream out(LearnedPath(), std::ios::trunc);
        for (auto& [k, v] : learned_) out << k << '\t' << v.shift << '\t' << v.n << '\t' << v.early << '\t' << v.late << '\t' << v.range << '\n';
        out.flush();
        if (out.good()) learnedDirty_ = false;
    }
    void MarkLearned(double now) {
        learnedDirty_ = true;
        if (now - learnedDirtyAt_ >= 10) { learnedDirtyAt_ = now; SaveLearned(); }
    }
    double LearnedLate(const std::string& key) {
        if (!learnedLoaded_) LoadLearned();
        auto it = learned_.find(key);
        return it != learned_.end() ? it->second.late : 0;
    }
    double LearnedEarly(const std::string& key) {
        if (!learnedLoaded_) LoadLearned();
        auto it = learned_.find(key);
        return it != learned_.end() ? it->second.early : 0;
    }
    double LearnedShift(const std::string& key) {
        if (!learnedLoaded_) LoadLearned();
        auto it = learned_.find(key);
        return it != learned_.end() && it->second.n >= 1 ? it->second.shift : 0;
    }
    // 受伤时：找预计命中时间离"此刻"最近的来袭帧，偏差 = 预计 - 实际（正数 = 实际更早）。早 / 晚超过 30ms 才学，
    // 指数平均，单次最多 ±0.6s；新偏差立刻生效，文件 10 秒内写一次（%LOCALAPPDATA%\PBZLab\learned-timing.tsv）
    void Learn(double now) {
        const Seen* best = nullptr;
        for (auto& r : recent_)
            if (r.timed && r.act && r.hit > now - 1.0 && r.hit < now + 0.8 && (!best || std::abs(r.hit - now) < std::abs(best->hit - now))) best = &r;
        if (!best || best->attacker.empty() || best->attacker[0] == '?') return;
        const double delta = best->hit - now;  // best->hit 已含旧偏差修正：再学的是剩余误差
        if (best->tries > 0 && delta < -0.03) {
            // 已经格挡 / 闪避过还挨打，而且是在预计时间之后：这招有"尾巴"（飞剑连发 / 多段）。记下尾巴长度，下次举防举到那时
            auto& l = learned_[best->attacker];
            const double tail = std::clamp(-delta + 0.1, 0.0, 1.2);
            if (tail > l.late + 0.02) {
                l.late = tail;
                log::Info("边打边学：{} 有连发尾巴，举防延长到命中后 {:.2f}s", best->attacker, l.late);
                MarkLearned(now);
            }
            return;
        }
        if (!learnedIds_.insert(best->id).second) return;  // 时机每一击只学一次（连发的后几下不算）
        // 只学 ±0.35s 内的：离预计时间更远的多半是同一时间段里别的攻击（无预警的飞行物 / 横扫）打的，不是这一下
        if (std::abs(delta) < 0.03 || std::abs(delta) > 0.35) return;
        auto& l = learned_[best->attacker];
        l.shift = std::clamp(l.n ? l.shift + 0.5 * delta : delta, -0.6, 0.6);
        // 忽早忽晚（快慢刀）时平均值会互相抵消：另记"最多早到多少"，用来提前举防并一直举着（举防也接得住晚到的）
        if (delta > 0) l.early = std::clamp(std::max(l.early * 0.9, delta + std::max(0.0, l.shift)), 0.0, 0.4);
        ++l.n;
        log::Info("边打边学：{} 实际命中比预计{} {:.2f}s，之后按 {:+.2f}s 修正（第 {} 次）", best->attacker, delta > 0 ? "早" : "晚", std::abs(delta), l.shift, l.n);
        MarkLearned(now);
    }
    // 没有来袭帧的伤害：记下当时身边的敌方飞行物（类名、距离、接近速度），找出没被监视到的飞剑 / 远程攻击
    double lastExplain_ = 0;
    int explainBudget_ = 40;
    // 无预警飞行物：游戏不给出招预警、位置也常读不准（花千笑的斩击读出来在 2 公里外）。改学"出现 → 打中"的时间：
    // 无预警挨打时，找 3 秒内由附近敌人放出的飞行物，取最近放出的那一类，记下间隔（同类取平均）。键 "弹:" + 类名
    void LearnBullet(double now) {
        if (!(flags & kNormalFlags)) return;
        double me[3];
        if (!Vec(player_, fnActorLoc_, me)) return;
        const Threat* best = nullptr;
        for (auto& t : threats_) {
            if (!OrdinaryThreat(t)) continue;
            // A verified consumer names the real source. Do not attribute a
            // particle hit to a nearby emitter/arrow born at a similar time.
            if (now-lastNativeContact_>=0 && now-lastNativeContact_<.15 && lastNativeBullet_.Alive() && t.bullet.object!=lastNativeBullet_.object) continue;
            const double age = now - t.born;
            if (age < 0.08 || age > 3.0 || !t.startedAt || !t.ownerRef.Alive() || !SpawnTimingAllowed(t.actor,t.particle,t.moving,t.positioned)) continue;
            // 可测轨迹的运动弹体交给接触预测；不能从一次挨打推断固定飞行时间。
            double at[3];
            if (!Vec(t.owner, fnActorLoc_, at) || std::hypot(me[0] - at[0], me[1] - at[1]) > 2500) continue;
            if (!best || t.born > best->born) best = &t;
        }
        if (!best) return;
        if (!learnedLoaded_) LoadLearned();
        learned_.erase("弹:PBZ_SweepTraceBullet");  // 旧版误学的通用横扫，删掉
        const double age = now - best->born;
        auto& l = learned_["弹:" + best->cls];
        l.shift = l.n ? l.shift * 0.6 + age * 0.4 : age;
        double at[3];
        if (Vec(best->owner, fnActorLoc_, at)) l.range = std::max(l.range, std::hypot(me[0] - at[0], me[1] - at[1]));  // 打中时放它的敌人离多远
        ++l.n;
        log::Info("边打边学：无预警飞行物 {} 放出后约 {:.2f}s 打中（第 {} 次），之后到点自动闪避", best->cls, l.shift, l.n);
        MarkLearned(now);
    }
    // 学过的无预警飞行物：放出它的敌人在 25 米内，到"放出后 X 秒"前一点闪避（不需要它的位置）
    bool TimedBulletDodge(double now) {
        if (!(flags & (kNormalFlags|kRed))) return false;
        // 按学到的出招时刻预判闪弹，不看弹体是否真冲着玩家：开着自动格挡时不用（用户 2026-11-06："自顾自后退闪避"）；
        // 真正来袭的弹体仍由轨迹判定处理，可挡的就挡
        if (flags & kNormalDeflect) return false;
        if (!learnedLoaded_) LoadLearned();
        double me[3];
        if (!Vec(player_, fnActorLoc_, me)) return false;
        if (now < nextTimed_) return false;
        for (auto& t : threats_) {
            if (!RespondableThreat(t) || t.timed || !t.startedAt || !t.ownerRef.Alive() || !SpawnTimingAllowed(t.actor,t.particle,t.moving,t.positioned)) continue;
            auto it = learned_.find("弹:" + t.cls);
            if (it == learned_.end() || it->second.n < 1) continue;
            const double due = t.born + it->second.shift;
            if (!TimedRetryReady(now, due, dodgeLead_ + std::min(gap_, 0.04), t.timedTry, t.timed)) continue;
            double at[3], rot[3];
            if (!Vec(t.owner, fnActorLoc_, at) || !Vec(t.owner, fnActorRot_, rot)) continue;
            const double dx = me[0] - at[0], dy = me[1] - at[1], dist = std::hypot(dx, dy);
            const double reach = it->second.range > 0 ? it->second.range + 300 : 1500;  // 学到的打中距离 + 3 米；没学过按 15 米
            const double yaw = rot[1] * 3.14159265358979 / 180;
            const double facing = dist > 1 ? (std::cos(yaw) * dx + std::sin(yaw) * dy) / dist : 1;
            // 已落在身边的延迟攻击不再受发射者转身影响；远处仍保留射程/朝向限制。
            const bool local = t.positioned && std::hypot(me[0] - t.position[0], me[1] - t.position[1]) <= 400;
            if (!local && (dist > reach || facing < 0.7)) continue;
            t.timedTry = now;
            urgent_ = true;
            const bool ok = RespondThreat(t,due,now,local && !t.moving ? t.position.data() : nullptr);
            urgent_ = false;
            if (ok) { t.timed = true; nextTimed_ = now + 0.35; }
            if (monitorBudget_-- > 0) log::Info("监测 无预警飞行物 {} type={} 按学到的 {:.2f}s {} → {}", t.cls, t.type, it->second.shift, t.response, ok ? "已接受" : "被拒");
            if (ok) return true;
        }
        return false;
    }
    double nextTimed_ = 0;
    int sideFlip_ = 1;
    // 向前闪：闪避前给一个朝向敌人、左右偏 35° 的移动输入（左右交替），靠无敌帧穿过攻击、贴住敌人
    // （贴身的攻击好挡好闪，拉开距离后远程才难判断；不给输入时游戏默认后撤）
    void SideStep(ue::UObject* attacker) {
        if (Blocked() || !attacker || !player_) return;
        if (!fnMoveInput_) if (auto* pawn = ue::FindClass("Pawn")) fnMoveInput_ = pe::FindFunction(pawn, "AddMovementInput");
        double me[3], at[3];
        if (!fnMoveInput_ || !Vec(player_, fnActorLoc_, me) || !Vec(attacker, fnActorLoc_, at)) return;
        const double dx = me[0] - at[0], dy = me[1] - at[1], len = std::hypot(dx, dy);
        if (len < 1) return;
        sideFlip_ = -sideFlip_;
        // 玩家 → 敌人方向，左右偏 35°
        const double fx = -dx / len, fy = -dy / len, c = std::cos(0.61), sn = std::sin(0.61) * sideFlip_;
        const double dir[3] = {fx * c - fy * sn, fx * sn + fy * c, 0};
        MoveDirection(dir);
    }
    void AwayFrom(const double center[3]) {
        double me[3];
        if (!Vec(player_, fnActorLoc_, me)) return;
        double dx = me[0] - center[0], dy = me[1] - center[1], len = std::hypot(dx, dy);
        if (len < 1) { double rot[3]; if (!Vec(player_, fnActorRot_, rot)) return; dx = std::cos(rot[1]*3.14159265358979/180); dy = std::sin(rot[1]*3.14159265358979/180); len = 1; }
        const double dir[3] = {dx/len, dy/len, 0};
        MoveDirection(dir);
    }
    void MoveDirection(const double dir[3]) {
        if(Blocked())return;
        if (!fnMoveInput_) if (auto* pawn = ue::FindClass("Pawn")) fnMoveInput_ = pe::FindFunction(pawn, "AddMovementInput");
        if (!fnMoveInput_) return;
        Params a(fnMoveInput_);
        if (const int o = a.Off("WorldDirection"); o >= 0) std::memcpy(a.buf.data() + o, dir, 3*sizeof(double));
        if (const int o = a.Off("ScaleValue"); o >= 0) { const float one = 1.f; std::memcpy(a.buf.data() + o, &one, 4); }
        if (const int o = a.Off("bForce"); o >= 0) a.buf[o] = 1;
        pe::Call(player_, fnMoveInput_, a.buf.data());
    }
    ue::UObject* fnMoveInput_ = nullptr;
    void ExplainNoFrame(double now) {
        if (now - lastExplain_ < 1.0 || explainBudget_ <= 0) return;
        lastExplain_ = now; --explainBudget_;
        double me[3];
        if (!Vec(player_, fnActorLoc_, me)) return;
        std::string seen;
        for (auto& t : threats_) {
            if (!t.bullet.Alive()) continue;
            double at[3];
            if (!Vec(t.actor ? t.bullet.object : t.owner, fnActorLoc_, at)) continue;
            seen += std::format(" [{} {:.1f}米 {}]", t.cls, std::hypot(me[0] - at[0], me[1] - at[1]) / 100, t.actor ? "弹体" : "判定");
        }
        std::string nearby;  // 不在监视名单里的：身边 6 米内的子弹类对象（全表扫一次，限频）
        int count = 0;
        ue::ForEach([&](int, ue::UObject* o) {
            if (count >= 6) return false;
            if (ue::IsDefault(o)) return true;
            const bool bullet = (bulletActorCls_ && ue::IsA(o, bulletActorCls_)) || (bulletObjCls_ && ue::IsA(o, bulletObjCls_));
            if (!bullet || !bulletActorCls_ || !ue::IsA(o, bulletActorCls_)) return true;
            double at[3];
            if (!Vec(o, fnActorLoc_, at) || std::hypot(me[0] - at[0], me[1] - at[1]) > 600) return true;
            nearby += " " + ue::ClassName(o); ++count;
            return true;
        });
        log::Info("监测 无帧受伤：监视中的{}；身边 6 米的弹体{}", seen.empty() ? "：无" : seen, nearby.empty() ? "：无" : nearby);
    }
    // 有敌方飞行弹体在 8 米内：远处发起的攻击（扔飞剑）照常应对
    bool ProjectileNear() {
        double me[3];
        if (!Vec(player_, fnActorLoc_, me)) return false;
        for (auto& t : threats_) {
            if (!t.actor || !t.bullet.Alive()) continue;
            double at[3];
            if (Vec(t.bullet.object, fnActorLoc_, at) && std::hypot(me[0] - at[0], me[1] - at[1]) < 800) return true;
        }
        return false;
    }
    std::map<AttackId, double> shiftById_, earlyById_, lateById_;
    std::map<AttackId, double> stanceAttempts_;
    std::set<AttackId> learnedIds_;
    std::set<AttackId> farLogged_;

    static bool ExecutableImage(const void* address, size_t length) {
        MEMORY_BASIC_INFORMATION m{};
        if (!address || !length || !VirtualQuery(address,&m,sizeof m) || m.State!=MEM_COMMIT || m.Type!=MEM_IMAGE ||
            m.AllocationBase!=GetModuleHandleW(nullptr) || (m.Protect&(PAGE_GUARD|PAGE_NOACCESS))) return false;
        const auto begin=reinterpret_cast<uintptr_t>(address), region=reinterpret_cast<uintptr_t>(m.BaseAddress);
        const DWORD access=m.Protect&0xff;
        return begin>=region && length<=m.RegionSize && begin-region<=m.RegionSize-length &&
            (access==PAGE_EXECUTE || access==PAGE_EXECUTE_READ || access==PAGE_EXECUTE_READWRITE || access==PAGE_EXECUTE_WRITECOPY);
    }
    void* ResolveNativeContact() {
        auto* fn=pe::FindFunction(contactAttackCls_,"TryAttack");
        if (!fn || !fnContactValid_ || !fnComponentOwner_ || !primitiveCls_ || offModuleBullet_<0 ||
            offActorData_<0 || offActorSource_<0 || offObjectData_<0 || offObjectSource_<0 ||
            ue::Get<int32_t>(fn+ue::Layout::StructSize)!=120) return nullptr;
        Params layout(fn);
        for (const auto& [name,offset] : std::initializer_list<std::pair<const char*,int>>{
            {"TargetComponent",0},{"HitBone",8},{"AttackDirection",16},{"HitLocation",40},{"HitNormal",64},{"SweepDirection",88},{"ReturnValue",112}})
            if (layout.Off(name)!=offset) return nullptr;
        // UFunction native pointer offset is accepted only with the full
        // current-build thunk/body fingerprints below, never on its own.
        auto* entry=ue::Get<uint8_t*>(fn+0x128);
        std::array<uint8_t,768> thunk{};
        if (!ExecutableImage(entry,thunk.size()) || !ue::Read(entry,thunk.data(),thunk.size())) return nullptr;
        const int call=FindAttackCall(thunk.data(),thunk.size());
        if (call<0) return nullptr;
        auto* target=reinterpret_cast<uint8_t*>(reinterpret_cast<intptr_t>(entry)+call+5+Copied<int32_t>(thunk.data()+call+1));
        std::array<uint8_t,sizeof(kAttackBody)> body{};
        if (!ExecutableImage(target,body.size()) || !ue::Read(target,body.data(),body.size()) || !AttackBodyMatches(body.data(),body.size())) return nullptr;
        return target;
    }
    static bool __fastcall NativeContact(ue::UObject* module,ue::UObject* component,const uint64_t* bone,
                                        const double* direction,const double* location,const double* normal,const double* sweep) {
        auto* e=nativeEngine_;
        // Other threads never touch UObject state or invoke gameplay. Always
        // preserve the original attack, its bool result and all 7 arguments.
        if (e && GetCurrentThreadId()==e->observerThread_) {
            try { e->BeforeNativeContact(module,component,direction,sweep); }
            catch (...) { /* an optional response must never swallow native damage */ }
        }
        return nativeOriginal_(module,component,bone,direction,location,normal,sweep);
    }
    void BeforeNativeContact(ue::UObject* module,ue::UObject* component,const double* direction,const double* sweep) {
        if (!nativeEnabled_ || inContact_ || !(flags&kContactFlags) || !Valid() ||
            !ObjectRef(module).Alive() || !ue::IsA(module,contactAttackCls_) ||
            !ObjectRef(component).Alive() || !ue::IsA(component,primitiveCls_) ||
            !ue::Get<uint8_t>(module+0xd0)) return; // attack-enabled gate proven by the exact body fingerprint
        Params owner(fnComponentOwner_);
        const int result=owner.Off("ReturnValue");
        if (result<0) return;
        pe::Call(component,fnComponentOwner_,owner.buf.data());
        if (Copied<ue::UObject*>(owner.buf.data()+result)!=player_) return;
        auto* bullet=ue::Get<ue::UObject*>(module+offModuleBullet_);
        if (ObjectRef(bullet).Alive()) { lastNativeBullet_=ObjectRef(bullet); lastNativeContact_=Now(); }
        double copiedDirection[3]{},copiedSweep[3]{};
        const bool hasDirection=direction && ue::Read(direction,copiedDirection,sizeof copiedDirection);
        const bool hasSweep=sweep && ue::Read(sweep,copiedSweep,sizeof copiedSweep);
        ContactResponse(bullet,module,"Native-TryAttack",hasDirection ? copiedDirection : nullptr,hasSweep ? copiedSweep : nullptr);
    }
    void SyncNativeContact() {
        const bool wanted=monitoring_ && (flags&kContactFlags);
        if (wanted && !nativeTried_) {
            nativeTried_=true;
            auto* target=ResolveNativeContact();
            if (target) {
                nativeEngine_=this;
                // Existing absolute-hook service freezes/fixes thread IPs.
                // Its restored trampoline stays resident, so an in-flight
                // non-game-thread original call can safely finish on disable.
                std::string why;
                nativeCreated_=abshook::Prepare(nativeHook_,target,reinterpret_cast<void*>(NativeContact),&why);
                if (nativeCreated_) nativeOriginal_=reinterpret_cast<NativeAttack>(nativeHook_.tramp);
            }
            if (!nativeCreated_) log::Info("自动应对：原生接触入口指纹/布局/钩子不符，禁用此兜底，保留已有预测");
        }
        if (nativeCreated_ && nativeEnabled_!=wanted && abshook::Apply(nativeHook_,wanted)) {
            nativeEnabled_=wanted;
            log::Info("自动应对：原生接触入口{}（按真实颜色各守开关；原攻击与判定保持）",wanted ? "启用" : "还原");
        }
    }
    void ContactResponse(ue::UObject* obj, ue::UObject* attackModule, const char* route,
                         const double* direction = nullptr, const double* sweep = nullptr) {
        if (inContact_ || GetCurrentThreadId()!=observerThread_ || !(flags&kContactFlags) || !Valid() ||
            !ObjectRef(obj).Alive() || ue::IsDefault(obj) || !ObjectRef(attackModule).Alive() ||
            !ue::IsA(attackModule,contactAttackCls_) || ue::Get<ue::UObject*>(attackModule+offModuleBullet_)!=obj) return;
        if(Blocked())return;
        const bool actor = bulletActorCls_ && ue::IsA(obj,bulletActorCls_);
        if (!actor && (!bulletObjCls_ || !ue::IsA(obj,bulletObjCls_))) return;
        const int status = actor ? offActorStatus_ : offObjectStatus_, owner = actor ? offActorOwner_ : offObjectOwner_;
        if (status<0 || owner<0 || ue::Get<uint8_t>(obj+status,255)!=2) return;
        auto* attacker = ue::Get<ue::UObject*>(obj+owner);
        if (!ObjectRef(attacker).Alive() || attacker==player_ || !npcCls_ || !ue::IsA(attacker,npcCls_)) return;
        Params eligibility(fnContactValid_);
        const int target = eligibility.Off("TargetAvatar"), valid = eligibility.Off("ReturnValue");
        if (target < 0 || valid < 0) return;
        memcpy(eligibility.buf.data()+target,&player_,sizeof player_);
        pe::Call(attackModule,fnContactValid_,eligibility.buf.data());
        if (!eligibility.buf[valid]) return;
        Threat response;
        if (auto it = std::find_if(threats_.begin(),threats_.end(),[&](const Threat& t) { return t.bullet.object == obj && t.bullet.Alive(); }); it != threats_.end()) response = *it;
        else {
            response.bullet = ObjectRef(obj); response.actor = actor;
            response.data = ObjectRef(ue::Get<ue::UObject*>(obj+(actor ? offActorData_ : offObjectData_)));
            auto* ability = ue::Get<ue::UObject*>(obj+(actor ? offActorSource_ : offObjectSource_));
            response.type = DataAttackType(ability,response.data.object);
        }
        int color = response.type;
        ColoredTags(obj,actor ? offActorTags_ : offObjectTags_,color);
        if (response.data.Alive()) ColoredTags(response.data.object,offDataTags_,color);
        // A real colored contact may have NO ReceiveFrame (e.g. the delayed
        // explosion of a red throw). Only its own color flag can authorize
        // the fallback; normal modes cannot borrow that permission.
        if (!PreciseContactPermission(flags,color)) return;
        response.type = color; response.framed = false; response.owner = attacker; response.ownerRef = ObjectRef(attacker);
        const double now = Now();
        if (!std::isfinite(now) || now <= 0) return;
        auto& contact = contacts_[obj];
        if (!contact.bullet.Alive()) { contact = ContactAttempt(); contact.bullet = ObjectRef(obj); }
        contact.seen = now;
        const bool guarding = ownsStance_ && StanceRunning() && !shachiScarce_;
        if (contact.guarded && guarding) return;
        const bool dashing = DashBlocksResponse();
        if (dashing || !CollisionRetry(now,contact.tried,contact.accepted,contact.guarded,guarding,false)) return;
        if (color!=PowerAttack && ColoredType(color) && ProtectedResponseRunning()) return;
        if (contact.tried < 0) ++threatSeen_;
        contact.tried = now;
        struct Scope { bool& value; bool prior; explicit Scope(bool& v):value(v),prior(v) { value=true; } ~Scope() { value=prior; } } scope(inContact_), urgency(urgent_);
        double escape[3];
        const bool lateral = particleCls_ && ue::IsA(obj,particleCls_) && LateralEscape(direction,sweep,sideFlip_,escape);
        double areaCenter[3]{},me[3]{};
        std::array<double,12> areaTransform{};
        const bool actualArea = response.area && response.shape>=0 && offCustomTransform_>=0 && offAttach_>=0 &&
            !ue::Get<ue::UObject*>(obj+offAttach_) && Vec(player_,fnActorLoc_,me) &&
            ue::Read(obj+offCustomTransform_,areaTransform.data(),sizeof areaTransform) &&
            AreaContains(me,areaTransform.data(),response.shape,response.radius,response.halfHeight,response.extent.data());
        if (actualArea) std::copy_n(areaTransform.data()+4,3,areaCenter);
        bool ok=false;
        if (color==BlueEye) { response.response="蓝光格挡"; ok=Deflect(-1,nullptr,now,now); }
        else if (color==RedEye) { response.response="红光闪避"; ok=Dodge(-1,RedEye,nullptr,0,now); }
        else if (color==PowerAttack) { response.response="黑光（不自动应对）"; ok=false; }
        else ok=RespondThreat(response,now,now,actualArea ? areaCenter : nullptr,false,lateral ? escape : nullptr);
        if (ok) {
            contact.accepted = now;
            contact.guarded = color==BlueEye || (!ColoredType(color) && response.response!=std::string_view("闪避"));
            ++threatDodged_;
        }
        if (nativeLogBudget_-- > 0) log::Info("监测 真实接触前[{}] {} 对象={} 数据={} type={} → {}{} 横向={}（请求不等于免伤）",
            route,ue::ClassName(obj),ue::ObjName(obj),response.data.Alive() ? ue::ObjName(response.data.object) : "未知",color,response.response,ok ? "已接受" : "被拒",lateral);
    }
    // Confirmed live PE route: the BP's real trace hits arrive here BEFORE
    // its TryAttack / shared-effect / HitTarget processing. Observe the
    // engine-owned array, resolve HitActor with the native scalar helper,
    // then let the original attack judge the genuine guard/dodge normally.
    void ConsiderContactFn(ue::UObject* fn) {
        if (!monitoring_ || !contactLayout_ || !fn || ue::ClassOf(fn) != functionCls_) return;
        if (!contactName_ && ue::ObjName(fn) == "On Trace Hit") contactName_ = ue::Get<uint32_t>(fn+ue::Layout::ObjName);
        if (!contactName_ || ue::Get<uint32_t>(fn+ue::Layout::ObjName) != contactName_ ||
            std::find(contactFns_.begin(),contactFns_.end(),fn) != contactFns_.end()) return;
        auto* owner = ue::Get<ue::UObject*>(fn+ue::Layout::ObjOuter);
        if (!owner || ue::ObjName(owner).find("Bullet") == std::string::npos) return;
        const int hits = Params(fn).Off("HitData");
        if (hits < 0) return;
        contactFns_.push_back(fn);
        contactTokens_.push_back(pe::Observe(fn,[this,hits](ue::UObject* obj,void* raw) {
            if (!raw || inContact_ || GetCurrentThreadId() != observerThread_ || !(flags & kContactFlags) || !Valid() ||
                !ObjectRef(obj).Alive() || ue::IsDefault(obj) || !ue::IsA(obj,bulletActorCls_) ||
                offActorStatus_ < 0 || ue::Get<uint8_t>(obj+offActorStatus_,255) != 2) return;
            auto* attacker = offActorOwner_ >= 0 ? ue::Get<ue::UObject*>(obj+offActorOwner_) : nullptr;
            if (!attacker || attacker == player_ || !npcCls_ || !ue::IsA(attacker,npcCls_)) return;
            auto* header = static_cast<uint8_t*>(raw)+hits;
            auto* data = ue::Get<uint8_t*>(header);
            const int count = ue::Get<int32_t>(header+8,-1), capacity = ue::Get<int32_t>(header+12,-1);
            if (!data || count <= 0 || count > 64 || capacity < count || capacity > 4096) return;
            Params broken(fnBreakHit_);
            const int input = broken.Off("Hit"), actor = broken.Off("HitActor");
            if (input < 0 || actor < 0) return;
            bool localHit = false;
            for (int i = 0; i < count; ++i) {
                if (!ue::Read(data+0x100*i,broken.buf.data()+input,0x100)) continue;
                pe::Call(staticsCdo_,fnBreakHit_,broken.buf.data());
                if (Copied<ue::UObject*>(broken.buf.data()+actor) == player_) { localHit = true; break; }
            }
            if (!localHit) return; // empty traces, scenery, allies, other targets are not incoming player damage
            const int attackOffset = ue::PropOffset(ue::ClassOf(obj),"Attack Module");
            auto* attackModule = attackOffset >= 0 ? ue::Get<ue::UObject*>(obj+attackOffset) : nullptr;
            if (!ObjectRef(attackModule).Alive() || !ue::IsA(attackModule,contactAttackCls_) ||
                ue::Get<ue::UObject*>(attackModule+offModuleBullet_) != obj) return;
            ContactResponse(obj,attackModule,"BP-HitData");
        }));
    }
    void ConsiderInitFn(ue::UObject* o) {
        if (!monitoring_ || !initName_ || !o || ue::ClassOf(o) != functionCls_ || ue::Get<uint32_t>(o + ue::Layout::ObjName) != initName_) return;
        if (std::find(initFns_.begin(), initFns_.end(), o) != initFns_.end()) return;
        initFns_.push_back(o);
        const int offOwner = Params(o).Off("OwnerAvatar");
        if (offOwner < 0) return;
        const int offData = Params(o).Off("Data");
        const int offSource = Params(o).Off("SourceAbility"), offParent = Params(o).Off("Parent");
        initTokens_.push_back(pe::Observe(o, [this, offOwner, offData, offSource, offParent](ue::UObject* obj, void* params) {
            if (!params || GetCurrentThreadId() != observerThread_ || !flags || !playerRef_.Alive()) return;
            auto* owner = Copied<ue::UObject*>(static_cast<uint8_t*>(params) + offOwner);
            if (!owner || owner == player_ || !npcCls_ || !ue::IsA(owner, npcCls_)) return;
            const bool actor = bulletActorCls_ && ue::IsA(obj, bulletActorCls_);
            if (!actor && !(bulletObjCls_ && ue::IsA(obj, bulletObjCls_))) return;
            Threat t;
            // 对象池会复用同一个 UObject；新一次初始化必须替换旧的轨迹/计时状态。
            std::erase_if(threats_, [&](const Threat& old) { return old.bullet.object == obj; });
            contacts_.erase(obj);
            t.bullet = ObjectRef(obj); t.owner = owner; t.ownerRef = ObjectRef(owner); t.born = lastNow_; t.actor = actor; t.cls = ue::ClassName(obj);
            auto* data = offData >= 0 ? Copied<ue::UObject*>(static_cast<uint8_t*>(params)+offData) : nullptr;
            t.data = ObjectRef(data);
            auto* source = offSource >= 0 ? Copied<ue::UObject*>(static_cast<uint8_t*>(params)+offSource) : nullptr;
            t.type = DataAttackType(source,data);
            auto* parent = offParent >= 0 ? Copied<ue::UObject*>(static_cast<uint8_t*>(params)+offParent) : nullptr;
            for (const auto& previous : threats_) if (previous.bullet.object == parent && ColoredType(previous.type)) t.type = MergeAttackColor(t.type,previous.type);
            t.particle = actor && particleCls_ && ue::IsA(obj,particleCls_);
            t.area = !actor && data && overlapDataCls_ && ue::IsA(data, overlapDataCls_);
            if (t.area && offAreaShape_ >= 0 && offAreaDelay_ >= 0 && offAreaLife_ >= 0 && offAreaInterval_ >= 0 &&
                offShapeType_ >= 0 && offShapeRadius_ >= 0 && offShapeHeight_ >= 0 && offShapeExtent_ >= 0) {
                auto* shape = data + offAreaShape_;
                t.shape = ue::Get<uint8_t>(shape+offShapeType_, 255);
                t.radius = ue::Get<float>(shape+offShapeRadius_);
                t.halfHeight = ue::Get<float>(shape+offShapeHeight_);
                ue::Read(shape+offShapeExtent_, t.extent.data(), sizeof(t.extent));
                t.areaDelay = ue::Get<float>(data+offAreaDelay_, -1);
                t.areaLifetime = ue::Get<float>(data+offAreaLife_, -1);
                t.areaInterval = ue::Get<float>(data+offAreaInterval_, -1);
                if (!std::isfinite(t.areaDelay) || t.areaDelay < 0 || t.areaDelay > 30 ||
                    !std::isfinite(t.areaInterval) || t.areaInterval < 0 || t.areaInterval > 30 ||
                    !std::isfinite(t.areaLifetime) || t.areaLifetime < 0 || t.areaLifetime > 60) t.shape = -1;
                if (monitorBudget_-- > 0) log::Info("监测 范围攻击 {} 数据={} 形状={} 半径={:.0f} 延迟={:.2f}s 间隔={:.2f}s 持续={:.2f}s",
                    t.cls, ue::ObjName(data), t.shape, t.radius, t.areaDelay, t.areaInterval, t.areaLifetime);
            }
            threats_.push_back(std::move(t));
        }));
    }
    bool Vec(ue::UObject* a, ue::UObject* fn, double out[3]) {
        if (!fn) return false;
        uint8_t ret[32] = {};
        pe::Call(a, fn, ret);
        memcpy(out, ret, 24);
        return std::isfinite(out[0]) && std::isfinite(out[1]) && std::isfinite(out[2]);
    }
    bool FrameCovers(double now) const {
        for (auto& r : recent_) if (r.act && r.hit > now - 0.1 && r.hit < now + 0.5) return true;
        return false;
    }
    bool AreaThreat(Threat& t, const double me[3], double now, double center[3], std::string& why) {
        if (t.shape < 0 || !t.data.Alive() || offCustomTransform_ < 0 || offAttach_ < 0) return false;
        // 自定义附着/多落点的区域要走游戏的原生落点接口；不能拿发射者代替落点。
        if (ue::Get<ue::UObject*>(t.bullet.object+offAttach_)) return false;
        if (t.areaDue < 0) t.areaDue = now + t.areaDelay;
        const double lead = dodgeLead_ + std::min(gap_, 0.04);
        if (now > t.areaDue + 0.1) {
            if (t.areaInterval <= 0) return false;
            t.areaDue += std::ceil((now-t.areaDue-0.1)/t.areaInterval)*t.areaInterval;
        }
        if (t.areaLifetime > 0 && t.areaDue > t.startedAt+t.areaDelay+t.areaLifetime) return false;
        if (now < t.areaDue-lead || now > t.areaDue+0.1) return false;
        std::array<double, 12> transform{};
        if (!ue::Read(t.bullet.object+offCustomTransform_, transform.data(), sizeof(transform)) ||
            !AreaContains(me, transform.data(), t.shape, t.radius, t.halfHeight, t.extent.data())) return false;
        std::copy_n(transform.data()+4, 3, center);
        why = std::format("落点 {:.1f}米 生效前 {:.2f}s（按原生范围/延迟）", std::hypot(me[0]-center[0], me[1]-center[1])/100, t.areaDue-now);
        return true;
    }
    void Threats(double now, bool busy) {
        std::erase_if(threats_, [&](const Threat& t) {
            const int off = t.actor ? offActorStatus_ : offObjectStatus_;
            const int ownerOff = t.actor ? offActorOwner_ : offObjectOwner_;
            return now - t.born > 30 || !t.bullet.Alive() || !t.ownerRef.Alive() ||
                (ownerOff >= 0 && ue::Get<ue::UObject*>(t.bullet.object+ownerOff) != t.owner) ||
                (off >= 0 && ue::Get<uint8_t>(t.bullet.object + off, 3) >= 3) ||
                (offDestroyed_ >= 0 && !t.actor && ue::Get<uint8_t>(t.bullet.object + offDestroyed_));
        });
        if (!(flags & kNormalFlags)) return; // 颜色单开仍需清理已回池对象，但不产生普通应对
        MatchThreatFrames(now);
        // 原生弹体常用自定义运动，Actor.GetVelocity 为零；用连续世界位置测量速度。
        // 先更新轨迹，再决定是否允许退回“出现→打中”计时，避免慢弹刚发射就抢闪。
        for (auto& t : threats_) {
            if (!OrdinaryThreat(t) || t.particle) continue;
            const int statusOff = t.actor ? offActorStatus_ : offObjectStatus_;
            const int status = statusOff >= 0 ? ue::Get<uint8_t>(t.bullet.object+statusOff, 255) : 255;
            if (status != 2) { t.positioned = false; t.moving = false; t.velocity = {}; continue; }
            if (!t.startedAt) {
                if (t.actor && now-t.born > 0.1 && monitorBudget_-- > 0)
                    log::Info("监测 弹体实际发射 {} 预置等待 {:.2f}s（蓄力阶段不按飞行时间闪避）", t.cls, now-t.born);
                t.born = now; t.positioned = false; t.moving = false; t.sampledAt = 0;
                t.startedAt = now;
                if (t.area) t.areaDue = now + t.areaDelay;
            }
            if (!t.actor) continue;
            double at[3];
            if (!Vec(t.bullet.object, fnActorLoc_, at)) { t.positioned = false; continue; }
            const double elapsed = now - t.sampledAt;
            if (t.positioned && elapsed > 0 && elapsed < 0.003) continue;
            if (t.positioned && elapsed >= 0.003 && elapsed <= 0.15) {
                for (int i = 0; i < 3; ++i) t.velocity[i] = (at[i] - t.position[i]) / elapsed;
                const double speed = std::hypot(t.velocity[0], t.velocity[1], t.velocity[2]);
                if (speed > 100000) { t.positioned = false; t.velocity = {}; continue; } // 回池/传送跳变不是飞行
                if (speed > 50) t.moving = true;
            } else {
                t.velocity = {};
                Vec(t.bullet.object, fnActorVel_, t.velocity.data());
                if (std::hypot(t.velocity[0], t.velocity[1], t.velocity[2]) > 50) t.moving = true;
            }
            std::copy_n(at, 3, t.position.begin()); t.sampledAt = now; t.positioned = true;
        }
        if (!threats_.empty() && !busy && dashTag_[0] && TimedBulletDodge(now)) return;
        if (threats_.empty() || busy || !dashTag_[0]) return;
        double me[3], playerVelocity[3] = {};
        if (!Vec(player_, fnActorLoc_, me)) return;
        Vec(player_, fnActorVel_, playerVelocity);
        const bool covered = FrameCovers(now);
        for (auto& t : threats_) {
            if (!RespondableThreat(t)) continue;
            // 近身判定：有来袭帧覆盖的是普通近战（交给帧逻辑），整条作废；只看生成后 1 秒内
            if (!t.startedAt) continue; // 原生 Running 状态出现前不把初始化当出手
            if (!t.actor && !t.area && (covered || now - t.born > 1.0)) { t.tries = 3; continue; }
            if (t.accepted >= 3 && !t.area) continue;
            if (now - t.lastTry < (t.accepted ? 0.35 : 0.025)) continue;
            double at[3], vel[3] = {};
            std::string why;
            if (t.area) {
                if (!AreaThreat(t, me, now, at, why)) continue;
            } else if (t.actor) {  // 飞行弹体：相对轨迹 → 预计接触时间
                if (!t.positioned || !t.moving) continue;
                std::copy(t.position.begin(), t.position.end(), at);
                for (int i = 0; i < 3; ++i) vel[i] = t.velocity[i] - playerVelocity[i];
                const double d[3] = {at[0] - me[0], at[1] - me[1], at[2] - me[2]};
                const double dist = std::hypot(d[0], d[1], d[2]);
                const double tti = ProjectileContact(d, vel, 85, 120);
                const double lead = std::min(projLead_, dodgeLead_ + std::min(gap_, 0.04));
                if (tti < 0 || tti > lead) continue;
                why = std::format("距离 {:.1f} 米 预计 {:.2f}s 命中", dist / 100, std::min(tti, 9.99));
            } else {        // 近身判定：发起者够近且面朝玩家才算冲着玩家
                // 用户 2026-11-06：敌人只是站近、面朝玩家（类型未知的无帧威胁）不再闪避——这是"没人打却自己后闪"的主要来源
                if (t.type == CantDeflect) { t.tries = 3; continue; }
                if (t.tries >= 2 || (t.tries && !t.accepted)) continue;
                if (now - t.born < kSweepConfirm) continue;  // 普通近战的来袭帧常比判定晚一点出现：先等等，有帧就交给帧逻辑
                double rot[3];
                if (!Vec(t.owner, fnActorLoc_, at) || !Vec(t.owner, fnActorRot_, rot)) continue;
                const double dx = me[0] - at[0], dy = me[1] - at[1], dist = std::hypot(dx, dy);
                if (dist > kSweepRange) continue;
                const double yaw = rot[1] * 3.14159265358979 / 180;
                const double facing = dist > 1 ? (std::cos(yaw) * dx + std::sin(yaw) * dy) / dist : 1;
                if (facing < 0.5) continue;  // 背对 / 侧对玩家（夹角 > 60°）
                why = std::format("发起者 {:.1f} 米 朝向夹角 {:.0f}°", dist / 100, std::acos(std::clamp(facing, -1.0, 1.0)) * 180 / 3.14159265358979);
            }
            if (covered && !t.actor && !t.area) continue;
            if (!t.tries) ++threatSeen_;
            ++t.tries; t.lastTry = now;
            urgent_ = true;  // 无帧攻击都是临近才发现：被拒就用最高优先级重试
            const bool ok = RespondThreat(t,now+0.05,now,t.area ? at : nullptr);
            // 无类型证据的弹体不能盲目格挡，也不能借用红光反制能力（会接走另一条红光）。
            urgent_ = false;
            if (ok) { ++t.accepted; ++threatDodged_; }
            if (ok && t.area) t.areaDue = t.areaInterval > 0 ? t.areaDue+t.areaInterval : 1e9;
            if (monitorBudget_-- > 0) log::Info("监测 无帧攻击 {} type={} {} → {}{}", t.cls, t.type, why, t.response, ok ? "已接受" : "被拒");
            if (ok) break;  // 一帧只出一个动作
        }
    }
    // 来袭帧来源（近战动作）→ 沿 Outer 链找发起者角色；快打到时离玩家超过 6 米视为空挥。飞行物 / 其他来源不判（本来就远）
    bool TooFar(int32_t actionIndex) {
        if (!npcCls_ || !fnActorLoc_ || actionIndex <= 0 || actionIndex >= ue::Num()) return false;
        auto* a = ue::Obj(actionIndex);
        if (!a || !ue::IsA(a, durationCls_)) return false;
        ue::UObject* owner = a;
        for (int i = 0; owner && i < 8 && !ue::IsA(owner, npcCls_); ++i) owner = ue::Get<ue::UObject*>(owner + ue::Layout::ObjOuter);
        if (!owner || !ue::IsA(owner, npcCls_)) return false;
        double me[3], at[3];
        if (!Vec(player_, fnActorLoc_, me) || !Vec(owner, fnActorLoc_, at)) return false;
        return std::hypot(me[0] - at[0], me[1] - at[1]) > kMeleeReach;
    }
    static constexpr double kMeleeReach = 600;

    // ---- 自动攻击：没有攻击要来时，对身边最近的怪物按轻攻击（SGameInputComponent.ForceTrigger = 等同真按键，
    // 走游戏自己的输入缓冲与连段）。攻击来袭时格挡 / 闪避用 ForceTag 打断出招，所以只需在空档出招 ----
    ue::UObject *fnTrigger_ = nullptr, *iaAttack_ = nullptr;
    double nextAttack_ = 0, attackGap_ = .25;
    int attackRange_ = 450, offMonster_ = -2;
    uint64_t attacks_ = 0;
    static bool GameFocused() {
        DWORD pid = 0;
        GetWindowThreadProcessId(GetForegroundWindow(), &pid);
        return pid == GetCurrentProcessId();
    }
    // 诊断：开着却不出招时，每 5 秒记一次各原因次数
    struct AttackWhy { int idle = 0, focus = 0, upcoming = 0, threat = 0, stance = 0, tooFar = 0, none = 0; } why_;
    double nextWhy_ = 0;
    uint64_t lastAttacks_ = 0;
    ue::UObject* fnCylinder_ = nullptr;
    void AttackDiag(double now) {
        if (now < nextWhy_) return;
        nextWhy_ = now + 5;
        if (attacks_ == lastAttacks_)
            log::Info("自动攻击 5 秒内未出招：非空档{} 窗口未聚焦{} 有来袭{} 刚躲过{} 举防中{} 敌人太远{} 附近无怪{}", why_.idle, why_.focus, why_.upcoming,
                      why_.threat, why_.stance, why_.tooFar, why_.none);
        lastAttacks_ = attacks_;
        why_ = {};
    }
    void AutoAttack(double now, const std::vector<double>& upcoming, bool idle) {
        AttackDiag(now);
        if (!sgame_ || !fnTrigger_ || !iaAttack_ || now < nextAttack_) return;
        if (!idle) { ++why_.idle; return; }
        if (!GameFocused()) { ++why_.focus; return; }
        // 只要看得到来袭（来袭帧约提前 0.3 秒出现）就不出招：出招后段不可取消，格挡 / 闪避会被游戏拒绝
        if (!upcoming.empty()) { ++why_.upcoming; return; }
        for (auto& t : threats_) if (t.tries < 3 && now - t.born < 1.0) { ++why_.threat; return; }
        if (ownsStance_ && releaseAt_ > now) { ++why_.stance; return; }  // 正举着防
        double me[3];
        if (!Vec(player_, fnActorLoc_, me)) return;
        if (offMonster_ == -2) offMonster_ = npcCls_ ? ue::PropOffset(npcCls_, "BIsMonster") : -1;
        const int offHp = attr::AttrOffset("Health");
        // 距离按"到敌人身体边缘"算：Boss / 大型怪胶囊半径大，中心距离常超过攻击范围（实测贴身也不出招）
        if (!fnCylinder_) if (auto* actor = ue::FindClass("Actor")) fnCylinder_ = pe::FindFunction(actor, "GetSimpleCollisionCylinder");
        double best = 1e18, bestCenter = 1e18;
        bool any = false;
        for (auto& t : attr::Targets()) {
            if (t.who != attr::Who::Enemies || !attr::Alive(t) || !npcCls_ || !ue::IsA(t.obj, npcCls_)) continue;
            if (offMonster_ >= 0 && !ue::Get<uint8_t>(t.obj + offMonster_)) continue;
            if (offHp >= 0 && *attr::AttrCurrent(t, offHp) <= 0) continue;
            double e[3];
            if (!Vec(t.obj, fnActorLoc_, e)) continue;
            any = true;
            const double center = std::sqrt((e[0] - me[0]) * (e[0] - me[0]) + (e[1] - me[1]) * (e[1] - me[1]) + (e[2] - me[2]) * (e[2] - me[2]));
            float radius = 0;
            if (fnCylinder_ && center < attackRange_ + 1500) {
                Params c(fnCylinder_);
                pe::Call(t.obj, fnCylinder_, c.buf.data());
                if (const int r = c.Off("CollisionRadius"); r >= 0) radius = std::clamp(*reinterpret_cast<float*>(c.buf.data() + r), 0.0f, 1500.0f);
            }
            if (center - radius < best) { best = center - radius; bestCenter = center; }
        }
        if (!any) { ++why_.none; return; }
        if (best > attackRange_) { ++why_.tooFar; return; }
        best = bestCenter * bestCenter;
        Params p(fnTrigger_);
        const int ia = p.Off("InputAction");
        if (ia < 0) return;
        memcpy(p.buf.data() + ia, &iaAttack_, sizeof iaAttack_);
        if (Blocked()) return;
        pe::Call(sgame_, fnTrigger_, p.buf.data());
        nextAttack_ = now + attackGap_;
        if (++attacks_ % 50 == 1) log::Info("自动攻击 第 {} 下（最近敌人 {:.1f} 米）", attacks_, std::sqrt(best) / 100);
    }
public:
    void Monitor(bool on) {
        if (on == monitoring_) return;
        monitoring_ = on;
        if (on) {
            auto* chr = ue::FindClass("PBZCharacter");
            auto* set = ue::FindClass("BasicAttributeSet");
            offSetBase_ = chr ? ue::PropOffset(chr, "AttributeSetBase") : -1;
            offHealth_ = set ? ue::PropOffset(set, "Health") : -1;
            offShachi_ = set ? ue::PropOffset(set, "Shachi") : -1;
            offShachiMax_ = set ? ue::PropOffset(set, "Shachi_Max") : -1;
            lastHp_ = -1;
            log::Info("监测：受伤检测{}", offSetBase_ >= 0 && offHealth_ >= 0 ? "已开" : "不可用（字段缺失）");
            functionCls_ = ue::FindClass("Function");
            bulletObjCls_ = ue::FindClass("BulletObjectBase");
            bulletActorCls_ = ue::FindClass("BulletActorBase");
            particleCls_ = ue::FindClass("PBZ_ParticleBullet");
            primitiveCls_ = ue::FindClass("PrimitiveComponent");
            fnComponentOwner_ = pe::FindFunction(ue::FindClass("ActorComponent"),"GetOwner");
            npcCls_ = ue::FindClass("PBZCharacter_NPC");
            auto* actor = ue::FindClass("Actor");
            fnActorLoc_ = actor ? pe::FindFunction(actor, "K2_GetActorLocation") : nullptr;
            fnActorVel_ = actor ? pe::FindFunction(actor, "GetVelocity") : nullptr;
            fnActorRot_ = actor ? pe::FindFunction(actor, "K2_GetActorRotation") : nullptr;
            offDestroyed_ = bulletObjCls_ ? ue::PropOffset(bulletObjCls_, "Destroyed") : -1;
            offActorStatus_ = bulletActorCls_ ? ue::PropOffset(bulletActorCls_, "BulletStatus") : -1;
            offObjectStatus_ = bulletObjCls_ ? ue::PropOffset(bulletObjCls_, "BulletStatus") : -1;
            offActorOwner_ = bulletActorCls_ ? ue::PropOffset(bulletActorCls_, "BulletOwnerAvatar") : -1;
            offActorData_ = bulletActorCls_ ? ue::PropOffset(bulletActorCls_, "BulletData") : -1;
            offActorSource_ = bulletActorCls_ ? ue::PropOffset(bulletActorCls_, "BulletSourceAbility") : -1;
            offObjectOwner_ = bulletObjCls_ ? ue::PropOffset(bulletObjCls_, "BulletOwnerAvatar") : -1;
            offObjectSource_ = bulletObjCls_ ? ue::PropOffset(bulletObjCls_,"BulletSourceAbility") : -1;
            offObjectTags_ = bulletObjCls_ ? ue::PropOffset(bulletObjCls_,"BulletTags") : -1;
            offActorTags_ = bulletActorCls_ ? ue::PropOffset(bulletActorCls_, "BulletTags") : -1;
            auto* dataBase = ue::FindClass("BulletDataBase");
            offDataTags_ = dataBase ? ue::PropOffset(dataBase, "BulletTags") : -1;
            auto* abilityClass = ue::FindClass("LegoAbilityBlueprintGeneratedClass");
            offAbilityTimelines_ = abilityClass ? ue::PropOffset(abilityClass, "AbilityTimelines") : -1;
            offProjectileData_ = projCls_ ? ue::PropOffset(projCls_, "Bullet Data") : -1;
            offCustomTransform_ = bulletObjCls_ ? ue::PropOffset(bulletObjCls_, "CustomTransform") : -1;
            offAttach_ = bulletObjCls_ ? ue::PropOffset(bulletObjCls_, "CustomAttachToComponent") : -1;
            overlapDataCls_ = ue::FindClass("PBZ_OverlapBulletData");
            auto areaOffset = [&](const char* name) { return overlapDataCls_ ? ue::PropOffset(overlapDataCls_, name) : -1; };
            offAreaShape_ = areaOffset("ColliderData"); offAreaDelay_ = areaOffset("StartDelay");
            offAreaLife_ = areaOffset("Lifetime"); offAreaInterval_ = areaOffset("OverlapInterval");
            offAreaLocation_ = areaOffset("LocationData");
            auto* location = ue::FindClass("PBZBulletLocationData_Standalone");
            offAreaOrigin_ = location ? ue::PropOffset(location, "Origin") : -1;
            auto* shape = ue::FindClass("CollisionShapeProfile");
            auto shapeOffset = [&](const char* name) { return shape ? ue::PropOffset(shape, name) : -1; };
            offShapeType_ = shapeOffset("CollisionType"); offShapeRadius_ = shapeOffset("Radius");
            offShapeHeight_ = shapeOffset("HalfHeight"); offShapeExtent_ = shapeOffset("Extent");
            timelineTaskCls_ = ue::FindClass("LGATask_PlayTimeline");
            offKnownTasks_ = ue::PropOffset(ue::FindClass("GameplayTasksComponent"), "KnownTasks");
            offTaskTimeline_ = timelineTaskCls_ ? ue::PropOffset(timelineTaskCls_, "Timeline") : -1;
            offTaskCursor_ = timelineTaskCls_ ? ue::PropOffset(timelineTaskCls_, "CurrentTime") : -1;
            offTaskAbility_ = timelineTaskCls_ ? ue::PropOffset(timelineTaskCls_, "Ability") : -1;
            auto* timeline = ue::FindClass("LGATimeline");
            offTimelineActions_ = timeline ? ue::PropOffset(timeline, "Actions") : -1;
            offStartBulletTimes_ = ue::PropOffset(durationCls_, "StartBulletTimes");
            offEndBulletTimes_ = ue::PropOffset(durationCls_, "EndBulletTimes");
            sweepDataCls_ = ue::FindClass("PBZ_SweepTraceBulletData");
            offDurationData_ = ue::PropOffset(durationCls_, "BulletData"); offInlineData_ = ue::PropOffset(durationCls_, "InlineBulletData");
            fnActionTimeline_ = pe::FindFunction(durationCls_, "GetTimeline");
            offActionBullets_ = ue::PropOffset(durationCls_,"Bullets");
            offObjectData_ = bulletObjCls_ ? ue::PropOffset(bulletObjCls_,"BulletData") : -1;
            sweepBulletCls_ = ue::FindClass("PBZ_SweepTraceBullet");
            sweepModuleCls_ = ue::FindClass("BulletSweepTraceModule");
            auto* module = ue::FindClass("BulletModuleBase");
            offSweepModule_ = sweepBulletCls_ ? ue::PropOffset(sweepBulletCls_,"SweepTraceModule") : -1;
            offModuleBullet_ = module ? ue::PropOffset(module,"Bullet") : -1;
            offSweepColliders_ = sweepModuleCls_ ? ue::PropOffset(sweepModuleCls_,"ColliderData") : -1;
            offSweepShapes_ = sweepModuleCls_ ? ue::PropOffset(sweepModuleCls_,"ShapeDataInterpolation") : -1;
            auto* helper = ue::FindClass("AttachColliderHelper");
            fnAttachShapeTransform_ = helper ? pe::FindFunction(helper,"GetAttachShapeTransform") : nullptr;
            attachHelperCdo_ = nullptr;
            ue::ForEach([&](int,ue::UObject* o) {
                if (ue::ObjName(o) == "Default__AttachColliderHelper") { attachHelperCdo_ = o; return false; }
                return true;
            });
            sceneCls_ = ue::FindClass("SceneComponent");
            fnComponentTransform_ = sceneCls_ ? pe::FindFunction(sceneCls_,"K2_GetComponentToWorld") : nullptr;
            auto* bulletInterface = ue::FindClass("BulletInterface");
            fnBulletTransform_ = bulletInterface ? pe::FindFunction(bulletInterface,"GetBulletTransform") : nullptr;
            capsuleCls_ = ue::FindClass("CapsuleComponent"); sphereCls_ = ue::FindClass("SphereComponent"); boxCls_ = ue::FindClass("BoxComponent");
            offCapsuleRadius_ = capsuleCls_ ? ue::PropOffset(capsuleCls_,"CapsuleRadius") : -1;
            offCapsuleHeight_ = capsuleCls_ ? ue::PropOffset(capsuleCls_,"CapsuleHalfHeight") : -1;
            offSphereRadius_ = sphereCls_ ? ue::PropOffset(sphereCls_,"SphereRadius") : -1;
            offBoxExtent_ = boxCls_ ? ue::PropOffset(boxCls_,"BoxExtent") : -1;
            auto layoutSize = [](const char* name,int size) {
                auto* c = ue::FindClass(name); return c && ue::Get<int32_t>(c+ue::Layout::StructSize) == size;
            };
            fnBreakHit_ = pe::FindFunction(ue::FindClass("GameplayStatics"),"BreakHitResult");
            contactAttackCls_ = ue::FindClass("BulletAttackModule");
            fnContactValid_ = pe::FindFunction(contactAttackCls_,"IsValidTarget");
            contactLayout_ = layoutSize("HitResult",0x100) && fnBreakHit_ &&
                Params(fnBreakHit_).Off("Hit") >= 0 && Params(fnBreakHit_).Off("HitActor") >= 0 && offActorData_ >= 0 && offActorSource_ >= 0 &&
                fnContactValid_ && offModuleBullet_ >= 0;
            collisionLayout_ = offActionBullets_ >= 0 && offObjectData_ >= 0 && offSweepModule_ >= 0 && offModuleBullet_ >= 0 &&
                offSweepColliders_ >= 0 && offSweepShapes_ >= 0 && attachHelperCdo_ && fnAttachShapeTransform_ && fnComponentTransform_ &&
                layoutSize("RuntimeAttachCollisionShape",0x80) && layoutSize("CollisionShapeWrapper",0x10) &&
                layoutSize("CharacterPoseCacheAttachData",0x18) && layoutSize("RuntimeShapeDataArrayWrapper",0x10) &&
                layoutSize("BulletSweepTestRuntimeShapeData",0xe0) && layoutSize("BulletSweepTestRuntimeColliderData",0x70);
            auto* event = ue::FindClass("LGAActionEvent");
            eventLayout_ = event && ue::Get<int32_t>(event+ue::Layout::StructSize) == 0x40 && ue::PropOffset(event,"StartTime") == 0 &&
                ue::PropOffset(event,"TriggerTimeOffset") == 8 && ue::PropOffset(event,"Action") == 0x10;
            // Received_Initialize 是蓝图事件，基类上未必有 UFunction：按名字在全部函数对象里找一个取 FName
            ue::ForEach([&](int, ue::UObject* o) {
                if (ue::ClassOf(o) != functionCls_ || ue::ObjName(o) != "Received_Initialize") return true;
                initName_ = ue::Get<uint32_t>(o + ue::Layout::ObjName);
                return false;
            });
            ue::ForEach([&](int, ue::UObject* o) { ConsiderInitFn(o); ConsiderContactFn(o); return true; });
            log::Info("监测：子弹生成监听 {} 个函数（新加载的类由增量扫描补挂）", initFns_.size());
            SyncNativeContact();
            log::Info("自动应对策略 contact-v15：蓝仅挡蓝/红仅躲红；真实帧专用反制优先；范围字段={}；回池状态={}；原生时间轴={}；BP接触监听={}；原生接触={}",
                offAreaShape_ >= 0 && offCustomTransform_ >= 0 && offShapeType_ >= 0, offActorStatus_ >= 0 && offObjectStatus_ >= 0,
                collisionLayout_ && eventLayout_ && fnActionTimeline_ && offStartBulletTimes_ >= 0 && offEndBulletTimes_ >= 0 && offKnownTasks_ >= 0 && offTaskCursor_ >= 0,contactFns_.size(),nativeEnabled_);
        } else {
            ClearExecutionWatch();
            SyncNativeContact();
            if (learnedDirty_) SaveLearned();
            for (int t : initTokens_) pe::Unobserve(t);
            for (int t : contactTokens_) pe::Unobserve(t);
            contactTokens_.clear(); contactFns_.clear(); contacts_.clear();
            initTokens_.clear(); initFns_.clear(); threats_.clear();
        }
    }
    void ResetDiagnostics() {
        exec_ = {}; // World time can restart on travel; do not retain an old execution deadline.
        executionNeedsDiscard_=false;executionScanAt_=-1;executionRunning_=ObjectRef();
        executionTagRetryAt_=hitReactTagRetryAt_=0;
        done_.clear(); pending_.clear(); accepted_.clear(); types_.clear(); firstSeen_.clear();
        windowSeen_.clear();
        counterRunning_=ObjectRef();
        requested_ = acceptedCount_ = contact_ = notAccepted_ = 0;
        ordinaryGuards_ = ordinaryDashes_ = 0;
        observed_ = {}; attempted_ = {}; admitted_ = {};
        nextSummary_ = 0; probeUntil_ = 0; logBudget_ = 400; recent_.clear(); monitorBudget_ = 800; nativeLogBudget_ = 100;
        threats_.clear(); nextTimed_ = 0;
        timelineWatches_.clear(); timelineBullets_.clear(); collisionFrames_.clear(); contacts_.clear();
        lastNativeBullet_=ObjectRef(); lastNativeContact_=-10;
        shiftById_.clear(); earlyById_.clear(); lateById_.clear(); learnedIds_.clear(); farLogged_.clear(); mixChoice_.clear();
        stanceAttempts_.clear();
    }
    void ReleaseAutoStance() {
        if(ownsStance_&&Blocked())return;
        releaseAt_ = 0;
        if (ownsStance_ && passiveRef_.Alive() && fnRelease_ && !(GetAsyncKeyState(VK_RBUTTON) & 0x8000))
            pe::Call(passive_, fnRelease_, nullptr);
        ownsStance_ = false;
    }
    void Prepare() {  // 开启时预先找对象、读配置，避免在闪避那一刻全表扫描
        observerThread_ = GetCurrentThreadId();
        LoadConfig();
        if (!Valid()) FindActors();
        if(Valid())WatchExecutionInput();
        if (Valid() && dodgeClasses_.empty()) CollectDodgeClasses();
    }
    void PolicyChanged() {
        ReleaseAutoStance();
        ResetDiagnostics();
        shachiScarce_ = false;
        SyncNativeContact();
        lastHp_ = -1; wasReacting_ = false; lastHurtAt_ = -10;
        hurt_ = hurtNoFrame_ = 0;
        log::Info("自动应对模式切换：blue={} red={} 普通模式={}（格挡1/闪避2/交替3；无帧仅属普通模式）",
            (flags & kBlue) != 0, (flags & kRed) != 0, ResponsePolicy(flags,Normal));
    }

    // Observe the native judgement only. Never force K2_CanActivateAbility true.
    void HookCanActivate(bool on) {
        if (!on) {
            for (auto* f : canFns_) pe::RemoveIntercept(f);
            canFns_.clear();
            return;
        }
        if (!canFns_.empty()) return;
        std::vector<ue::UObject*> classes;
        ue::ForEach([&](int, ue::UObject* o) {
            if (!ue::ClassName(o).ends_with("Class") || ue::IsDefault(o)) return true;
            const auto n = ue::ObjName(o);
            if (n.starts_with("GA_Hun_") && n.ends_with("_PerfectDodge_C") || n == "BP_Hun_PerfectDodge_ForProjectile_C") classes.push_back(o);
            return true;
        });
        for (auto* cls : classes) {
            auto* f = pe::FindFunction(cls, "K2_CanActivateAbility");
            if (!f || std::find(canFns_.begin(), canFns_.end(), f) != canFns_.end()) continue;
            canFns_.push_back(f);
            const int ret = Params(f).Off("ReturnValue");
            pe::Intercept(f, [this, f, ret](ue::UObject* obj, void* params) {
                if (GetCurrentThreadId() != observerThread_ || !playerRef_.Alive() || ue::Get<ue::UObject*>(obj + ue::Layout::ObjOuter) != player_) return false;
                const auto cn = ue::ClassName(obj);
                if (cn.find("_PerfectDodge") == std::string::npos || cn.find("Passive") != std::string::npos) return false;
                pe::Call(obj, f, params);
                if (ret >= 0 && params) {
                    auto& r = static_cast<uint8_t*>(params)[ret];
                    if (Probing() && (r || lastJudgeLog_ + 0.1 < lastNow_) && logBudget_-- > 0) {
                        lastJudgeLog_ = lastNow_;
                        log::Info("  完美闪避判定 {} → {}", ue::ObjName(obj), r);
                    }
                }
                return true;
            });
        }
        for (auto* cls : classes) {
            auto* f = pe::FindFunction(cls, "K2_ActivateAbility");
            if (!f || std::find(canFns_.begin(), canFns_.end(), f) != canFns_.end()) continue;
            canFns_.push_back(f);
            pe::Intercept(f, [this](ue::UObject* obj, void*) {
                if (GetCurrentThreadId() != observerThread_ || !playerRef_.Alive() || ue::Get<ue::UObject*>(obj + ue::Layout::ObjOuter) != player_) return false;
                if (ue::ClassName(obj).find("_PerfectDodge") != std::string::npos && Probing() && logBudget_-- > 0)
                    log::Info("  激活 {}", ue::ObjName(obj));
                return false;  // 只记录，原函数照常执行
            });
        }
        log::Info("完美闪避判定拦截：{} 个函数", canFns_.size());
    }

private:

    ue::UObject *passiveCls_ = nullptr, *deflectCls_ = nullptr, *durationCls_ = nullptr, *projCls_ = nullptr, *burstCls_ = nullptr;
    ue::UObject *fnTime_ = nullptr, *staticsCdo_ = nullptr;
    ue::UObject *passive_ = nullptr, *player_ = nullptr, *playerCls_ = nullptr;
    std::vector<ue::UObject*> dodgeClasses_, canFns_;
    bool Probing() const { return probeUntil_ > 0 && lastNow_ < probeUntil_; }
    double probeUntil_ = 0, lastNow_ = 0, lastJudgeLog_ = 0, dodgeLead_ = .08, redLead_ = .035, deflectLead_ = .045;
    double nextSummary_ = 0, projLead_ = .25, gap_ = 0;
    uint64_t requested_ = 0, acceptedCount_ = 0, contact_ = 0, notAccepted_ = 0;
    uint64_t ordinaryGuards_ = 0, ordinaryDashes_ = 0;
    std::array<uint64_t, 6> observed_{}, attempted_{}, admitted_{};
    bool ownsStance_ = false;
    std::map<AttackId, int> mixChoice_;  // 交替模式每一击的选择（1 格挡 / 2 闪避），只定一次
    int lastNormal_ = 0, mixDeflect_ = 85;  // 交替模式：上一次普攻应对（1 格挡 / 2 闪避）；格挡占比 %
    uint32_t rng_ = GetTickCount() | 1;
    int NextPercent() { rng_ ^= rng_ << 13; rng_ ^= rng_ >> 17; rng_ ^= rng_ << 5; return static_cast<int>(rng_ % 100); }
    std::array<uint32_t, 2> dashTag_{}, perfectDodgeTag_{};
    std::vector<ObjectRef> stanceAbilities_, protectedAbilities_, dashAbilities_;
    ObjectRef counterRunning_;
    ObjectRef playerRef_, passiveRef_, dodgePassiveRef_, sgameRef_, aicRef_;
    ue::UObject *fnAvatar_ = nullptr, *fnLocal_ = nullptr;
    int offAicAsc_ = -1, offSgameAsc_ = -1;
    DWORD observerThread_ = 0;
    ue::UObject *fnGiveWindow_ = nullptr, *fnActivateStance_ = nullptr, *fnForceTag_ = nullptr;
    ue::UObject *fnForceClass_ = nullptr, *perfectBaseCls_ = nullptr;
    int offStanceTag_ = -1;
    int priority_ = 100;
    ue::UObject *aicCls_ = nullptr, *aic_ = nullptr;
    ue::UObject *sgameCls_ = nullptr, *sgame_ = nullptr, *iaDodge_ = nullptr, *iaDeflect_ = nullptr;
    ue::UObject *dodgePassiveCls_ = nullptr, *dodgePassive_ = nullptr, *fnKeyDown_ = nullptr;
    std::map<AttackId, int> types_;
    std::map<AttackId, double> windowSeen_;
    int redLog_ = 120, tagsOffset_ = -1, scanCursor_ = 0;
    AbilityLists building_;
    std::set<AttackId> pending_, accepted_;
    ue::UObject* fnRelease_ = nullptr;
    double releaseAt_ = 0;
    std::set<AttackId> firstSeen_;
    int logBudget_ = 400;  // 诊断日志上限，防刷屏
    int offComp_ = -1, offFrames_ = -1, offPerfect_ = -1, offNormal_ = -1, offClick_ = -1, offCounter_ = -1;
    int offDurType_ = -1, offProjType_ = -1, offBurstType_ = -1, offAsc_ = -1;
    float since_ = 1e9f;
    std::set<AttackId> done_;
};

Engine& E() {
    static Engine e;
    return e;
}

class AutoFeature : public IFeature {
public:
    AutoFeature(const char* id, int flag) : id_(id), flag_(flag) {}
    const char* Id() const override { return id_; }
    bool IsToggle() const override { return true; }
    bool Resolve(std::string* why) override {
        if (flag_ & (kPerfectDeflect | kPerfectDodge)) {
            *why = "旧强制判定方案已证伪，禁止修改窗口时间戳/强制CanActivate；待独立重做";
            return false;
        }
        return E().Resolve(why);
    }
    void Enable() override {
        if (flag_ & (kPerfectDeflect | kPerfectDodge)) return;
        if (flag_ & (kNormalDeflect | kNormalDodge | kNormalMix))  // 普攻三种模式互斥
            for (const char* other : {"player.auto_deflect_normal", "player.auto_dodge_normal", "player.auto_mix_normal"})
                if (auto* f = FindFeature(other); f && f != this && f->enabled) f->Disable();
        const bool dodgeBefore = E().flags & kDodgeFlags;
        E().flags |= flag_;
        enabled = true;
        E().PolicyChanged();
        // Only install read-only judgement/activation observers.
        if (!dodgeBefore && (E().flags & kDodgeFlags)) E().HookCanActivate(true);
        E().Prepare();
        E().Monitor(true);
    }
    void Disable() override {
        E().flags &= ~flag_;
        enabled = false;
        E().PolicyChanged();
        if (!(E().flags & kDeflectFlags)) E().ReleaseAutoStance();
        if ((flag_ & kDodgeFlags) && !(E().flags & kDodgeFlags)) E().HookCanActivate(false);
        if (!E().flags) { E().Monitor(false); E().ResetDiagnostics(); }
    }
    void Tick(float dt) override {
        // 四个开关共用一个引擎：只由排在最前面的已启用开关驱动，避免一帧跑多次
        for (auto& f : Features()) {
            if (auto* a = dynamic_cast<AutoFeature*>(f.get()); a && a->enabled) {
                if (a == this) E().Tick(dt);
                return;
            }
        }
    }

private:
    const char* id_;
    int flag_;
};

#endif  // PBZ_AUTO_RESPONSE_TEST

}  // namespace

#ifndef PBZ_AUTO_RESPONSE_TEST
void RegisterAutoResponseFeatures() {
    auto& F = Features();
    F.push_back(std::make_unique<AutoFeature>("player.auto_deflect_blue", kBlue));
    F.push_back(std::make_unique<AutoFeature>("player.auto_dodge_red", kRed));
    F.push_back(std::make_unique<AutoFeature>("player.auto_deflect_normal", kNormalDeflect));
    F.push_back(std::make_unique<AutoFeature>("player.auto_dodge_normal", kNormalDodge));
    F.push_back(std::make_unique<AutoFeature>("player.auto_mix_normal", kNormalMix));
    F.push_back(std::make_unique<AutoFeature>("player.auto_attack", kAutoAttack));
}
#endif

}  // namespace pbz

#ifdef PBZ_AUTO_RESPONSE_TEST
// Isolated tests compile this same file without Engine/registration. They do
// not open the game or invoke any native gameplay function.
#include <iostream>
#include <stdexcept>
namespace pbz::ue {
bool Read(const void* p, void* out, size_t n) {
    if (!p) return false;
    memcpy(out, p, n); return true;
}
}
template<class T> void TestPut(uint8_t* p, int offset, T value) { memcpy(p + offset, &value, sizeof value); }
int main() try {
    int checks = 0;
    auto check = [&](bool ok, const char* why) { ++checks; if (!ok) throw std::runtime_error(why); };
    std::array<uint8_t, 0x38> h{};
    std::array<uint8_t, 0x6000> storage{};
    pbz::ReceiveSlots view;
    TestPut(h.data(), 0, storage.data()); TestPut(h.data(), 8, 12); TestPut(h.data(), 12, 23);
    TestPut(h.data(), 0x28, 12); TestPut(h.data(), 0x2c, 128); TestPut(h.data(), 0x34, 12);
    check(pbz::ReadReceiveSlots(h.data(), view), "real empty sparse-array shape");
    for (int i = 0; i < 12; ++i) check(!view.Contains(i), "freed slot visited");
    TestPut(h.data(), 8, 6); TestPut(h.data(), 0x28, 6); TestPut(h.data(), 0x34, 4); TestPut(h.data(), 0x10, uint32_t(9));
    check(pbz::ReadReceiveSlots(h.data(), view) && view.Contains(0) && view.Contains(3) && !view.Contains(1) && !view.Contains(6) && !view.Contains(-1), "inline allocation bitmap");
    const auto old = view;
    TestPut(h.data(), 0x34, 5);
    check(!pbz::ReadReceiveSlots(h.data(), view) && view.count == old.count, "invalid layout changes valid output");
    std::array<uint32_t, 8> heap{}; heap[0] = 1u << 7; heap[4] = 1u << 1;
    TestPut(h.data(), 8, 140); TestPut(h.data(), 12, 146); TestPut(h.data(), 0x28, 140);
    TestPut(h.data(), 0x2c, 256); TestPut(h.data(), 0x34, 138); TestPut(h.data(), 0x20, heap.data());
    check(pbz::ReadReceiveSlots(h.data(), view) && view.Contains(7) && view.Contains(129) && !view.Contains(128), "heap allocation bitmap");
    TestPut(h.data(), 0x28, 139); check(!pbz::ReadReceiveSlots(h.data(), view), "mismatched bit count");
    TestPut(h.data(), 8, 257); TestPut(h.data(), 0x28, 257); check(!pbz::ReadReceiveSlots(h.data(), view), "oversized frame map");
    h = {}; check(pbz::ReadReceiveSlots(h.data(), view) && view.count == 0, "zeroed empty map");
    {   // 处决闸门：处决中拦下；结束后 0.6 秒内仍拦；之后放行；进入 / 恢复各报一次
        pbz::ExecGate g;
        check(g.Update(10.0, false) == 0 && !g.Block(), "not executing does not block");
        check(g.Update(10.1, true) == 1 && g.Block() && g.Block(), "execution blocks every response");
        check(g.Update(11.0, true) == 0 && g.Block(), "still executing");
        check(g.Update(11.5, false) == 0 && g.Block(), "grace after execution still blocks (victim frames / learned schedules)");
        check(g.Update(11.61, false) == -1 && !g.Block() && g.suppressed == 4, "resumes after grace and counts suppressed requests");
        check(g.Update(12.0, true) == 1 && g.suppressed == 0, "next execution starts a fresh count");
    }
    {
        for(const char* tag:{"Ability.Action.Execution","Ability.Action.Execution.Combat","Ability.Action.Execution.Stealth"})
            check(pbz::ExecutionTag(tag),"native execution tag family is not protected");
        for(const char* tag:{"Ability.Action.Executioner","Ability.Action.Hun.Dash","CharacterState.Immortal","Ability.Executed",""})
            check(!pbz::ExecutionTag(tag),"unrelated invulnerability or ability is falsely treated as player execution");
        check(pbz::ExecutionBase("GA_SimpleExecution_Base_C")&&pbz::ExecutionBase("GA_ExecutionAbilityBase_C"),"SDK execution base families are not covered");
        check(!pbz::ExecutionBase("BP_PlayerPerfectDodge_Base_C")&&!pbz::ExecutionBase("GA_EnemyExecuted_C")&&!pbz::ExecutionBase("BP_Hun_PerfectDodge_ExecutionBase_C"),"ordinary/red counter or enemy victim is falsely classified as a player finisher merely by its name");
        pbz::ExecGate rejected;
        check(rejected.Update(20,false)==0&&!rejected.Block(),"ordinary frame before player request is blocked");
        check(rejected.Pulse(20.001,.18)==1&&rejected.Block(),"execution request between Tick and native contact is not immediately protected");
        check(rejected.Update(20.10,false)==0&&rejected.Block(),"request grace ended before native activation could run");
        check(rejected.Update(20.182,false)==-1&&!rejected.Block(),"rejected native execution request stalls ordinary defense indefinitely");
        pbz::ExecGate longAnimation;
        check(longAnimation.Pulse(30,.18)==1,"native execution begin fails to arm priority");
        for(int i=0;i<=200;++i)check(longAnimation.Update(30+i*.05,true)==0&&longAnimation.Block(),"long execution released automatic response at the old1.5-second continuation limit");
        check(longAnimation.Update(40.59,false)==0&&longAnimation.Block(),"full end-of-execution grace lost");
        check(longAnimation.Update(40.61,false)==-1&&!longAnimation.Block(),"normal defense fails to resume after cinematic ends");
        check(pbz::RetireExecutionFrame(50,50)&&pbz::RetireExecutionFrame(49,50),"already elapsed cinematic attack is not retired");
        check(!pbz::RetireExecutionFrame(50.001,50)&&!pbz::RetireExecutionFrame(51,50),"real attack landing after execution is wrongly discarded");
        check(!pbz::RetireExecutionFrame(pbz::kWindowHitTime,50)&&!pbz::RetireExecutionFrame(NAN,50),"untimed counter window or invalid timestamp is treated as an elapsed attack");
    }
    std::array<uint8_t, 0x50> frame{}; TestPut(frame.data(), 0, uint64_t(0x123400000123));
    const auto a = pbz::Identity(10, frame.data(), 1000), b = pbz::Identity(11, frame.data(), 1000);
    const auto mirror = pbz::Identity(-1, frame.data(), 1000);
    check(a != b, "simultaneous handles merged"); check(pbz::SameMirror(a, mirror), "mirror identity lost");
    check(a != pbz::Identity(10, frame.data(), 1000.0001), "sub-millisecond identity lost");
    TestPut(frame.data(), 4, uint32_t(0x2345));
    check(!pbz::SameMirror(a, pbz::Identity(-1, frame.data(), 1000)), "weak serial ignored");
    const uint64_t capturedWindowBits=0xc7eff933c0000000ULL;
    double capturedWindow=0; memcpy(&capturedWindow,&capturedWindowBits,sizeof capturedWindow);
    check(capturedWindow==pbz::kWindowHitTime,"captured native untimed sentinel differs");
    check(pbz::WindowCounterFrame(5150,pbz::RedEye,true,true,capturedWindow),"real grenade counter window rejected as expired");
    for (int type=0;type<6;++type)
        check(pbz::WindowCounterFrame(5150,type,true,true,capturedWindow)==(type==pbz::RedEye),"untimed window borrows another attack type");
    check(!pbz::WindowCounterFrame(-1,pbz::RedEye,true,true,capturedWindow),"mirror starts counter");
    check(!pbz::WindowCounterFrame(5150,pbz::RedEye,false,true,capturedWindow),"nondodge frame starts counter");
    check(!pbz::WindowCounterFrame(5150,pbz::RedEye,true,false,capturedWindow),"invalid counter class starts counter");
    check(!pbz::WindowCounterFrame(5150,pbz::RedEye,true,true,-1),"ordinary stale time treated as native window");
    check(!pbz::WindowCounterFrame(5150,pbz::RedEye,true,true,NAN),"invalid time starts counter");
    const auto window=pbz::Identity(5150,frame.data(),capturedWindow);
    check(std::get<2>(window)==pbz::kWindowStamp,"sentinel overflows microsecond identity");
    check(window!=pbz::Identity(5164,frame.data(),capturedWindow),"independent counter windows merged");
    check(!pbz::AttackExpired(pbz::kWindowStamp,100,101),"live counter window loses done state");
    check(pbz::AttackExpired(pbz::kWindowStamp,100,103),"removed counter window leaks done state");
    check(!pbz::AttackExpired(pbz::kWindowStamp,103,103),"long counter window repeats accepted action");
    check(pbz::AttackExpired(100000000,0,103),"ordinary timestamp expiry changed");
    for (int mode : {pbz::kNormalDeflect,pbz::kNormalDodge,pbz::kNormalMix}) {
        check(!pbz::OrdinaryThreatPermission(mode,pbz::Normal,false,true,false),"timeline prediction races spawn fallback");
        check(!pbz::OrdinaryThreatPermission(mode,pbz::Normal,true,false,false),"real frame prediction races spawn fallback");
        check(pbz::OrdinaryThreatPermission(mode,pbz::Normal,true,true,true),"native contact rejected by prediction ownership");
        check(pbz::OrdinaryThreatPermission(mode,pbz::CantDeflect,false,true,true),"native explosion rejected by timeline ownership");
        check(pbz::OrdinaryThreatPermission(mode,-1,false,false,true),"unknown real particle loses ordinary fallback");
        check(!pbz::OrdinaryThreatPermission(mode,pbz::RedEye,false,true,true),"ordinary native contact borrows red permission");
        check(!pbz::OrdinaryThreatPermission(mode,pbz::BlueEye,false,true,true),"ordinary native contact borrows blue permission");
    }
    check(!pbz::OrdinaryThreatPermission(pbz::kRed|pbz::kBlue,pbz::Normal,false,false,true),"colored toggles authorize ordinary contact");
    pbz::Incoming red, blue, normal;
    red.type = pbz::RedEye; red.hit = 1000.012; red.id = a;
    blue.type = pbz::BlueEye; blue.hit = 1000.005; blue.id = b;
    normal.type = pbz::Normal; normal.hit = 1000.003; normal.id = mirror;
    auto order = [&](const auto& x) { return pbz::ResponseOrder(x, 1000, .016); };
    check(order(red) < order(blue) && order(blue) < order(normal), "same-frame danger ordering");
    red.hit = 1000.035; check(order(blue) < order(red), "later red steals earlier frame");
    std::array<pbz::Incoming, 3> items{red, blue, normal};
    std::sort(items.begin(), items.end(), [&](const auto& x, const auto& y) { return order(x) < order(y); });
    check(items.front().type == pbz::BlueEye, "urgent candidate not first");
    check(!(order(items[0]) < order(items[0])), "non-strict comparator");
    check(order(items[0]) < order(items[1]) && order(items[1]) < order(items[2]) && order(items[0]) < order(items[2]), "non-transitive comparator");
    check(std::abs(pbz::TimingShift("弹:slow",1.54)-1.54)<1e-9, "slow projectile timing truncated on reload");
    check(std::abs(pbz::TimingShift("actor|1",1.54)-0.6)<1e-9, "melee correction unbounded");
    check(pbz::TimingShift("弹:slow",99)==3, "projectile timing unbounded");
    const double slowPos[3]={1000,0,0}, slowVel[3]={-100,0,0};
    check(std::abs(pbz::ProjectileContact(slowPos,slowVel,85,120)-9.15)<1e-9, "slow flight must wait for physical contact");
    const double fastVel[3]={-10000,0,0};
    check(std::abs(pbz::ProjectileContact(slowPos,fastVel,85,120)-0.0915)<1e-9, "fast projectile timing");
    const double missPos[3]={1000,200,0};
    check(pbz::ProjectileContact(missPos,fastVel,85,120)<0, "passing projectile triggers dodge");
    const double leavingVel[3]={100,0,0}, insidePos[3]={50,0,0};
    check(pbz::ProjectileContact(insidePos,leavingVel,85,120)<0, "departing nearby projectile triggers dodge");
    const double highPos[3]={1000,0,500};
    check(pbz::ProjectileContact(highPos,fastVel,85,120)<0, "overhead miss triggers dodge");
    const double fallingPos[3]={0,0,1000}, fallingVel[3]={0,0,-1000};
    check(std::abs(pbz::ProjectileContact(fallingPos,fallingVel,85,120)-0.88)<1e-9, "falling impact timing");
    const double crossingPos[3]={-100,0,0}, crossingVel[3]={-200,0,0};
    check(pbz::ProjectileContact(crossingPos,crossingVel,85,120)<0, "relative escape trajectory");
    check(pbz::ProjectileContact(insidePos,slowVel,85,120)==0, "imminent contact dropped");
    check(pbz::TimedRetryReady(9.95,10,.08,9.90,false), "rejected timed action must retry within window");
    check(!pbz::TimedRetryReady(9,10,.08,0,false), "timed dodge too early");
    check(!pbz::TimedRetryReady(10.2,10,.08,0,false), "expired timed attack retried");
    check(!pbz::TimedRetryReady(9.95,10,.08,0,true), "accepted timed attack repeated");
    check(!pbz::TimedRetryReady(9.95,10,.08,9.94,false), "rejected request retry spin");
    check(std::abs(pbz::TimelineRate(1.2,1,.1)-2)<1e-9, "actual animation speed ignored");
    check(pbz::TimelineRate(1,1,.1)==0, "charge hold treated as ticking timer");
    check(pbz::TimelineRate(.1,2,.1)==0, "timeline restart interpreted as impact");
    check(pbz::TimelineRate(1.2,1,0)==0, "paused world predicts impact");
    check(pbz::TimelineRate(1.2,1,1)==0, "stale timeline sample extrapolated");
    std::array<uint8_t,16> impactHeader{};
    float prepared[]={0,0.5f,1.25f};
    TestPut(impactHeader.data(),0,prepared); TestPut(impactHeader.data(),8,3); TestPut(impactHeader.data(),12,3);
    std::vector<float> schedule;
    check(pbz::ReadImpactSchedule(impactHeader.data(),schedule) && schedule.size()==3 && schedule[0]==0, "instant unguardable runtime schedule rejected");
    // Captured charge: event start2.293437, prepared StartBulletTimes[0]=0,
    // RuntimePreHitPointDuration=0. It must schedule BEFORE the action begins.
    check(std::abs(pbz::TimelineContact(2.293437,0,2.243437,1)-.05)<1e-6, "charge waits until instant damage already began");
    check(std::abs(pbz::TimelineContact(2.293437,.5,2.243437,1)-.55)<1e-6, "repeated delayed hit collapsed into first hit");
    check(std::abs(pbz::TimelineContact(2,0,1.9,.5)-.2)<1e-6, "slow charge ignores actual timeline rate");
    check(!std::isfinite(pbz::TimelineContact(2,0,1.9,0)), "held charge starts a wall-clock countdown");
    check(!std::isfinite(pbz::TimelineContact(2,0,1.9,-1)), "rewound charge predicts a hit");
    check(!std::isfinite(pbz::TimelineContact(NAN,0,1.9,1)), "invalid event start schedules dodge");
    prepared[1]=-1; check(!pbz::ReadImpactSchedule(impactHeader.data(),schedule), "negative runtime hit accepted");
    prepared[1]=1.5f; check(!pbz::ReadImpactSchedule(impactHeader.data(),schedule), "unordered runtime schedule accepted");
    prepared[1]=NAN; check(!pbz::ReadImpactSchedule(impactHeader.data(),schedule), "invalid runtime hit accepted");
    prepared[1]=.5f;
    TestPut(impactHeader.data(),8,0); check(!pbz::ReadImpactSchedule(impactHeader.data(),schedule), "empty template used as prepared runtime schedule");
    TestPut(impactHeader.data(),8,65); check(!pbz::ReadImpactSchedule(impactHeader.data(),schedule), "unbounded runtime hit count");
    TestPut(impactHeader.data(),8,3); TestPut(impactHeader.data(),12,2); check(!pbz::ReadImpactSchedule(impactHeader.data(),schedule), "invalid schedule capacity accepted");
    // Live ordinary sweep: its nominal pre-hit .326535 lies 250ms AFTER
    // collision starts, which remains active for another 250ms afterwards.
    float nativeStart[]={.0765347f,.8f}, nativeEnd[]={.5765347f,1.1f};
    std::array<uint8_t,16> startHeader{},endHeader{};
    TestPut(startHeader.data(),0,nativeStart); TestPut(startHeader.data(),8,2); TestPut(startHeader.data(),12,2);
    TestPut(endHeader.data(),0,nativeEnd); TestPut(endHeader.data(),8,2); TestPut(endHeader.data(),12,2);
    std::vector<std::pair<float,float>> windows;
    check(pbz::ReadCollisionWindows(startHeader.data(),endHeader.data(),windows) && windows.size()==2, "captured collision windows rejected");
    const double beginTime=10+windows[0].first,endTime=10+windows[0].second;
    check(pbz::FrameWithinCollision(10.3265347,beginTime,endTime), "nominal pre-hit not recognized inside physical window");
    check(pbz::CollisionWindowDue(beginTime-10.4,endTime-10.4,.08), "later entry into active sweep ignored");
    check(!pbz::CollisionWindowDue(beginTime-9.8,endTime-9.8,.08), "wind-up triggers physical response");
    check(!pbz::CollisionWindowDue(beginTime-10.7,endTime-10.7,.08), "expired physical collision still active");
    check(!pbz::FrameWithinCollision(10.8,beginTime,endTime), "next combo frame consumed by first interval");
    check(!pbz::FrameWithinCollision(NAN,beginTime,endTime), "invalid frame covered by native window");
    check(!pbz::CollisionWindowDue(.1,-.1,.08), "backwards physical interval accepted");
    check(!pbz::CollisionWindowDue(0,NAN,.08), "invalid collision end accepted");
    nativeEnd[0]=.01f; check(!pbz::ReadCollisionWindows(startHeader.data(),endHeader.data(),windows), "collision end before start accepted");
    nativeEnd[0]=.5765347f;
    TestPut(endHeader.data(),8,1); check(!pbz::ReadCollisionWindows(startHeader.data(),endHeader.data(),windows), "unpaired collision starts and ends accepted");
    TestPut(endHeader.data(),8,2); nativeEnd[1]=NAN;
    check(!pbz::ReadCollisionWindows(startHeader.data(),endHeader.data(),windows), "nonfinite collision end accepted");
    check(pbz::CollisionRetry(10,-10,-1,false,false,false), "first physical collision attempt suppressed");
    check(!pbz::CollisionRetry(10.02,10,-1,false,false,false), "rejected collision attempt spins");
    check(!pbz::CollisionRetry(10.4,10,10,true,true,false), "held guard gets released and clicked again");
    check(pbz::CollisionRetry(10.4,10,10,true,false,false), "interrupted guard never retried during sustained hit");
    check(!pbz::CollisionRetry(10.4,10,10,false,false,true), "native dash cancelled during collision");
    check(!pbz::CollisionRetry(10.2,10,10,false,false,false), "accepted dash repeated before minimum spacing");
    check(pbz::CollisionRetry(10.4,10,10,false,false,false), "lingering hazard ignored after native dash ends");
    check(pbz::CollisionRetry(10.56,10,10,false,false,pbz::DashBlocksResponse(true,true)), "captured cancel-phase dash blocks later ground wave");
    check(!pbz::CollisionRetry(10.56,10,10,false,false,pbz::DashBlocksResponse(true,false)), "uncancellable dash interrupted by later wave");
    check(!pbz::CollisionRetry(10.2,10,10,false,false,pbz::DashBlocksResponse(true,true)), "cancel phase removes minimum retry spacing");
    check(!pbz::DashBlocksResponse(false,false), "ended dash blocks red follow-up");
    const double guardWindow = pbz::ResponseLead(pbz::Normal,1,.14,.04,.08,.20,.016);
    const double dodgeWindow = pbz::ResponseLead(pbz::Normal,2,.14,.04,.08,.20,.016);
    check(guardWindow > .19 && guardWindow < .20, "guard early allowance lost");
    check(dodgeWindow > .09 && dodgeWindow < .10, "guard learning makes dodge 220ms early");
    check(.22 > dodgeWindow && .08 <= dodgeWindow, "slow attack dodge must wait for its own window");
    check(pbz::ResponseLead(pbz::RedEye,2,.4,.04,.08,.20,.016) > .21, "red counter window replaced by ordinary dodge");
    check(pbz::ResponseLead(pbz::Normal,2,.4,.04,.08,.20,.25) <= .121, "long frame carries guard allowance into dodge");
    check(pbz::ResponseLead(pbz::CantDeflect,2,.4,.04,.08,.20,0) == .08, "unguardable inherits learned guard allowance");
    check(pbz::FrameSchedulesThreat(4,pbz::Normal,false,10.5,10), "known live frame loses ownership");
    check(pbz::FrameSchedulesThreat(4,pbz::BlueEye,false,10.5,10), "blue frame ownership lost");
    check(!pbz::FrameSchedulesThreat(4,-1,false,10.5,10), "unknown ReleaseBullet frame blocks trajectory response");
    check(!pbz::FrameSchedulesThreat(-1,pbz::Normal,false,10.5,10), "mirror blocks trajectory response");
    check(!pbz::FrameSchedulesThreat(4,pbz::Normal,true,10.5,10), "contacted frame blocks trajectory response");
    check(!pbz::FrameSchedulesThreat(4,pbz::Normal,false,9,10), "expired frame blocks later physical contact");
    check(!pbz::FrameSchedulesThreat(4,pbz::Normal,false,NAN,10), "invalid frame owns projectile");
    std::array<uint8_t,16> nativeShape{};
    TestPut(nativeShape.data(),0,3); TestPut(nativeShape.data(),4,80.f); TestPut(nativeShape.data(),8,2500.f);
    int decodedShape=-1; double decodedRadius=0,decodedHeight=0,decodedExtent[3]{};
    check(pbz::DecodeNativeShape(nativeShape.data(),decodedShape,decodedRadius,decodedHeight,decodedExtent) && decodedShape==1 && decodedRadius==80 && decodedHeight==2500,
        "captured long cannon capsule decoded as profile or radial range");
    // A sideways capsule reaches far along its axis, not every direction within
    // the same root distance. Quaternion rotates local Z onto world X.
    double longTransform[12]={0,std::sqrt(.5),0,std::sqrt(.5),0,0,0,0,1,1,1,0};
    const double axisHit[3]={900,0,0}, sideMiss[3]={0,900,0}, behindTip[3]={2800,0,0};
    check(pbz::AreaContains(axisHit,longTransform,decodedShape,decodedRadius,decodedHeight,decodedExtent), "long cannon hit rejected because root travel is zero");
    check(!pbz::AreaContains(sideMiss,longTransform,decodedShape,decodedRadius,decodedHeight,decodedExtent), "long cannon turned into a radial attack");
    check(!pbz::AreaContains(behindTip,longTransform,decodedShape,decodedRadius,decodedHeight,decodedExtent), "capsule length ignored at endpoint");
    longTransform[8]=longTransform[9]=longTransform[10]=1.2;
    check(pbz::AreaContains(behindTip,longTransform,decodedShape,decodedRadius,decodedHeight,decodedExtent), "native scaled collision size ignored");
    TestPut(nativeShape.data(),0,2);
    check(pbz::DecodeNativeShape(nativeShape.data(),decodedShape,decodedRadius,decodedHeight,decodedExtent) && decodedShape==0, "native sphere type mismatch");
    TestPut(nativeShape.data(),0,1); TestPut(nativeShape.data(),4,200.f); TestPut(nativeShape.data(),8,50.f); TestPut(nativeShape.data(),12,60.f);
    check(pbz::DecodeNativeShape(nativeShape.data(),decodedShape,decodedRadius,decodedHeight,decodedExtent) && decodedShape==2 && decodedExtent[0]==200 && decodedExtent[2]==60, "native box union mismatch");
    TestPut(nativeShape.data(),0,0); check(!pbz::DecodeNativeShape(nativeShape.data(),decodedShape,decodedRadius,decodedHeight,decodedExtent), "line/unknown collision guessed as capsule");
    TestPut(nativeShape.data(),0,3); TestPut(nativeShape.data(),4,80.f); TestPut(nativeShape.data(),8,20.f);
    check(!pbz::DecodeNativeShape(nativeShape.data(),decodedShape,decodedRadius,decodedHeight,decodedExtent), "invalid capsule accepted");
    TestPut(nativeShape.data(),8,NAN); check(!pbz::DecodeNativeShape(nativeShape.data(),decodedShape,decodedRadius,decodedHeight,decodedExtent), "nonfinite capsule accepted");
    // 用户定义的五项单开契约：列为不可挡/普通/蓝/红/重击/长按；额外开自动攻击不扩张范围。
    const int modes[5]={pbz::kBlue,pbz::kRed,pbz::kNormalDeflect,pbz::kNormalDodge,pbz::kNormalMix};
    const int expected[5][6]={{0,0,1,0,0,0},{0,0,0,2,0,0},{2,1,0,0,0,1},{2,2,0,0,0,2},{2,3,0,0,0,3}};  // 黑光（第 5 列）不再自动应对
    for (int mode=0;mode<256;++mode) {
        check(!pbz::ContactPermission(mode,pbz::BlueEye) && !pbz::ContactPermission(mode,pbz::RedEye), "physical contact path steals colored attacks");
        check(pbz::ContactPermission(mode,-1)==bool(mode&pbz::kNormalFlags), "unknown real contact expands colored-only scope");
        check(pbz::ContactPermission(mode,pbz::Normal)==bool(mode&pbz::kNormalFlags), "real ordinary contact ignores normal-mode permission");
        check(!pbz::ContactPermission(mode,255), "invalid contact type accepted");
        check(pbz::PreciseContactPermission(mode,pbz::BlueEye)==bool(mode&pbz::kBlue), "native blue contact borrows other switches");
        check(pbz::PreciseContactPermission(mode,pbz::RedEye)==bool(mode&pbz::kRed), "native red contact borrows other switches");
        check(pbz::PreciseContactPermission(mode,pbz::Normal)==bool(mode&pbz::kNormalFlags), "colored-only native route catches ordinary attack");
        check(pbz::PreciseContactPermission(mode,-1)==bool(mode&pbz::kNormalFlags), "unknown native contact expands colored permission");
        check(!pbz::PreciseContactPermission(mode,255), "invalid native contact expands permission");
    }
    for (int mode=0;mode<5;++mode) for (int type=0;type<6;++type) {
        check(pbz::ResponsePolicy(modes[mode],type)==expected[mode][type], "single-mode attack scope violated");
        check(pbz::ResponsePolicy(modes[mode]|pbz::kAutoAttack,type)==expected[mode][type], "auto attack expands response permissions");
    }
    for (int mode:modes) {
        check(pbz::ResponsePolicy(mode,-1)==0 && pbz::ResponsePolicy(mode,255)==0, "unknown frame classified as normal");
    }
    for (int flags=0;flags<256;++flags) {
        check(pbz::ResponsePolicy(flags,pbz::BlueEye)==((flags&pbz::kBlue)?1:0), "ordinary mode catches blue");
        check(pbz::ResponsePolicy(flags,pbz::RedEye)==((flags&pbz::kRed)?2:0), "ordinary mode catches red");
        check(pbz::ResponsePolicy(flags,pbz::PowerAttack)==0, "black power attacks get no auto response (2026-11-06: black-light dodge removed)");
        if ((flags&pbz::kNormalDeflect)&&(flags&pbz::kNormalDodge))
            check(pbz::ResponsePolicy(flags,pbz::Normal)==3, "guard + dodge both on: guard-first mix (2026-11-06)");
        check(pbz::PreciseContactPermission(flags,pbz::PowerAttack)==bool(flags&pbz::kRed), "black real contact borrows guard/normal permissions");
        check(!pbz::OrdinaryThreatPermission(flags,pbz::PowerAttack,false,false,true), "black contact is treated as an ordinary guard fallback");
        check(pbz::ResponsePolicy(flags,pbz::CantDeflect)==((flags&pbz::kNormalFlags)?2:0), "colored-only mode dodges uncolored attacks");
    }
    check(!pbz::Guardable(pbz::PowerAttack)&&!pbz::NormalType(pbz::PowerAttack),"black attacks can drain shachi: never select guard or guard fallback");
    check(pbz::AttackTagType("Bullet.Tag.Feature.Deflect.PowerAttack")==pbz::PowerAttack,"native power-attack tag not recognized");
    check(pbz::AttackTagType("Bullet.Tag.Feature.Deflect.PowerAttackOther")==-1,"unrelated tag classified as black");
    for(int first=-1;first<6;++first)for(int second=-1;second<6;++second){
        check(pbz::MergeAttackColor(pbz::MergeAttackColor(first,second),pbz::PowerAttack)==pbz::PowerAttack,"black payload loses precedence over inherited blue/red tags");
        check(pbz::MergeAttackColor(pbz::PowerAttack,pbz::MergeAttackColor(first,second))==pbz::PowerAttack,"later tag clears black classification");
    }
    check(pbz::ExecutionBase("GA_ExactExecutionBase"),"native exact execution ability is not protected");
    for (int type=0;type<6;++type) check(pbz::ResponsePolicy(pbz::kAutoAttack,type)==0, "auto attack implies defense");
    check(!pbz::SpawnTimingAllowed(true,true,false,false), "particle emitter treated as a timed projectile");
    check(!pbz::SpawnTimingAllowed(true,true,true,true), "moving emitter treated as a point projectile");
    check(!pbz::SpawnTimingAllowed(false,false,false,false), "object sweep learned as a projectile");
    check(!pbz::SpawnTimingAllowed(true,false,true,true), "measured trajectory replaced by spawn timing");
    check(pbz::SpawnTimingAllowed(true,false,false,false), "unpositioned real projectile fallback removed");
    double axis[3]={3,4,0}, sweep[3]={0,2,0}, escape[3]={};
    check(pbz::LateralEscape(axis,sweep,1,escape) && std::abs(escape[0]+.8)<1e-9 && std::abs(escape[1]-.6)<1e-9, "beam dodge runs along the ray");
    check(pbz::LateralEscape(axis,sweep,-1,escape) && std::abs(escape[0]-.8)<1e-9 && std::abs(escape[1]+.6)<1e-9, "opposite lateral direction lost");
    axis[0]=axis[1]=0;
    check(pbz::LateralEscape(axis,sweep,1,escape) && escape[0]==-1 && escape[1]==0, "zero attack direction does not use sweep");
    check(!pbz::LateralEscape(axis,axis,1,escape), "zero vectors invented a beam direction");
    axis[0]=NAN;
    check(!pbz::LateralEscape(axis,nullptr,1,escape), "invalid native vector accepted");
    std::array<uint8_t,768> attackThunk{};
    memcpy(attackThunk.data()+521,pbz::kAttackThunkTail,sizeof(pbz::kAttackThunkTail));
    check(pbz::FindAttackCall(attackThunk.data(),attackThunk.size())==567, "captured TryAttack ABI thunk not resolved");
    attackThunk[568]^=0x4a;
    check(pbz::FindAttackCall(attackThunk.data(),attackThunk.size())==567, "thunk relocation rejected");
    attackThunk[524]^=1;
    check(pbz::FindAttackCall(attackThunk.data(),attackThunk.size())<0, "changed FName pointer register accepted");
    attackThunk[524]^=1;
    memcpy(attackThunk.data()+1,pbz::kAttackThunkTail,sizeof(pbz::kAttackThunkTail));
    check(pbz::FindAttackCall(attackThunk.data(),attackThunk.size())<0, "ambiguous consumer calls accepted");
    check(pbz::FindAttackCall(nullptr,0)<0 && pbz::FindAttackCall(attackThunk.data(),4)<0, "short/null thunk accepted");
    std::array<uint8_t,sizeof(pbz::kAttackBody)> attackBody{};
    memcpy(attackBody.data(),pbz::kAttackBody,attackBody.size());
    check(pbz::AttackBodyMatches(attackBody.data(),attackBody.size()), "captured native continuation rejected");
    for (size_t byte=0;byte<attackBody.size();++byte) {
        attackBody[byte]^=1;
        check(pbz::AttackBodyMatches(attackBody.data(),attackBody.size())==(byte>=70 && byte<74), "native ABI/opcode change not rejected");
        attackBody[byte]^=1;
    }
    check(!pbz::AttackBodyMatches(attackBody.data(),attackBody.size()-1) && !pbz::AttackBodyMatches(nullptr,attackBody.size()), "truncated continuation accepted");
    check(pbz::ChooseNormalResponse(1,true,false,false,false,false,99,75)==1, "normal guard should lead with guard");
    check(pbz::ChooseNormalResponse(1,true,true,false,true,true,0,75)==2, "low shachi must override held guard");
    check(pbz::ChooseNormalResponse(1,false,false,true,true,true,0,75)==2, "unguardable normal must dodge");
    check(pbz::ChooseNormalResponse(2,true,false,false,false,false,0,75)==2, "normal dodge should lead with dodge");
    check(pbz::ChooseNormalResponse(2,true,false,true,false,false,0,75)==1, "rapid follow-up guard fallback");
    check(pbz::ChooseNormalResponse(2,true,true,true,true,false,0,75)==2, "guard fallback despite low shachi");
    int mixedGuard=0,mixedDodge=0;
    for (int roll=0;roll<100;++roll) {
        const int choice=pbz::ChooseNormalResponse(3,true,false,false,false,false,roll,85);
        mixedGuard+=choice==1; mixedDodge+=choice==2;
    }
    check(mixedGuard==85 && mixedDodge==15, "mixed baseline is not 85/15 (2026-11-06: guard-first)");
    check(pbz::ExecutionBase("GA_ContinuousExecutionSimple_Base_C") && pbz::ExecutionBase("GA_ASM_UpRunExecution_C") &&
          pbz::ExecutionBase("GA_BrutalAxe_Execution_C") && pbz::ExecutionBase("GA_GreatBladeMan_ExecuteSeq_C") && pbz::ExecutionBase("GA_ExecutionAbilityBase_C"),
          "execution gate misses continuous / run-up / weapon executions");
    check(!pbz::ExecutionBase("GA_ExecutedAbilityBase_C") && !pbz::ExecutionBase("GA_Accessory_Execution_C") &&
          !pbz::ExecutionBase("GA_HuaShan_Troop_ExecutionPassive_C") && !pbz::ExecutionBase("GA_Accessory_ExecutionSwitchCool_Lv1_C") && !pbz::ExecutionBase("BP_Execution_C"),
          "execution gate catches the executed side / passive accessories");
    check(pbz::ScarceShachi(false,15,100), "low shachi entry threshold");
    check(pbz::ScarceShachi(true,29,100), "low shachi hysteresis lost");
    check(!pbz::ScarceShachi(true,31,100), "shachi recovery not restored");
    check(!pbz::ScarceShachi(false,-1,100) && !pbz::ScarceShachi(false,10,0), "missing shachi data treated as empty");
    double transform[12]={0,0,0,1,-1000,-2000,-300,0,1,1,1,0};
    const double areaInside[3]={-1000,-2000,-200}, areaOutside[3]={-500,-2000,-200}, extent[3]={200,50,50};
    check(pbz::AreaContains(areaInside,transform,0,250,0,extent), "negative-coordinate sphere origin");
    check(!pbz::AreaContains(areaOutside,transform,0,250,0,extent), "distant ground explosion triggers dodge");
    transform[8]=transform[9]=transform[10]=2;
    check(pbz::AreaContains(areaOutside,transform,0,250,0,extent), "scaled explosion radius ignored");
    transform[8]=transform[9]=transform[10]=1;
    check(pbz::AreaContains(areaInside,transform,2,0,0,extent), "box ground attack rejected");
    check(!pbz::AreaContains(areaOutside,transform,2,0,0,extent), "box bounds ignored");
    transform[2]=std::sqrt(.5); transform[3]=std::sqrt(.5);
    const double rotatedInside[3]={-1000,-1750,-300};
    check(pbz::AreaContains(rotatedInside,transform,2,0,0,extent), "rotated box origin");
    transform[2]=0; transform[3]=1; transform[8]=0;
    check(!pbz::AreaContains(areaInside,transform,0,250,0,extent), "invalid transform accepted");
    transform[8]=1;
    const double capsuleInside[3]={-1000,-2000,50};
    check(pbz::AreaContains(capsuleInside,transform,1,50,400,extent), "vertical capsule impact rejected");
    std::cout << "AUTO_POLICY_TEST_OK " << checks << " checks (isolated)\n";
    return 0;
} catch (const std::exception& error) {
    std::cerr << "AUTO_POLICY_TEST_FAILED " << error.what() << '\n'; return 1;
}
#endif
