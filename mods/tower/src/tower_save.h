// 独立影塔档（纯逻辑）：从"进塔基准"（游戏自己的存档 JSON）和塔档（growth::Profile）生成一份游戏格式的塔内存档。
// 进塔时把它放进游戏内存（ReplaceSaveString），玩家回标题 → 继续即载入；出塔放回基准，同样回标题 → 继续。
// 两个方向走的是 2026-10-31 实机验证过的同一条路（基准放回 → 回标题继续 → 逐项与基准一致）。
// 塔内存档里：
//   - 背包只有塔档里的兵器 / 药 / 饰品（原档其他物品不出现、不能装备），装备槽直接是塔档的 2 主 + 2 影之武装 + 2 药；
//   - 升级材料与意识尘埃为 0：塔内升级只用影塔点数，原生材料升级在塔内不可用；
//   - 能力节点按塔档；
//   - 其余（世界、剧情、钟、地图、位置……）保持进塔基准，不改世界状态。
// 字段格式按 2026-10-31 本机基准 JSON 核对：主兵器等级 WeaponLevel（1 级时省略）、影之武装 ShaChiWeaponLevel、
// 药 CurrentLevel 从 0 起（药葫芦 0 级 = 2 次）、饰品 CurrentLevel 从 0 起、饰品槽数组 6 格（空为 "None"）。
#pragma once
#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

#include "tower_growth.h"
#include "tower_json.h"
#include "tower_replay.h"
#include "replay_flow_nodes.generated.h"

