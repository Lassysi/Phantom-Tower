// 影塔进出塔流程（纯逻辑）：把塔档落到游戏里、记账、出塔还原。游戏这一侧全部经过 IGame 接口，
// 引擎里接真实的原生调用（沙盒、背包、能力组件……），离线测试接假的游戏端。
// 顺序来自 2026-10-30 的实机教训（notes/tower-growth-plan.md）：
//   进塔：备份存档 → 开沙盒 → 停成就 → 记账 → 改游戏
//   出塔：按账本逆向撤销 → 沙盒改成"读真存档" → 回标题再进档 → 进档完成之后才关沙盒、恢复成就
//   沙盒只要在进档完成前关掉，期间一次自动存档就会把塔内状态写进真存档。
#pragma once
#include <algorithm>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "tower_growth.h"

namespace tower::session {

// 游戏侧接口。返回 false 表示这一步失败（流程据此中止或报错）
struct IGame {
    virtual ~IGame() = default;
    virtual bool BackupSave(std::string* where) = 0;
    virtual bool Sandbox(bool on) = 0;             // 存档写入改道
    virtual bool SandboxReadThrough() = 0;          // 清空沙盒里的存档，读档时读到的是真存档（沙盒仍然开着）
    virtual void Achievements(bool blocked) = 0;
    virtual int WeaponLevel(std::string_view id, int kind) = 0;  // 没有返回 -1
    virtual bool SetWeaponLevel(std::string_view id, int kind, int level) = 0;
    virtual bool GrantWeapon(std::string_view id, int kind) = 0;
    virtual bool RemoveWeapon(std::string_view, int) { return false; }
    virtual void FixCurrentSlots() {}
    virtual int AddShaqiBullet(int) { return -1; }                  // 3.3.4 给当前影之武装加暗源；返回加后读回的暗源（-1 失败）
    virtual int ShaqiBullets() { return -1; }                       // 3.3.4 当前暗源（共用池）；读不到 -1                               // 3.3.2 交易后手持槽指向空槽时切到还在的兵器
    virtual bool RemoveMedicine(std::string_view) { return false; }   // N9i 驿站回收药（RemovePropsFromBackPack，不行再 RemoveMedicineConfig）
    virtual bool RemoveAccessory(std::string_view) { return false; }  // N9i 驿站回收饰品（RemovePropsFromBackPack）  // 3.1.0 驿站交易：原生删兵器（RemoveWeaponByID），读回没有了才 true
    virtual bool SnapWeapon(std::string_view id, int kind) = 0;
    virtual std::vector<std::string> OwnedWeapons(int kind) = 0;
    virtual int NodeLevel(std::string_view id) = 0;
    virtual bool SetNodeLevel(std::string_view id, int level) = 0;
    virtual bool HasMedicine(std::string_view id) = 0;
    virtual bool GrantMedicine(std::string_view id) = 0;
    virtual int MedicineLevel(std::string_view id) = 0;
    virtual bool SetMedicineLevel(std::string_view id, int level) = 0;
    virtual bool HasAccessory(std::string_view id) = 0;
    virtual bool GrantAccessory(std::string_view id) = 0;
    virtual int AccessoryLevel(std::string_view) { return -1; }          // 塔内约定：原生 CurrentLevel + 1；没有 -1
    virtual bool SetAccessoryLevel(std::string_view, int) { return false; } // 只升不降（原生没有降级接口）
    virtual int ItemCount(std::string_view) { return -1; }              // 道具持有数；不支持 -1
    virtual int MedicineUsesLeft() { return -1; }                      // 全部药瓶剩余次数之和（禁药挑战）；读不到 -1
    virtual bool GrantItem(std::string_view, int) { return false; }    // Boss 原生道具掉落（原生获得界面），读回增加才 true
    virtual bool ReloadFromTitle() = 0;            // 开始回标题再进档
    virtual bool ReloadFinished() = 0;             // 轮询：新存档已进完
};

// 账本：每个被改过的东西只记第一次改之前的值（撤销用）；发放类（游戏里本来没有）只能靠读档撤销
struct Entry {
    enum Kind { WeaponLevel, NodeLevel, MedicineLevel, Granted } kind;
    std::string id;
    int sub = 0;   // 兵器：0 主武器 / 1 影之武装
    int old = 0;   // 原值
};

enum class Phase { Outside, Inside, ExitReload, Failed };
inline std::string_view PhaseName(Phase p) {
    switch (p) {
        case Phase::Outside: return "outside";
        case Phase::Inside: return "inside";
        case Phase::ExitReload: return "exit_reload";
        case Phase::Failed: return "failed";
    }
    return "?";
}

class Session {
public:
    Phase phase() const { return phase_; }
    const std::vector<Entry>& journal() const { return journal_; }
    const std::string& error() const { return error_; }

