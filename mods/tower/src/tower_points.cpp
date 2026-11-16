// 塔内原生升级面板改用影塔点数结算（只在独立塔档里、本局走原生成长时启用；出塔 / 停用即全部卸下，原游戏交易照旧）。
// 依据：Codex 2026-10-31 实机硬件断点——原生长按升级时 UpgradeWeapon / HasEnoughItems / RemovePropsFromBackPack
// 经过 UFunction 原生执行入口（+0x128）；扣尘 ChangeDustNum 是蓝图函数，走 ProcessEvent。
//   - UpgradeWeapon(ID, 普通, N)：塔档里有这把、淬炼点够 N、不超塔内上限 → 先记账扣点再放行（原函数本身只加级、不扣材料）；否则拦下；
//   - ActivateNode(ID, ASC, 初始化?, 满级?)：载入时的初始化放行；玩家操作按塔规则（前置 / 上限 / 修为点）记账扣点后放行，否则拦下；
//   - UpgradeAccessory / UpgradeMedicine：塔内药瓶等级、饰品精炼由层奖励决定，原生升级一律拦下；
//   - RemovePropsFromBackPack(升级材料) / ChangeDustNum(扣)：塔内不用原生材料与尘，拦下（塔内存档里本来就是 0）；
//   - HasEnoughItems(全是升级材料)：返回"够"（塔内材料不是限制条件，真正的限制是上面的点数检查）；
//   - GetCurrentDust（沿用 dust_gate 参数）：仅原生塔内成长面板中返回对应点数余额；
//     GetItemMaterialsByID 回答 1 点与空材料表，原生图形费用行显示淬炼点 / 修为点 / 铸魂令。
//     预算从不写入真实尘或存档；信息页、塔外及恢复核验照原生返回。出塔先卸钩再放回基准。
//   - 原生回退 / 洗点同样返还塔内点数，内部奖励和对账调用绕过界面交易，避免重复扣点。
// 记账之后会在下一帧读回原生等级核对，不一致就退回点数（避免"扣了点但原函数没生效"）。
// 证据等级：静态 + 离线；实机未验。
#include <windows.h>

#include <algorithm>
#include <cstring>
#include <cmath>
#include <format>
#include <string>
#include <unordered_set>
#include <vector>

#include "../../../src/core/host/log.h"
#include "../../../src/core/sdk/pbz_module.h"
#include "../../../src/core/services/gamethread.h"
#include "growth_data.generated.h"
#include "tower_save.h"
#include "tower_points.h"
#include "tower_points_logic.h"
#include "tower_perf.h"
#include "tower_weapon_cap.h"

namespace towerapi {
// tower.cpp：按塔规则对一次原生升级 / 激活记账（返回 false = 不允许）；refund 撤销上一次记账
bool ChargeWeapon(const std::string& id, int kind, int levels, std::string* why);
bool ChargeNode(const std::string& id, std::string* why);
bool ChargeSoul(const std::string& id, int kind, std::string* why);
bool ChargeAccessory(const std::string& id, int levels, std::string* why);
void RefundAccessory(const std::string& id, int levels);
void RefundSoul(const std::string& id, int kind);
bool NativeDowngrade(const std::string& id, int kind, int target, std::string* why);
bool NativeResetNodes(const std::string& id, bool all, std::string* why);
void Refund(const std::string& id, int kind, int levels);
void Notify(const std::string& code, const std::string& msg);
bool InsideTower();  // 独立塔档会话"在塔里"（进塔中 / 出塔中 / 塔外都是 false）
bool TemperMasterHeld();  // 3.1.0 淬炼大师
int TemperLevels(const std::string& id, int kind, int levels);  // N9h 淬炼大师：原生请求升 n 级 → 实际升几级（2n，不超上限）
float MeltMulOf(const std::string& id, int kind);  // N9k 熔铸：这把兵器的倍率（没熔铸 = 1）
float MeltPowerMul(void* weaponConfig);  // N9m：按武器配置查熔铸倍率
}  // namespace towerapi
namespace towernative { int Level(std::string_view weaponId, int kind); int NodeLevel(std::string_view nodeId); int Snapped(std::string_view id, int kind); int AccessoryLevel(std::string_view id); }