namespace tower::save {

using json::Value;

// 升级用材料：塔内存档里清空
inline bool IsUpgradeMaterial(std::string_view id) {
    for (std::string_view p : {"Weapon_Material_", "Ac_Material_", "Medi_Material_", "Return_Weapon_", "Return_ShaqiWeapon_"})
        if (ids::Equal(id.substr(0, p.size()), p)) return true;
    return false;
}

// 永久加成道具（用户 2026-11-03：塔内生命 / 杀气等从原游戏初值开始爬）。DA_Inventorydata 的 AmountTierExtraGEMap /
// AlwaysPushTipsItemSet：Item_001 杀气核心（按数量叠永久 GE，抬杀气上限）、Item_002（按数量叠永久 GE）及两种碎片。
// R6 实机：原档进塔后生命 935 / 杀气 800，是原档攒的这些道具撑起来的
inline bool IsPermanentBoost(std::string_view id) {
    for (std::string_view p : {"Item_001", "Item_002", "Item_Piece_001", "Item_Piece_002"})
        if (ids::Equal(id, p)) return true;
    return false;
}

// 原生铸魂材料（DT_WeaponData / DT_ShaqiWeaponData 的 SnapSoulData.Materials 全是 TU_xxxxx，每把兵器各一种）。
// 塔内铸魂统一用 1 枚通用铸魂令（用户 2026-11-01），这些专属材料在塔里不是限制条件
inline bool IsSoulMaterial(std::string_view id) { return id.size() > 3 && ids::Equal(id.substr(0, 3), "TU_"); }

struct Equip {  // 塔内存档里的装备槽（按塔档顺序取前两件）
    std::vector<std::string> main, shaqi, meds, accessories;
};
inline Equip DefaultEquip(const growth::Profile& p) {
    Equip e;
    for (auto& w : p.weapons) (growth::kWeapons[w.index].kind == 0 ? e.main : e.shaqi).push_back(std::string(growth::kWeapons[w.index].id));
    if (e.main.size() > 2) e.main.resize(2);
    if (e.main.size() == 1) e.main.push_back("None");
    e.shaqi.resize(2, "None");  // 原生两个影之武装槽；不足两把时显式补 None
    for (int m : p.medicines) if (e.meds.size() < 2) e.meds.push_back(std::string(growth::kMedicines[m].id));
    return e;
}

// 继续上一局（用户 2026-11-05）：按保存时的实际装备还原；每一组只要有一件不在塔档里，那一组退回默认
inline Equip SavedEquip(const growth::Profile& p, std::string_view text) {
    Equip e = DefaultEquip(p);
    if (text.empty()) return e;
    auto split = [](std::string_view v, char c) { std::vector<std::string> o; size_t a = 0; while (a <= v.size()) { size_t b = v.find(c, a); if (b == std::string_view::npos) b = v.size(); o.emplace_back(v.substr(a, b - a)); a = b + 1; } return o; };
    const auto groups = split(text, '|');
    if (groups.size() != 4) return e;
    auto none = [](const std::string& s) { return s.empty() || s == "None"; };
    auto weapons = [&](const std::string& g, int kind, std::vector<std::string>* out) {
        auto v = split(g, ',');
        bool any = false;
        for (auto& id : v) {
            if (none(id)) { id = "None"; continue; }
            const int w = growth::WeaponIndex(id, kind);
            if (w < 0 || p.Find(w) < 0) return;
            any = true;
        }
        if (!any) return;
        v.resize(2, "None");
        *out = v;
    };
    weapons(groups[0], 0, &e.main);
    weapons(groups[1], 1, &e.shaqi);
    {
        auto v = split(groups[2], ',');
        bool ok = true;
        for (auto& id : v) {
            if (none(id)) { id = "None"; continue; }
            const int m = growth::MedicineIndex(id);
            ok = ok && m >= 0 && std::find(p.medicines.begin(), p.medicines.end(), m) != p.medicines.end();
        }
        if (ok && !v.empty()) { v.resize(2, "None"); e.meds = v; }
    }
    {
        std::vector<std::string> acc;
        bool ok = true;
        for (auto& id : split(groups[3], ',')) {
            if (none(id)) continue;
            const int a = growth::AccessoryIndex(id);
            ok = ok && a >= 0 && std::any_of(p.accessories.begin(), p.accessories.end(), [&](const growth::OwnedAccessory& x) { return x.index == a; });
            acc.push_back(id);
        }
        if (ok) e.accessories = acc;
    }
    return e;
}

struct Result { bool ok = false; std::string why; Value save; int resetTags = 0, resetNodes = 0; };

// 原生遭遇重放（tower_replay.h）：塔档里删掉这些遭遇的完成标记（EventPipeline 里各 Flow 物件的 EventTag），
// 打过原生这场的存档进塔后遭遇重新可开；真档不动（塔档是独立档）。返回删掉的条数
inline int ResetReplayTags(Value& save) {
    Value* ep = save.Find("EventPipeline");
    if (!ep) return 0;
    int removed = 0;
    auto doomed = [](const Value& v) {
        if (v.kind != Value::String) return false;
        for (auto& c : replay::kContracts)
            for (int i = 0; i < c.resetN; ++i) if (v.text == c.resetTags[i]) return true;
        return false;
    };
    auto walk = [&](auto& self, Value& v) -> void {
        if (v.kind == Value::Array) {
            const size_t before = v.items.size();
            std::erase_if(v.items, doomed);
            removed += static_cast<int>(before - v.items.size());
            for (auto& x : v.items) self(self, x);
        } else if (v.kind == Value::Object) {
            for (auto& [k, x] : v.fields) self(self, x);
        }
    };
    walk(walk, *ep);
    return removed;
}

// 原生 Flow 节点的"已执行"记录（EventPipeline.ExecuteNodeGuid，键 = 节点 GUID，或 "<地图>-<宿主 Actor>_<节点 GUID>"）。
// 只删标签不够：6d105f2 第 4 层追命，原档打败过追命，死亡 Flow 转阶段分支 SetExcuteType 2B132618… 记为已执行，
// 一阶段死后原生清组事件照常发出，但 Flow 不再进 Seq023。塔档里删掉每个重放契约出场 / 死亡 Flow 全部节点的记录。返回删掉的条数
inline int ResetReplayNodes(Value& save) {
    Value* ep = save.Find("EventPipeline");
    Value* done = ep ? ep->Find("ExecuteNodeGuid") : nullptr;
    if (!done || done->kind != Value::Object) return 0;
    auto doomed = [](const std::string& key) {
        if (key.size() < 32) return false;
        const std::string_view guid = std::string_view(key).substr(key.size() - 32);
        for (auto& n : replay::kFlowNodes) if (ids::Equal(n.guid, guid)) return true;
        return false;
    };
    const size_t before = done->fields.size();
    std::erase_if(done->fields, [&](const auto& f) { return doomed(f.first); });
    return static_cast<int>(before - done->fields.size());
}

// stamp：DirtyTime / DateTime 字段的时间文本（"2026.10.31-18.16.05"）
// entitlementSave：平台赠送（DLC / 预购）物品取自哪份存档。以世界模板为底时传玩家自己的原档——模板主人的赠品别人没有，
// 游戏载入时会删掉，核验就报"饰品 AC_Loot 缺少"（1.0.1 玩家反馈）。不传 = 取 baseline 自己的
inline Result Build(const Value& baseline, const growth::Profile& p, const Equip& eq, const std::string& stamp, const Value* entitlementSave = nullptr) {
    Result r;
    r.save = baseline;
    Value* inv = r.save.Find("Inventory");
    Value* player = r.save.Find("Player");
    Value* nodesSec = r.save.Find("AbilityNode_RenewalHun");
    Value* nodes = nodesSec ? nodesSec->Find("AbilityNodes") : nullptr;
    if (!inv || inv->kind != Value::Object || !player || !nodes || nodes->kind != Value::Object) { r.why = "基准存档缺少 Inventory / Player / AbilityNode_RenewalHun"; return r; }
    if (eq.main.empty()) { r.why = "塔档没有主兵器"; return r; }
    // 3.3.2（玩家：回收了正在装备的影之武装后保存重进，核验报「当前装备索引 CurrentShaqiWeaponIndex」进不去）：
    // 装备槽里空槽在前时，当前手持（固定写 0）指向空槽。有兵器的槽排到前面，空槽放后面
    Equip eqFixed = eq;
    for (auto* slots : {&eqFixed.main, &eqFixed.shaqi})
        std::stable_partition(slots->begin(), slots->end(), [](const std::string& s) { return !s.empty() && s != "None"; });
    const Equip& eqUse = eqFixed;
    if (eqUse.main.empty() || eqUse.main[0].empty() || eqUse.main[0] == "None") { r.why = "塔档没有主兵器"; return r; }

    // Reject ambiguous FName keys; preserving one arbitrarily would hide a bad baseline.
    for (const char* sec : {"MainWeaponInfos", "ShaChiWeaponInfo", "MedicineInfos", "AccessoryInfos"})
        if (const auto* map = inv->Find(sec)) for (const auto& [id, unused] : map->fields)
            if (ids::Find(map, id).ambiguous) { r.why = std::string("基准物品键冲突：") + sec + "/" + id; return r; }
    auto oldInfo = [&](const char* sec, const std::string& id) -> const Value* { return ids::Find(inv->Find(sec), id).value; };
    // 主兵器
    Value mains = Value::Obj(), shaqis = Value::Obj();
    for (auto& w : p.weapons) {
        const auto& d = growth::kWeapons[w.index];
        const std::string id(d.id);
        if (d.kind == 0) {
            Value e = oldInfo("MainWeaponInfos", id) ? *oldInfo("MainWeaponInfos", id) : Value::Obj();
            if (e.fields.empty()) { e.Set("DateTime", Value::Str(stamp)); e.Set("AcquiredCycle", Value::Num(0)); e.Set("FirstAcquiredCycle", Value::Num(0)); e.Set("HasUIRedPoint", Value::Boolean(false)); }
            if (w.level > 1) e.Set("WeaponLevel", Value::Num(w.level)); else e.Erase("WeaponLevel");
            e.Set("AlreadySnapSoul", Value::Num(w.snapped ? 1 : 0));
            mains.Set(id, std::move(e));
        } else {
            Value e = oldInfo("ShaChiWeaponInfo", id) ? *oldInfo("ShaChiWeaponInfo", id) : Value::Obj();
            if (e.fields.empty()) { e.Set("AcquiredCycle", Value::Num(0)); e.Set("FirstAcquiredCycle", Value::Num(0)); e.Set("DirtyTime", Value::Str(stamp)); e.Set("HasUIRedPoint", Value::Boolean(false)); }
            if (w.level > 1) e.Set("ShaChiWeaponLevel", Value::Num(w.level)); else e.Erase("ShaChiWeaponLevel");
            e.Set("AlreadySnapSoul", Value::Boolean(w.snapped));
            shaqis.Set(id, std::move(e));
        }
    }
    inv->Set("MainWeaponInfos", std::move(mains));
    inv->Set("ShaChiWeaponInfo", std::move(shaqis));
    auto names = [](const std::vector<std::string>& v) { Value a = Value::Arr(); for (auto& s : v) a.items.push_back(Value::Str(s)); return a; };
    inv->Set("MainWeapons", names(eqUse.main));
    inv->Set("ShaqiWeapons", names(eqUse.shaqi));
    inv->Set("CurrentWeaponIndex", Value::Num(0));
    inv->Set("CurrentMainWeaponID", Value::Str(eqUse.main[0]));
    inv->Set("CurrentShaqiWeaponIndex", Value::Num(0));
    // 药：等级从 0 起；当前 / 上限次数按塔内药瓶等级
    Value meds = Value::Obj();
    const int level0 = std::clamp(p.medLevel, 1, growth::kMedLevelMax) - 1;
    for (int m : p.medicines) {
        const auto& d = growth::kMedicines[m];
        Value e = oldInfo("MedicineInfos", std::string(d.id)) ? *oldInfo("MedicineInfos", std::string(d.id)) : Value::Obj();
        if (e.fields.empty()) { e.Set("AlreadyHasUIRedPoint", Value::Boolean(false)); e.Set("DirtyTime", Value::Str(stamp)); e.Set("AcquiredCycle", Value::Num(0)); }
        e.Set("CurrentLevel", Value::Num(level0));
        e.Set("CurrentTime", Value::Num(p.MedUses(m)));
        e.Set("MaxTime", Value::Num(p.MedUses(m)));
        e.Set("BuffRemainTime", Value::Num(0));
        meds.Set(d.id, std::move(e));
    }
    inv->Set("MedicineInfos", std::move(meds));
    inv->Set("BottomMedicine", Value::Str(eq.meds.size() > 0 ? eq.meds[0] : "None"));
    inv->Set("TopMedicine", Value::Str(eq.meds.size() > 1 ? eq.meds[1] : "None"));
    // 饰品：只留塔档里的；精炼 1 级 = CurrentLevel 0
    Value accs = Value::Obj();
    for (auto& a : p.accessories) {
        const auto& d = growth::kAccessories[a.index];
        Value e = oldInfo("AccessoryInfos", std::string(d.id)) ? *oldInfo("AccessoryInfos", std::string(d.id)) : Value::Obj();
        if (e.fields.empty()) { e.Set("DirtyTime", Value::Str(stamp)); e.Set("AcquiredCycle", Value::Num(0)); e.Set("HasUIRedPoint", Value::Boolean(false)); }
        e.Erase("GrantedByEntitlement");
        e.Set("CurrentLevel", Value::Num(std::max(0, a.refine - 1)));
        accs.Set(d.id, std::move(e));
    }
    const Value* ownInv = entitlementSave ? entitlementSave->Find("Inventory") : inv;
    if (const Value* old = ownInv ? ownInv->Find("AccessoryInfos") : nullptr; old && old->kind == Value::Object)
        for (auto& [id, e] : old->fields)
            if (const Value* g = e.Find("GrantedByEntitlement"); g && g->kind == Value::Bool && g->b && !accs.Find(id)) {
                // 1.0.1 多周目：二周目原档的赠品带着 AcquiredCycle = N、可能还精炼过；塔档周目是 0，统一成 0 周目、未精炼，
                // 和塔档里其他物品一致（周目对不上原生可能当成"别的周目的物品"处理；精炼过的赠品也不该比别人强）
                Value gift = e;
                for (const char* k : {"AcquiredCycle", "FirstAcquiredCycle"}) if (gift.Find(k)) gift.Set(k, Value::Num(0));
                if (gift.Find("CurrentLevel")) gift.Set("CurrentLevel", Value::Num(0));
                accs.Set(id, std::move(gift));
            }
    inv->Set("AccessoryInfos", std::move(accs));
    Value slots = Value::Arr();
    for (size_t k = 0; k < 6; ++k) slots.items.push_back(Value::Str(k < eq.accessories.size() ? eq.accessories[k] : "None"));
    inv->Set("AccessoryIDs", std::move(slots));
    // 材料清空；临时装备 / 强制装备状态关掉（塔内存档从干净的装备状态开始）
    if (Value* items = inv->Find("InventoryInfos"); items && items->kind == Value::Object)
        std::erase_if(items->fields, [](auto& f) { return IsUpgradeMaterial(f.first) || IsPermanentBoost(f.first); });
    if (Value* f = inv->Find("ForcedLoadoutActive")) *f = Value::Boolean(false);
    if (Value* f = inv->Find("ZlgwBareMode")) *f = Value::Boolean(false);
    player->Set("Dust", Value::Num(0));
    // 能力节点：存档里有两棵同 ID 的节点树（AbilityNode_RenewalHun 与 _Climax），之前只改了前一棵，
    // 后一棵原档点过的节点（榕树之心、记忆之花等）原样进了塔。两棵都按塔档写
    for (Value* list : {nodes, [&]() -> Value* { Value* c = r.save.Find("AbilityNode_RenewalHun_Climax"); return c ? c->Find("AbilityNodes") : nullptr; }()}) {
        if (!list || list->kind != Value::Object) continue;
        for (auto& [id, v] : list->fields) {
            const int n = growth::NodeIndex(id);
            v = Value::Num(n >= 0 ? p.nodes[n] : 0);
        }
    }
    r.resetTags = ResetReplayTags(r.save);
    r.resetNodes = ResetReplayNodes(r.save);
    r.ok = true;
    return r;
}

}  // namespace tower::save