    // 进塔：任何一步失败都退回去，不留半截状态
    bool Enter(IGame& g, const growth::Profile& p) {
        if (phase_ != Phase::Outside) return Fail("已经在塔里");
        std::string where;
        if (!g.BackupSave(&where)) return Fail("存档备份失败，没有进塔");
        if (!g.Sandbox(true)) return Fail("存档沙盒开不了，没有进塔");
        g.Achievements(true);
        phase_ = Phase::Inside;
        journal_.clear();
        backup_ = where;
        Sync(g, p, true);
        return true;
    }
    // 独立塔档：塔内存档已经按塔档生成并载入（tower_trip.h），这里只登记"在塔里"，之后的改动照常 Sync
    void Adopt() { phase_ = Phase::Inside; journal_.clear(); snapped_.clear(); error_.clear(); }
    void Release() { phase_ = Phase::Outside; journal_.clear(); snapped_.clear(); }
    // 塔档有变化（升级、回退、拿到东西）：把差异落到游戏里。first：进塔那次，把塔档以外的兵器降到 1 级
    void Sync(IGame& g, const growth::Profile& p, bool first = false) {
        if (phase_ != Phase::Inside) return;
        for (const auto& w : p.weapons) {
            const auto& d = growth::kWeapons[w.index];
            const int now = g.WeaponLevel(d.id, d.kind);
            if (now < 0) { Note(Entry::Granted, d.id, d.kind, 0); g.GrantWeapon(d.id, d.kind); }
            if (now != w.level) { Note(Entry::WeaponLevel, d.id, d.kind, now < 0 ? 1 : now); g.SetWeaponLevel(d.id, d.kind, w.level); }
            if (w.snapped && !snapped_.count(std::string(d.id))) { Note(Entry::Granted, std::string("snap:") + std::string(d.id), d.kind, 0); g.SnapWeapon(d.id, d.kind); snapped_.insert(std::string(d.id)); }
        }
        if (first)  // 塔档以外的兵器：原生背包删不掉（实机），降到 1 级，出塔还原
            for (int kind : {0, 1})
                for (const auto& id : g.OwnedWeapons(kind)) {
                    const int wi = growth::WeaponIndex(id, kind);
                    if (wi >= 0 && p.Find(wi) >= 0) continue;
                    const int now = g.WeaponLevel(id, kind);
                    if (now > 1) { Note(Entry::WeaponLevel, id, kind, now); g.SetWeaponLevel(id, kind, 1); }
                }
        for (int i = 0; i < growth::kNodeCount; ++i) {
            const auto id = growth::kNodes[i].id;
            const int now = g.NodeLevel(id);
            if (now != p.nodes[i]) { Note(Entry::NodeLevel, id, 0, now); g.SetNodeLevel(id, p.nodes[i]); }
        }
        for (int m : p.medicines) {
            const auto id = growth::kMedicines[m].id;
            if (!g.HasMedicine(id)) { Note(Entry::Granted, std::string("med:") + std::string(id), 0, 0); g.GrantMedicine(id); }
            if (const int now = g.MedicineLevel(id); now != p.medLevel) { Note(Entry::MedicineLevel, id, 0, now); g.SetMedicineLevel(id, p.medLevel); }
        }
        for (const auto& a : p.accessories) {
            const auto id = growth::kAccessories[a.index].id;
            if (!g.HasAccessory(id)) { Note(Entry::Granted, std::string("acc:") + std::string(id), 0, 0); g.GrantAccessory(id); }
            // 用户 2026-11-11：饰品精炼（奖励页 / 淬炼点升级）要写进原生等级，效果才生效；原生升级会重挂已装备饰品的 GE
            if (const int now = g.AccessoryLevel(id); now > 0 && now < a.refine) { Note(Entry::Granted, std::string("acclv:") + std::string(id), 0, now); g.SetAccessoryLevel(id, a.refine); }
        }
    }
    // 出塔：先撤销能撤的，再读档把发放类也还原；沙盒到进档完成才关
    bool Exit(IGame& g) {
        if (phase_ != Phase::Inside) return Fail("不在塔里");
        Undo(g);
        if (!g.SandboxReadThrough()) return Fail("沙盒切换失败：请手动回标题再进存档（沙盒保持开着）");
        if (!g.ReloadFromTitle()) return Fail("没能回标题：请手动回标题再进存档（沙盒保持开着）");
        phase_ = Phase::ExitReload;
        return true;
    }
    // 每帧：读档完成后才关沙盒
    void Tick(IGame& g) {
        if (phase_ != Phase::ExitReload || !g.ReloadFinished()) return;
        g.Sandbox(false);
        g.Achievements(false);
        journal_.clear(); snapped_.clear();
        phase_ = Phase::Outside;
    }
    // 运行库要停（热换 / 退出游戏）而还在塔里：钩子马上会被拆，能做的只有把改过的等级先撤回去，
    // 这样之后即使自动存档，写进去的也是原来的等级（发放进背包的东西撤不掉，账本留着供下次启动提示）
    void OnRuntimeStopping(IGame& g) {
        if (phase_ == Phase::Inside) Undo(g);
    }