namespace towerpoints {
namespace {

using pbz::ue::UObject;
namespace ue = pbz::ue;
namespace pe = pbz::pe;
namespace nx = pbz::nx;

std::string NameOf(const uint8_t* p) {
    uint64_t n = 0;
    std::memcpy(&n, p, 8);
    if (!(n & 0xFFFFFFFF)) return {};
    auto s = ue::Name(static_cast<uint32_t>(n));
    const auto num = static_cast<uint32_t>(n >> 32);
    return num ? s + "_" + std::to_string(num - 1) : s;
}

std::vector<UObject*> g_native;  // 已挂的原生执行入口
std::vector<UObject*> g_dustFns;  // 尘增减（蓝图函数，ProcessEvent 拦截）
bool g_on = false, g_dustGate = false;
bool g_ready = false;
int g_blocked = 0, g_charged = 0, g_passed = 0, g_budgetReads = 0;
thread_local int g_internal = 0;
tower::points::Flight g_flight;

// 参数在缓冲里的偏移：装的时候一次取好，缺任何一个就不接管这一项（不拿 -1 去读写）
struct Params {
    UObject* fn = nullptr;
    int a = -1, b = -1, c = -1, ret = -1;
    bool ok() const { return fn && a >= 0; }
};
Params Get(UObject* cls, const char* name, const char* a, const char* b = nullptr, const char* c = nullptr) {
    Params p;
    p.fn = cls ? pe::FindFunction(cls, name) : nullptr;
    if (!p.fn) return p;
    p.a = a ? ue::PropOffset(p.fn, a) : -1;
    p.b = b ? ue::PropOffset(p.fn, b) : -1;
    p.c = c ? ue::PropOffset(p.fn, c) : -1;
    p.ret = ue::PropOffset(p.fn, "ReturnValue");
    if ((b && p.b < 0) || (c && p.c < 0)) p.a = -1;
    return p;
}
bool HookOne(const Params& p, const char* name, nx::Handler h) {
    if (!p.ok()) { g_ready = false; pbz::log::Info("塔内点数：{} 的函数或参数缺失，关闭本批点数交易", name); return false; }
    std::string why;
    if (!nx::Hook(p.fn, std::move(h), &why)) { g_ready = false; pbz::log::Info("塔内点数：挂 {} 失败：{}", name, why); return false; }
    g_native.push_back(p.fn);
    return true;
}

// ---- 1.0.1 武器回退 / 最大升级（用户：10 / 20 级回退要兵符退不了；缺"一键升到最高级"）----
// 最大升级：原生 UpgradeMaxPlan 逐级累加数据表里的 ConsumeDust 去比"当前尘"（塔内 = 淬炼点数），几百上千的原价比十几个点
// 连一级都算不出来，按钮不出现。塔内把两张武器表每级 ConsumeDust 改成 1：最大升级 = 当前等级 + 淬炼点数（不超过武器上限），
// 一次升完（扣点照旧走 UpgradeWeapon 记账）。出塔 / 卸下写回原值。改的是内存里已加载的表，存档不受影响
struct DustPatch { int32_t* at; int32_t value; };
std::vector<DustPatch> g_dustPatch;
// 1.1.1（「保存并退出」偶发闪退，4 次转储都在游戏工作线程的内存分配器里）：旧做法只记裸地址，出塔时"当前值是 1 就写回原值"。
// 表在中途被重载 / 搬家后旧地址早已是别的数据（进塔改 3490 处，出塔只"还原"到 91 / 136 / 38 处恰好等于 1 的地方）——
// 往别人的内存写数破坏了堆，稍后在分配器里崩。现在打补丁和还原都按"当前已加载的两张表里实际的尘价格子"走：
// 不在当前表里的地址一律不写；表已重载（新表本来就是原值）就不用还原
std::vector<int32_t*> DustCells() {
    std::vector<int32_t*> out;
    auto* dt = towerperf::ClassNamed("DataTable");
    if (!dt) return out;
    struct Spot { const char *table, *field, *elem; };
    static const Spot spots[] = {{"DT_WeaponData", "WeaponUpLevelDatas", "WeaponUpLevelData"},
                                 {"DT_ShaqiWeaponData", "ShaChiWeaponUpLevelData", "ShaChiWeaponUpLevelData"}};
    UObject* tables[2] = {};
    ue::ForEach([&](int, UObject* o) {
        if (!ue::IsA(o, dt) || ue::IsDefault(o)) return true;
        const auto n = ue::ObjName(o);
        for (int i = 0; i < 2; ++i) if (!tables[i] && n == spots[i].table) tables[i] = o;
        return !(tables[0] && tables[1]);
    });
    for (int i = 0; i < 2; ++i) {
        UObject* table = tables[i];
        auto* elem = towerperf::ClassNamed(spots[i].elem);
        if (!table || !elem) { pbz::log::Info("塔内点数：{} 未加载，最大升级按原生尘价", spots[i].table); continue; }
        auto* rowStruct = ue::Get<UObject*>(table + 0x28);  // UDataTable::RowStruct
        const int field = rowStruct ? ue::PropOffset(rowStruct, spots[i].field) : -1;
        const int dust = ue::PropOffset(elem, "ConsumeDust");
        const int stride = ue::Get<int32_t>(elem + ue::Layout::StructSize);
        if (field < 0 || dust < 0 || stride <= 0 || dust + 4 > stride) continue;
        // RowMap：TMap<FName, uint8*>（UDataTable+0x30），元素 = 键 8 + 行指针 8 + 哈希链 8；按稀疏数组分配位只取有效元素
        auto* map = table + 0x30;
        auto* rows = ue::Get<uint8_t*>(map);
        const int num = ue::Get<int32_t>(map + 8, -1), bits = ue::Get<int32_t>(map + 0x28, -1);
        auto* heap = ue::Get<uint8_t*>(map + 0x20);
        if (!rows || num <= 0 || num > 4096 || bits != num) continue;
        std::vector<uint32_t> flags((num + 31) / 32);
        if (!ue::Read(heap ? heap : map + 0x10, flags.data(), flags.size() * 4)) continue;
        for (int r = 0; r < num; ++r) {
            if (!((flags[r / 32] >> (r % 32)) & 1)) continue;
            auto* row = ue::Get<uint8_t*>(rows + 0x18 * r + 8);
            if (!row) continue;
            auto* data = ue::Get<uint8_t*>(row + field);
            const int n = ue::Get<int32_t>(row + field + 8, -1);
            if (!data || n <= 0 || n > 512) continue;
            for (int k = 0; k < n; ++k) out.push_back(reinterpret_cast<int32_t*>(data + stride * k + dust));
        }
    }
    return out;
}
void PatchWeaponDust(bool on) {
    const auto cells = DustCells();
    const std::unordered_set<int32_t*> live(cells.begin(), cells.end());
    if (!on || !g_dustPatch.empty()) {
        // 还原（或进塔时发现上一轮的记录）：只写回仍在当前表里、仍是 1 的格子
        int restored = 0, stale = 0;
        for (const auto& d : g_dustPatch) {
            if (!live.count(d.at)) { ++stale; continue; }
            int32_t cur = 0;
            if (ue::Read(d.at, &cur, 4) && cur == 1) { ue::Write(d.at, &d.value, 4); ++restored; }
        }
        if (!g_dustPatch.empty()) pbz::log::Info("塔内点数：武器表每级尘价已还原（{} 处；{} 处已不在当前表里，未写）", restored, stale);
        g_dustPatch.clear();
        if (!on) return;
    }
    for (auto* at : cells) {
        int32_t v = 0;
        if (!ue::Read(at, &v, 4) || v <= 1) continue;
        const int32_t one = 1;
        if (ue::Write(at, &one, 4)) g_dustPatch.push_back({at, v});
    }
    pbz::log::Info("塔内点数：武器表每级尘价改为 1（{} 处），最大升级按淬炼点数计算", g_dustPatch.size());
}

// ---- N9k（用户实机：熔铸后面板伤害 / 破防能力不变；实战已经 ×倍率）：武器面板读数来自背包的 Map<GameplayTag,float>，
// 原函数跑完后把被熔铸兵器里攻击 / 杀气（破防）类条目乘上倍率。只改界面读数，不碰实战（实战在武器强度接口）。
// 做法：拦下这次调用 → 用同一份参数自己再调一次（重入放行原函数）→ 改返回的表 → 把表交回调用方
thread_local int g_meltReent = 0;
int g_meltLogs = 0;
// Map<FGameplayTag,float>：元素 = {Tag(FName 8), float 4, 哈希链 4+4} = 20 字节
void ScaleTagMap(uint8_t* map, float mul, const std::string& id) {
    struct FArr { uint8_t* data; int32_t num, max; };
    const auto a = ue::Get<FArr>(map);
    if (a.num <= 0 || a.num > 256 || !a.data) return;
    const int bits = ue::Get<int32_t>(map + 0x28, -1);
    auto* heap = ue::Get<uint8_t*>(map + 0x20);
    if (bits != a.num) return;
    std::vector<uint32_t> flags((a.num + 31) / 32);
    if (!ue::Read(heap ? heap : map + 0x10, flags.data(), flags.size() * 4)) return;
    std::string seen;
    for (int i = 0; i < a.num; ++i) {
        if (!((flags[i / 32] >> (i % 32)) & 1)) continue;
        uint8_t* e = a.data + i * 20;
        const auto tag = ue::Name(static_cast<uint32_t>(ue::Get<uint64_t>(e)));
        float v = ue::Get<float>(e + 8);
        const bool hit = tag.find("Attack") != std::string::npos || tag.find("Shachi") != std::string::npos || tag.find("ShaChi") != std::string::npos ||
                         tag.find("Break") != std::string::npos || tag.find("Power") != std::string::npos || tag.find("Damage") != std::string::npos;
        if (g_meltLogs < 40) seen += std::format(" {}={:.1f}{}", tag, v, hit ? "*" : "");
        if (hit && std::isfinite(v)) { v *= mul; ue::Write(e + 8, &v, 4); }
    }
    if (g_meltLogs < 40) { ++g_meltLogs; pbz::log::Info("影塔熔铸面板：{} ×{:.2f}：{}", id, mul, seen); }
}
int g_meltCalls[4] = {};  // N9m 诊断：各面板入口被调用次数（前 15 次写日志，看面板实际走哪条）
void HookMeltUi(UObject* bag) {
    static const char* kNames[] = {"GetWeaponCombatAttribute", "GetWeaponCurrentGEUIData", "GetWeaponGEUIDataByConfig"};
    for (int fi = 0; fi < 3; ++fi) {
        const char* name = kNames[fi];
        const bool byConfig = fi == 2;
        const auto q = byConfig ? Get(bag, name, "WeaponConfig", "bIsNormalWeapon") : Get(bag, name, "WeaponID", "bIsNormalWeapon");
        if (!q.ok() || q.ret < 0) { pbz::log::Info("影塔熔铸面板：找不到 {}，面板读数不含熔铸", name); continue; }
        std::string why;
        const bool ok = nx::Hook(q.fn, [q, fi, byConfig, name](UObject* self, uint8_t* p, void* result) {
            if (g_meltReent || !towerapi::InsideTower() || !self) return false;
            std::string id; float mul = 1.0f;
            if (byConfig) { auto* cfg = ue::Get<UObject*>(p + q.a); id = cfg && ue::Live(cfg) ? ue::ObjName(cfg) : std::string("?"); mul = cfg ? towerapi::MeltPowerMul(cfg) : 1.0f; }
            else { id = NameOf(p + q.a); mul = towerapi::MeltMulOf(id, p[q.b] ? 0 : 1); }
            if (++g_meltCalls[fi] <= 15) pbz::log::Info("影塔熔铸面板：调用 {} 第 {} 次 {} ×{:.2f}", name, g_meltCalls[fi], id, mul);
            if (mul == 1.0f) return false;  // 没熔铸：原样放行
            std::vector<uint8_t> tmp(static_cast<size_t>(q.ret) + 0x80, 0);
            std::memcpy(tmp.data(), p, static_cast<size_t>(q.ret));
            ++g_meltReent; pe::Call(self, q.fn, tmp.data()); --g_meltReent;
            ScaleTagMap(tmp.data() + q.ret, mul, id);
            std::memcpy(result ? static_cast<uint8_t*>(result) : p + q.ret, tmp.data() + q.ret, 0x50);  // 表的所有权交给调用方（tmp 不析构）
            return true;
        }, &why);
        if (ok) g_native.push_back(q.fn); else pbz::log::Info("影塔熔铸面板：挂 {} 失败：{}", name, why);
    }
}

void Install() {
    g_ready = true;
    auto* bag = towerperf::ClassNamed("PBZBackPackSubsystem");
    auto* nodes = towerperf::ClassNamed("PBZAbilityNodesComponent");
    auto* player = towerperf::ClassNamed("PBZCharacter_Player");
    // 兵器升级：记账后放行（原函数免费加级）；读回不一致退点
    HookMeltUi(bag);
    const auto up = Get(bag, "UpgradeWeapon", "WeaponID", "IsNormalWeapon", "UpgradeLevel");
    HookOne(up, "UpgradeWeapon", [up](UObject*, uint8_t* p, void*) {
        if (g_internal || !towerapi::InsideTower()) return false;
        const std::string id = NameOf(p + up.a);
        const int kind = p[up.b] ? 0 : 1;
        int32_t n = 1;
        std::memcpy(&n, p + up.c, 4);
        std::string why;
        const int before = towernative::Level(id, kind);
        const auto token = g_flight.Begin();
        if (!token) return true; // A previous hold event is waiting for readback.
        // N9h（用户实机：有淬炼大师升级还是 +1）：原生按钮每次请求 +n 级，淬炼大师改成 +2n 级（到上限为止），扣点仍是 n（一半向上取整）
        if (n > 0) if (const int m = towerapi::TemperLevels(id, kind, n); m != n && m > 0) { n = m; std::memcpy(p + up.c, &n, 4); }
        if (n <= 0 || !towerapi::ChargeWeapon(id, kind, n, &why)) {
            g_flight.Finish(token);
            ++g_blocked;
            pbz::log::Info("塔内点数：拦下兵器升级 {} +{}（{}）", id, n, why);
            towerapi::Notify("err.grow", why);
            return true;  // 不调原函数
        }
        ++g_charged;
        pbz::gamethread::Post([id, kind, n, before, token] {  // 原函数执行完后核对
            if (!g_on || !g_flight.Finish(token)) return;
            if (towernative::Level(id, kind) != before + n) { towerapi::Refund(id, kind, n); pbz::log::Info("塔内点数：{} 升级未生效，已退回 {} 点", id, n); }
        });
        return false;
    });
    // 能力激活：初始化放行；玩家操作按塔规则记账
    const auto act = Get(nodes, "ActivateNode", "NodeID", "bActivateByInit");
    HookOne(act, "ActivateNode", [act](UObject*, uint8_t* p, void*) {
        if (g_internal || !towerapi::InsideTower()) return false;
        if (p[act.b]) { ++g_passed; return false; }
        const std::string id = NameOf(p + act.a);
        std::string why;
        const int before = towernative::NodeLevel(id);
        const auto token = g_flight.Begin(); if (!token) return true;
        if (!towerapi::ChargeNode(id, &why)) {
            g_flight.Finish(token);
            ++g_blocked;
            pbz::log::Info("塔内点数：拦下能力 {}（{}）", id, why);
            towerapi::Notify("err.grow", why);
            return true;
        }
        ++g_charged;
        pbz::gamethread::Post([id, before, token] {
            if (!g_on || !g_flight.Finish(token)) return;
            if (towernative::NodeLevel(id) <= before) { towerapi::Refund(id, 2, 1); pbz::log::Info("塔内点数：能力 {} 未生效，已退回", id); }
        });
        return false;
    });
    // The graphical soul panel must consume a tower token, with the same readback contract.
    const auto soul = Get(bag, "SnapSoulWeapon", "WeaponID", "IsNormalWeapon");
    HookOne(soul, "SnapSoulWeapon", [soul](UObject*, uint8_t* p, void*) {
        if (g_internal || !towerapi::InsideTower()) return false;
        const std::string id = NameOf(p + soul.a); const int kind = p[soul.b] ? 0 : 1;
        const auto token = g_flight.Begin(); if (!token) return true;
        std::string why;
        if (towernative::Snapped(id, kind) != 0 || !towerapi::ChargeSoul(id, kind, &why)) {
            g_flight.Finish(token); ++g_blocked;
            towerapi::Notify("err.grow", why.empty() ? "铸魂状态不可用或已经铸魂" : why); return true;
        }
        ++g_charged;
        pbz::gamethread::Post([id, kind, token] {
            if (!g_on || !g_flight.Finish(token)) return;
            if (towernative::Snapped(id, kind) != 1) { towerapi::RefundSoul(id, kind); pbz::log::Info("影塔原生铸魂未生效：{}，铸魂令已返还", id); }
        });
        return false;
    });
    // Graphical native refunds return points, never real dust/materials to a tower save.
    const auto down = Get(bag, "ResetWeaponToLevel", "WeaponID", "bIsNormalWeapon", "TargetLevel");
    const int refundDust = down.fn ? ue::PropOffset(down.fn, "OutRefundedDust") : -1;
    HookOne(down, "ResetWeaponToLevel", [down, refundDust](UObject*, uint8_t* p, void*) {
        if (g_internal || !towerapi::InsideTower()) return false;
        if (refundDust >= 0) { const int zero = 0; std::memcpy(p + refundDust, &zero, 4); }
        std::string why;
        if (g_flight.pending || !towerapi::NativeDowngrade(NameOf(p + down.a), p[down.b] ? 0 : 1, ue::Get<int32_t>(p + down.c), &why)) towerapi::Notify("err.grow", why.empty() ? "上一笔升级仍在核对" : why);
        return true; // OutRefundedMaterials remains its engine-initialized empty map.
    });
    for (const char* name : {"DeactivateNode", "DeactivateAllNodes"}) {
        const bool all = std::strcmp(name, "DeactivateAllNodes") == 0;
        const auto deact = Get(nodes, name, all ? "ASC" : "NodeID");
        HookOne(deact, name, [deact, all](UObject*, uint8_t* p, void*) {
            if (g_internal || !towerapi::InsideTower()) return false;
            std::string why;
            if (g_flight.pending || !towerapi::NativeResetNodes(all ? "" : NameOf(p + deact.a), all, &why)) towerapi::Notify("err.grow", why.empty() ? "上一笔升级仍在核对" : why);
            return true;
        });
    }
    // 饰品打磨（用户 2026-11-11）：花淬炼点，记账后放行原函数（原生会重挂已装备饰品的 GE），读回不符退点
    {
        const auto acc = Get(bag, "UpgradeAccessory", "AccessoryID", "UpgradeLevel");
        HookOne(acc, "UpgradeAccessory", [acc](UObject*, uint8_t* p, void*) {
            if (g_internal || !towerapi::InsideTower()) return false;
            const std::string id = NameOf(p + acc.a);
            int32_t n = 1; std::memcpy(&n, p + acc.b, 4);
            std::string why;
            const int before = towernative::AccessoryLevel(id);
            const auto token = g_flight.Begin();
            if (!token) return true;
            if (n <= 0 || before < 0 || !towerapi::ChargeAccessory(id, n, &why)) {
                g_flight.Finish(token); ++g_blocked;
                pbz::log::Info("塔内点数：拦下饰品打磨 {} +{}（{}）", id, n, why.empty() ? "原生饰品不存在" : why);
                towerapi::Notify("err.grow", why.empty() ? "这件饰品不能打磨" : why);
                return true;
            }
            ++g_charged;
            pbz::gamethread::Post([id, n, before, token] {
                if (!g_on || !g_flight.Finish(token)) return;
                if (towernative::AccessoryLevel(id) != before + n) { towerapi::RefundAccessory(id, n); pbz::log::Info("塔内点数：饰品 {} 打磨未生效，已退回淬炼点", id); }
            });
            return false;
        });
    }
    // 药瓶：塔内等级由层数决定（第 10/20/25/30/35 层自动升级），原生升级拦下
    for (const char* name : {"UpgradeMedicine"}) {
        const auto pr = Get(bag, name, "UpgradeLevel");
        HookOne(pr, name, [name](UObject*, uint8_t*, void*) {
            if (g_internal || !towerapi::InsideTower()) return false;
            ++g_blocked;
            pbz::log::Info("塔内点数：塔内不用原生 {}", name);
            towerapi::Notify("err.grow", "塔内药瓶等级随层数自动提升");
            return true;
        });
    }
    // 升级材料扣除：塔内不用原生材料
    const auto rm = Get(bag, "RemovePropsFromBackPack", "ItemId");
    HookOne(rm, "RemovePropsFromBackPack", [rm](UObject*, uint8_t* p, void*) {
        if (!towerapi::InsideTower()) return false;
        if (UiBalance() >= 0) { ++g_blocked; return true; } // Native ability UI also consumes special progression materials before ActivateNode.
        if (!tower::save::IsUpgradeMaterial(NameOf(p + rm.a))) { ++g_passed; return false; }
        ++g_blocked;
        return true;
    });
    // 材料够不够：全是升级材料时答"够"（Map<FName,int32> 元素 = FName 8 + int32 4 + 哈希 8 = 20 字节）
    const auto he = Get(bag, "HasEnoughItems", "RequiredItems");
    HookOne(he, "HasEnoughItems", [he](UObject*, uint8_t* p, void* result) {
        if (!towerapi::InsideTower()) return false;
        struct Arr { uint8_t* data; int32_t num, max; } a{};
        std::memcpy(&a, p + he.a, sizeof a);
        if (a.num == 0 && UiBalance() >= 0) { if (he.ret >= 0) p[he.ret] = 1; if (result) *static_cast<uint8_t*>(result) = 1; return true; }
        // 原生能力修行（用户 2026-11-06）：节点要的黑石 / 杀气琥珀 / 记忆之花 / 榕树之心塔里都没有，原先照原生答"不够"→ 长按升不了。
        // 能力面板里只看修为点：≥1 就答"够"；真正扣点与前置 / 上限检查在 ActivateNode（ChargeNode），原生材料扣除照旧拦下
        if (UiMode() == 2 && UiBalance() >= 0) {
            const uint8_t enough = UiBalance() >= 1;
            ++g_passed;
            if (he.ret >= 0) p[he.ret] = enough;
            if (result) *static_cast<uint8_t*>(result) = enough;
            return true;
        }
        if (!a.data || a.num <= 0 || a.num > 64) return false;
        // Respect TSparseArray allocation flags; freed map slots retain stale FNames.
        auto* map = p + he.a;
        const int bits = ue::Get<int32_t>(map + 0x28, -1), capacity = ue::Get<int32_t>(map + 0x2c, -1), free = ue::Get<int32_t>(map + 0x34, -1);
        auto* heap = ue::Get<uint8_t*>(map + 0x20);
        if (bits != a.num || capacity < bits || capacity > 1048576 || free < 0 || free > a.num || (!heap && capacity > 128)) return false;
        uint32_t flags[2]{};
        if (!ue::Read(heap ? heap : map + 0x10, flags, ((a.num + 31) / 32) * 4)) return false;
        int used = 0;
        bool soul = false;
        const bool soulPanel = UiMode() == 3;
        for (int i = 0; i < a.num; ++i) {
            if (!((flags[i / 32] >> (i % 32)) & 1)) continue;
            ++used;
            uint8_t name[8];
            if (!ue::Read(a.data + i * 20, name, 8)) return false;
            const auto id = NameOf(name);
            if (soulPanel && tower::save::IsSoulMaterial(id)) { soul = true; continue; }
            if (!id.empty() && !tower::save::IsUpgradeMaterial(id)) return false;  // 有非升级材料：照原生判断
        }
        if (used != a.num - free) return false;
        // 6ede7eb 实测：铸魂视图的长按门是 HasEnoughItems(该兵器的 TU_ 专属材料)，原先照原生答"不够"→ 有令仍"材料不足"。
        // 塔内铸魂只看通用令：有 ≥1 枚就答"够"；真正扣令在 SnapSoulWeapon（每把一次、读回不符退令），原生材料与尘照旧拦下不扣
        const uint8_t enough = soul ? UiBalance() >= 1 : 1;
        if (soul) ++g_passed;
        if (he.ret >= 0) p[he.ret] = enough;
        if (result) *static_cast<uint8_t*>(result) = enough;
        return true;
    });
    // 回退符数量（1.0.1 免费回退）：原生 10 级以上回退要"兵刃逆炼符 / 影之消退符"，面板用 GetItemNumInfo 查持有数。
    // 塔内报成足够（扣除本来就被 RemovePropsFromBackPack 拦下）；这个入口缺失只记日志，不关掉整批点数交易
    if (const auto gi = Get(bag, "GetItemNumInfo", "PropID", "CurrentNum", "MaxNum"); gi.ok()) {
        std::string why;
        if (nx::Hook(gi.fn, [gi](UObject*, uint8_t* p, void*) {
                if (!towerapi::InsideTower()) return false;
                const auto id = NameOf(p + gi.a);
                if (id != "Return_Weapon_1" && id != "Return_ShaqiWeapon_1") return false;
                const int32_t n = 99;
                std::memcpy(p + gi.b, &n, 4); std::memcpy(p + gi.c, &n, 4);
                return true;
            }, &why)) g_native.push_back(gi.fn);
        else pbz::log::Info("塔内点数：回退符数量入口挂接失败（{}），10 级以上回退仍按原生", why);
    } else pbz::log::Info("塔内点数：找不到 GetItemNumInfo，10 级以上回退仍按原生");
    PatchWeaponDust(true);
    // 当前尘：只读预算（见文件头 1–5），让面板按钮按点数而不是尘判断
    auto* renewalPlayer = towerperf::ClassNamed("BP_RenewalPlayer_C");
    if (g_dustGate) for (auto* owner : {player, renewalPlayer}) {
        auto gd = Get(owner, "GetCurrentDust", "ReturnValue");
        if (!gd.ok()) { g_ready = false; continue; }
        if (std::find(g_native.begin(), g_native.end(), gd.fn) != g_native.end()) continue;
        auto handler = [gd](UObject*, uint8_t* p, void* result) {
            if (!towerapi::InsideTower()) return false;  // 不在塔里：原生返回
            int32_t budget = UiBalance();
            if (budget < 0) return false;
            // 能力面板（用户 2026-11-05）：原生 JS 拿节点自己的尘价（GetConsumeDust，各节点类各有一份，拦截槽位不够全接管）
            // 和这里比；有修为点就答一个足够大的数，让原生判定只剩前置 / 上限。真正的扣点与前置 / 上限检查在 ActivateNode（ChargeNode）
            if (UiMode() == 2 && budget > 0) budget = 1 << 28;
            // 3.1.0 淬炼大师：兵器面板按「每级 1 尘」算最大升级，预算报 淬炼点 × 2（实际扣点在 UpgradeWeapon → ChargeWeapon 按一半向上取整）
            // N9h：不再翻倍预算（面板按「1 点 = 1 次」选，实际每次升 2 级在 UpgradeWeapon 里改）
            std::memcpy(p + gd.a, &budget, 4);
            if (result) std::memcpy(result, &budget, 4);
            ++g_budgetReads;
            return true;
        };
        HookOne(gd, "GetCurrentDust", handler);
        pe::Intercept(gd.fn, [handler](UObject* self, void* p) { return handler(self, static_cast<uint8_t*>(p), nullptr); });
        g_dustFns.push_back(gd.fn);
    }
    // 尘的增减（蓝图函数，ProcessEvent）：塔内一律不改，角色真实尘字段保持塔档的 0（预算永远不会被写进去）
    for (const char* name : {"ChangeDustNum", "ChangeDustNumInternal"}) {
        UObject* fn = renewalPlayer ? pe::FindFunction(renewalPlayer, name) : nullptr;
        if (!fn && player) fn = pe::FindFunction(player, name);
        if (!fn) { g_ready = false; pbz::log::Info("塔内点数：找不到 {}", name); continue; }
        pe::Intercept(fn, [](UObject*, void*) {
            if (!towerapi::InsideTower()) return false;
            ++g_blocked;
            return true;
        });
        g_dustFns.push_back(fn);
    }
    pbz::log::Info("塔内点数：原生升级 / 能力面板改用影塔点数（接管 {} 个原生入口，尘增减拦截 {} 个，只读尘预算 {}）", g_native.size(), g_dustFns.size(), g_dustGate ? "开" : "关");
}

void Uninstall() {
    towerweaponcap::Disable();
    PatchWeaponDust(false);
    ClearUi();
    g_flight.Reset();
    for (auto* fn : g_native) nx::Unhook(fn);
    g_native.clear();
    for (auto* fn : g_dustFns) pe::RemoveIntercept(fn);
    g_dustFns.clear();
    pbz::log::Info("塔内点数：已卸下，原游戏升级交易恢复（拦 {} 次，记账 {} 次）", g_blocked, g_charged);
}

}  // namespace

void EnterInternal() { ++g_internal; }
void LeaveInternal() { --g_internal; }

// 1.3.3（玩家："没有一键升到最高级"）：塔内重开 / 继续会回标题再载入，武器表随之重新载入成原价；
// Enable 见"已经开着"直接返回，补丁就没了，整局"最大升级"按原价算、按钮不出现（core.log 事务 2 实证）。
// 现在塔档每次载入、每次打开兵器升级都核对一遍：表里还有大于 1 的尘价就重新改。
// 旧记录：地址已不在表里、或被重载写回了原价的作废（不还原，免得把旧值写进新表）；仍是 1 的保留，出塔照常还原
void RefreshWeaponDust(const char* why) {
    if (!g_on) return;
    const auto cells = DustCells();
    std::vector<int32_t*> todo;
    for (auto* at : cells) {
        int32_t v = 0;
        if (ue::Read(at, &v, 4) && v > 1) todo.push_back(at);
    }
    if (todo.empty()) return;
    const std::unordered_set<int32_t*> live(cells.begin(), cells.end()), again(todo.begin(), todo.end());
    const auto before = g_dustPatch.size();
    std::erase_if(g_dustPatch, [&](const DustPatch& d) { return !live.count(d.at) || again.count(d.at); });
    const auto dropped = before - g_dustPatch.size();
    int patched = 0;
    for (auto* at : todo) {
        int32_t v = 0;
        if (!ue::Read(at, &v, 4) || v <= 1) continue;
        const int32_t one = 1;
        if (ue::Write(at, &one, 4)) { g_dustPatch.push_back({at, v}); ++patched; }
    }
    pbz::log::Info("塔内点数：{}时武器表尘价已回到原价（{} 处，旧记录作废 {} 处），重新改为 1；最大升级按淬炼点数计算", why, patched, dropped);
}

void Enable(bool on, bool dustGate) {
    if (on == g_on && dustGate == g_dustGate) {
        if (on) { RefreshWeaponDust("塔档重新载入"); if (!towerweaponcap::Ready()) towerweaponcap::Enable(); }
        return;
    }
    if (g_on) Uninstall();
    g_on = on;
    g_dustGate = dustGate;
    if (on) {
        if (auto* free = pbz::FindFeature("res.upgrade_free"); free && free->enabled) { g_on = false; pbz::log::Info("影塔点数：升级无消耗正在接管原生费用，未安装竞争钩子"); return; }
        Install();
        if (!g_ready) { Uninstall(); g_on = false; pbz::log::Info("塔内点数：关键接口缺失，已撤回全部交易接管与虚拟读数"); }
        else towerweaponcap::Enable();
    }
}
bool Enabled() { return g_on; }
std::string Status() {
    return std::format(R"("native_points":{{"on":{},"dust_gate":{},"hooks":{},"blocked":{},"charged":{},"passed":{},"budget_reads":{}}})", g_on, g_dustGate, g_native.size(), g_blocked, g_charged, g_passed, g_budgetReads);
}

}  // namespace towerpoints