    // ---- 崩溃恢复：账本存到 %LOCALAPPDATA%\PBZTower\session.txt；下次启动读到"在塔里"就走出塔流程
    std::string Save() const {
        std::ostringstream o;
        o << "phase=" << PhaseName(phase_) << "\nbackup=" << backup_ << '\n';
        for (const auto& e : journal_) o << "e=" << static_cast<int>(e.kind) << '\t' << e.id << '\t' << e.sub << '\t' << e.old << '\n';
        return o.str();
    }
    static Session Load(std::string_view text) {
        Session s;
        size_t a = 0;
        while (a < text.size()) {
            size_t b = text.find('\n', a);
            if (b == std::string_view::npos) b = text.size();
            auto line = text.substr(a, b - a);
            a = b + 1;
            if (line.rfind("phase=", 0) == 0) {
                const auto v = line.substr(6);
                s.phase_ = v == "inside" ? Phase::Inside : v == "exit_reload" ? Phase::ExitReload : Phase::Outside;
            } else if (line.rfind("backup=", 0) == 0) s.backup_ = std::string(line.substr(7));
            else if (line.rfind("e=", 0) == 0) {
                std::vector<std::string> f;
                std::string cur;
                for (char c : line.substr(2)) { if (c == '\t') { f.push_back(cur); cur.clear(); } else cur += c; }
                f.push_back(cur);
                if (f.size() == 4) s.journal_.push_back({static_cast<Entry::Kind>(std::stoi(f[0])), f[1], std::stoi(f[2]), std::stoi(f[3])});
            }
        }
        return s;
    }
    // 启动时：上次没正常出塔 → 立刻重新开沙盒，再走出塔（撤销 + 读档）
    bool Recover(IGame& g) {
        if (phase_ == Phase::Outside) return false;
        g.Sandbox(true);
        g.Achievements(true);
        phase_ = Phase::Inside;
        return Exit(g);
    }

private:
    bool Fail(std::string why) { error_ = std::move(why); return false; }
    void Note(Entry::Kind k, std::string_view id, int sub, int old) {
        for (const auto& e : journal_) if (e.kind == k && e.id == id && e.sub == sub) return;  // 只记第一次的原值
        journal_.push_back({k, std::string(id), sub, old});
    }
    void Undo(IGame& g) {
        for (auto it = journal_.rbegin(); it != journal_.rend(); ++it) {
            switch (it->kind) {
                case Entry::WeaponLevel: g.SetWeaponLevel(it->id, it->sub, it->old); break;
                case Entry::NodeLevel: g.SetNodeLevel(it->id, it->old); break;
                case Entry::MedicineLevel: g.SetMedicineLevel(it->id, it->old); break;
                case Entry::Granted: break;  // 只有读档能撤
            }
        }
    }

    Phase phase_ = Phase::Outside;
    std::vector<Entry> journal_;
    std::string error_, backup_;
    struct Set {  // 小集合：避免为几个字符串引入 <set>
        std::vector<std::string> v;
        bool count(const std::string& s) const { return std::find(v.begin(), v.end(), s) != v.end(); }
        void insert(std::string s) { if (!count(s)) v.push_back(std::move(s)); }
        void clear() { v.clear(); }
    } snapped_;
};

}  // namespace tower::session
