// 阵营血条着色：本工具生成并改过阵营的怪，其头顶血条（WB_EnemyHPBar）/ Boss 血条（WB_BossHPBar）按阵营上色，
// 斗蛐蛐时一眼分清敌我：我方 = 游戏自带的友方血条样式（InitNPCHelathBar）+ 蓝色、Boss 名字蓝色；混战橙、敌方二队紫、三队绿；
// 敌方保持游戏原样。血条控件靠每帧一小段增量扫描收集，每 0.5 秒对属于这些怪的血条重设颜色（游戏会重建 / 刷新控件）。
// 同一轮里把我方怪从锁定候选表（TargetingWorldSubsystem.TargetableActors）里摘掉：中键锁定 / 切换目标不会选到队友。
// 只在原数组里前移元素、减 Num，不分配内存；关闭时把摘掉的项放回原数组空位。
// Boss 血条：名字前加阵营（己方：/混战：/二队：/三队：）；有生成 Boss 时用自建看板（顶部敌方系一行 5 条、右侧我方一列 6 条，超出每 2 秒翻页）。
// 死亡登记与尸体生命周期由 enemy_spawn 独立维护，不随血条开关/阵营变化；这里不强制角色切死亡状态。
// 生成物定级由 enemy_spawn 原生 Init Level / ReinitializeAttributes 处理，血条不再按血量/攻击比值外推。
// 我方怪游戏不显示血条（只给敌对目标显示）：小怪头顶血条强制显示（UpdateBarVis），Boss 由看板自建血条显示。
#include <algorithm>
#include <array>
#include <cmath>
#include <cwchar>
#include <string_view>
#include <cstring>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "../../host/log.h"
#include "../../services/attributes.h"
#include "../../services/inventory.h"
#include "../../services/process_event.h"
#include "../feature.h"
#include "enemy_spawn.h"

namespace pbz {
namespace {

struct Rgba { float r, g, b, a; };

const wchar_t* LabelOf(const std::string& faction) {
    if (faction == "Faction.Player") return L"己方：";
    if (faction == "Faction.Chaotic") return L"混战：";
    if (faction == "Faction.Enemy2") return L"二队：";
    if (faction == "Faction.Enemy3") return L"三队：";
    return L"";
}

// 反射调用参数缓冲：按 UFunction 的参数属性名取偏移
struct Args {
    ue::UObject* fn;
    std::vector<uint8_t> buf;
    explicit Args(ue::UObject* f) : fn(f), buf(0x200, 0) {}
    uint8_t* At(std::string_view name) {
        for (auto* p = ue::Get<uint8_t*>(fn + ue::Layout::StructChildProps); p; p = ue::Get<uint8_t*>(p + ue::Layout::FieldNext))
            if (ue::Name(ue::Get<uint32_t>(p + ue::Layout::FieldName)) == name) {
                const int o = ue::Get<int32_t>(p + ue::Layout::PropOffset);
                if (o >= 0 && o < 0x1C0) return buf.data() + o;
            }
        static uint8_t sink[64];
        return sink;
    }
};

Rgba ColorOf(const std::string& faction) {
    if (faction == "Faction.Player") return {0.25f, 0.6f, 1.0f, 1};    // 我方：蓝
    if (faction == "Faction.Chaotic") return {1.0f, 0.55f, 0.1f, 1};   // 混战：橙
    if (faction == "Faction.Enemy2") return {0.75f, 0.35f, 1.0f, 1};   // 敌方二队：紫
    if (faction == "Faction.Enemy3") return {0.3f, 0.9f, 0.4f, 1};     // 敌方三队：绿
    return {1, 1, 1, 1};
}

class FactionBars : public IFeature {
public:
    const char* Id() const override { return "ui.faction_bars"; }
    bool IsToggle() const override { return true; }
    bool DefaultOn() const override { return true; }
    bool Resolve(std::string* why) override {
        bossCls_ = ue::FindClass("WB_BossHPBar_C");
        enemyCls_ = ue::FindClass("WB_EnemyHPBar_C");
        barCls_ = ue::FindClass("PBZAnimProgressBar_C");
        auto* pb = ue::FindClass("ProgressBar");
        auto* img = ue::FindClass("Image");
        auto* text = ue::FindClass("TextBlock");
        targetingCls_ = ue::FindClass("TargetingWorldSubsystem");
        if (!pb || !img || !text) { *why = "界面类未加载"; return false; }
        fnFill_ = pe::FindFunction(pb, "SetFillColorAndOpacity");
        fnImg_ = pe::FindFunction(img, "SetColorAndOpacity");
        fnText_ = pe::FindFunction(text, "SetColorAndOpacity");
        if (!fnFill_ || !fnImg_ || !fnText_) { *why = "界面颜色接口缺失"; return false; }
        return true;
    }
    void Enable() override { enabled = true; }
    void Disable() override {
        enabled = false;
        RestoreTargets();
        RestoreNames();
        ClearBoard();
        shown_.clear();
        bars_.clear();
        tinted_.clear();
    }
    void Tick(float dt) override {
        // 血条类随关卡 / 界面加载才出现：没解析到时每次重试（FindClass 有缓存）
        if (!bossCls_) bossCls_ = ue::FindClass("WB_BossHPBar_C");
        if (!enemyCls_) enemyCls_ = ue::FindClass("WB_EnemyHPBar_C");
        if (!barCls_) barCls_ = ue::FindClass("PBZAnimProgressBar_C");
        if (!targetingCls_) targetingCls_ = ue::FindClass("TargetingWorldSubsystem");
        now_ += dt;
        KeepCardScale();
        if ((since_ += dt) < 0.5f) return;
        since_ = 0;
        const auto owned = enemies::OwnedFactions();
        if (owned.empty() && !enemies::OwnedBosses().empty()) {  // 只有默认敌方阵营的生成 Boss：只管看板
            const auto bosses = enemies::OwnedBosses();
            Board(bosses.front(), bosses, {});
            return;
        }
        if (owned.empty()) { ClearBoard(); bars_.clear(); tinted_.clear(); shown_.clear(); names_.clear(); RestoreTargets(); return; }
        // 有改过阵营的怪时才收集血条控件：每 0.5 秒扫一段对象表（约 2 万个），一轮几秒，不卡帧
        ue::UObject* batch[2048];
        for (int k = 0; k < 10; ++k) {
            const int n = ue::ObjBatch(cursor_, 2048, batch);
            for (int i = 0; i < n; ++i) {
                auto* o = batch[i];
                if (!o || ue::IsDefault(o)) continue;
                auto* c = ue::ClassOf(o);
                if ((bossCls_ && c == bossCls_) || (enemyCls_ && c == enemyCls_)) bars_.insert(o);
                else if (targetingCls_ && c == targetingCls_) targeting_ = o;
            }
            cursor_ = cursor_ + 2048 >= ue::Num() ? 0 : cursor_ + 2048;
        }
        std::unordered_map<ue::UObject*, std::string> factions(owned.begin(), owned.end());
        const auto bossList = enemies::OwnedBosses();
        const std::unordered_set<ue::UObject*> bossActors(bossList.begin(), bossList.end());
        Board(owned.front().first, bossList, factions);
        InputGuard(owned.front().first);
        for (auto it = bars_.begin(); it != bars_.end();) {
            auto* w = *it;
            auto* c = ue::ClassOf(w);
            if (c != bossCls_ && c != enemyCls_) { tinted_.erase(w); it = bars_.erase(it); continue; }
            const bool boss = c == bossCls_;
            const int offActor = boss ? Off(bossCls_, "Character", offBossChar_) : Off(enemyCls_, "SelfActor", offEnemyActor_);
            auto* actor = offActor >= 0 ? ue::Get<ue::UObject*>(w + offActor) : nullptr;
            auto f = actor ? factions.find(actor) : factions.end();
            if (f != factions.end()) {
                Tint(w, boss, f->second);
                if (!boss && f->second == "Faction.Player" && !bossActors.count(actor)) ShowHeadBar(w);
            }
            ++it;
        }
        std::unordered_set<int32_t> allies;
        for (auto& [actor, faction] : owned)
            if (faction == "Faction.Player") allies.insert(ue::Get<int32_t>(actor + ue::Layout::ObjIndex, -1));
        if (!allies.empty()) Unlockable(allies);

    }

private:
    // TMap<FGameplayTag, FTargetingWorldSubSystemTargets>：TSet 元素 0x20 = Tag(8) + TArray<TWeakObjectPtr<Actor>>(0x10) + 哈希(8)
    struct Slots { uint8_t* data = nullptr; int count = 0; uint32_t bits[8] = {}; };
    // TSet / TMap 的稀疏数组头：Data, Num, Max, AllocationFlags(内联 4 dword + 指针 + NumBits + MaxBits), FirstFree, NumFree
    static bool ReadSparse(uint8_t* map, Slots& s) {
        uint8_t h[0x38];
        if (!map || !ue::Read(map, h, sizeof h)) return false;
        int count, max, bitMax;
        std::memcpy(&s.data, h, 8); std::memcpy(&count, h + 8, 4); std::memcpy(&max, h + 12, 4); std::memcpy(&bitMax, h + 0x2c, 4);
        if (count < 0 || count > 256 || max < count || (count && !s.data)) return false;
        uint8_t* bitData = map + 0x10;
        if (bitMax > 128) std::memcpy(&bitData, h + 0x20, 8);
        if (count && !ue::Read(bitData, s.bits, ((count + 31) / 32) * 4)) return false;
        s.count = count;
        return true;
    }
    static bool Used(const Slots& s, int i) { return s.bits[i / 32] & (1u << (i % 32)); }
    bool ReadMap(Slots& s) {
        if (!targeting_ || !targetingCls_ || !ue::IsA(targeting_, targetingCls_)) { targeting_ = nullptr; return false; }
        if (offMap_ == -2) offMap_ = ue::PropOffset(targetingCls_, "TargetableActors");
        return offMap_ >= 0 && ReadSparse(targeting_ + offMap_, s);
    }
    // 小怪头顶血条：游戏对非敌对目标保持隐藏（WantToShow=0），每 2 秒检查一次，隐藏了就重新显示
    void ShowHeadBar(ue::UObject* w) {
        if (offWant_ == -2) offWant_ = ue::PropOffset(enemyCls_, "WantToShow");
        if (offWant_ < 0 || ue::Get<uint8_t>(w + offWant_)) return;
        auto& last = shown_[w];
        if (last && now_ - last < 2) return;
        last = now_;
        const uint8_t one = 1;
        ue::Write(w + offWant_, &one, 1);
        if (auto* f = pe::FindFunction(enemyCls_, "ReadyToUpdateVis")) { uint8_t x[16] = {1}; pe::Call(w, f, x); }
        if (auto* f = pe::FindFunction(enemyCls_, "UpdateBarVis")) { uint8_t x[16] = {1}; pe::Call(w, f, x); }
    }
    ue::UObject* Hud(ue::UObject* context) {
        if (!hudCls_) hudCls_ = ue::FindClass("PBZHUDLayout");
        if (!hudCls_) return nullptr;
        if (!hudCdo_) ue::ForEach([&](int, ue::UObject* o) {
            if (ue::ClassOf(o) == hudCls_ && ue::IsDefault(o)) { hudCdo_ = o; return false; }
            return true;
        });
        auto* fn = pe::FindFunction(hudCls_, "GetHUDLayout");
        if (!hudCdo_ || !fn) return nullptr;
        uint8_t x[32] = {};
        std::memcpy(x, &context, 8);
        pe::Call(hudCdo_, fn, x);
        ue::UObject* hud;
        std::memcpy(&hud, x + 8, 8);
        return hud && ue::IsA(hud, hudCls_) ? hud : nullptr;
    }
    void Unlockable(const std::unordered_set<int32_t>& allies) {
        Slots s;
        if (!ReadMap(s)) return;
        for (int i = 0; i < s.count; ++i) {
            if (!Used(s, i)) continue;
            auto* arr = s.data + i * 0x20 + 8;
            auto* items = ue::Get<uint8_t*>(arr);
            int num = ue::Get<int32_t>(arr + 8), max = ue::Get<int32_t>(arr + 12);
            if (!items || num <= 0 || num > max || max > 4096) continue;
            std::vector<uint64_t> weak(num);  // TWeakObjectPtr = {ObjectIndex, SerialNumber}
            if (!ue::Read(items, weak.data(), num * 8)) continue;
            int kept = 0;
            for (auto w : weak) {
                if (allies.count(int32_t(w & 0xffffffff))) {
                    if (std::find(removed_.begin(), removed_.end(), std::pair{arr, w}) == removed_.end()) removed_.push_back({arr, w});
                    continue;
                }
                weak[kept++] = w;
            }
            if (kept == num) continue;
            if (ue::Write(items, weak.data(), kept * 8)) ue::Write(arr + 8, &kept, 4);
            if (!logged_) { logged_ = true; log::Info("我方怪已移出锁定候选（{} 个）", num - kept); }
        }
    }
    void RestoreTargets() {
        // 只放回仍在同一个数组、且数组还有空位的项；对象已销毁的弱引用游戏自己会跳过
        Slots s;
        if (!removed_.empty() && ReadMap(s))
            for (auto& [arr, w] : removed_) {
                bool live = false;
                for (int i = 0; i < s.count && !live; ++i) live = Used(s, i) && s.data + i * 0x20 + 8 == arr;
                if (!live) continue;
                auto* items = ue::Get<uint8_t*>(arr);
                int num = ue::Get<int32_t>(arr + 8), max = ue::Get<int32_t>(arr + 12);
                if (items && num < max && ue::Write(items + num * 8, &w, 8)) { ++num; ue::Write(arr + 8, &num, 4); }
            }
        removed_.clear();
        logged_ = false;
    }
    static int Off(ue::UObject* cls, const char* name, int& cache) {
        if (cache == -2) cache = cls ? ue::PropOffset(cls, name) : -1;
        return cache;
    }
    void CallColor(ue::UObject* obj, ue::UObject* fn, const Rgba& c, bool slate) {
        if (!obj || !fn) return;
        uint8_t args[64] = {};
        std::memcpy(args, &c, sizeof c);  // FLinearColor；FSlateColor 首字段同为 FLinearColor，ColorUseRule=0（指定颜色）
        (void)slate;
        pe::Call(obj, fn, args);
    }
    ue::UObject* Cdo(const char* cls) {
        auto& o = cdo_[cls];
        if (!o) if (auto* c = ue::FindClass(cls)) ue::ForEach([&](int, ue::UObject* x) {
            if (ue::ClassOf(x) == c && ue::IsDefault(x)) { o = x; return false; }
            return true;
        });
        return o;
    }
    // TextBlock 文本 ↔ 字符串（KismetTextLibrary）；读一次原名，之后每 3 秒按"前缀 + 原名"重设（游戏刷新控件会改回原名）
    bool ReadText(ue::UObject* text, std::wstring* out) {
        auto* get = pe::FindFunction(ue::ClassOf(text), "GetText");
        auto* lib = Cdo("KismetTextLibrary");
        auto* conv = lib ? pe::FindFunction(ue::ClassOf(lib), "Conv_TextToString") : nullptr;
        if (!get || !conv) return false;
        Args g(get);
        pe::Call(text, get, g.buf.data());
        Args c(conv);
        std::memcpy(c.At("InText"), g.At("ReturnValue"), 0x18);
        pe::Call(lib, conv, c.buf.data());
        auto* data = ue::Get<wchar_t*>(c.At("ReturnValue"));
        const int num = ue::Get<int32_t>(c.At("ReturnValue") + 8);
        if (!data || num <= 0 || num > 256) return false;
        std::wstring s(static_cast<size_t>(num - 1), L'\0');
        if (!ue::Read(data, s.data(), (num - 1) * sizeof(wchar_t))) return false;
        *out = s;
        return true;
    }
    void WriteText(ue::UObject* text, const std::wstring& value) {
        auto* set = pe::FindFunction(ue::ClassOf(text), "SetText");
        auto* lib = Cdo("KismetTextLibrary");
        auto* conv = lib ? pe::FindFunction(ue::ClassOf(lib), "Conv_StringToText") : nullptr;
        if (!set || !conv) return;
        std::wstring buf = value;  // FString 参数：指向本地缓冲，Num 含结尾 0；函数内部自己复制
        struct { wchar_t* data; int32_t num, max; } str{buf.data(), int32_t(buf.size() + 1), int32_t(buf.size() + 1)};
        Args c(conv);
        std::memcpy(c.At("InString"), &str, sizeof str);
        pe::Call(lib, conv, c.buf.data());
        Args t(set);
        std::memcpy(t.At("InText"), c.At("ReturnValue"), 0x18);
        pe::Call(text, set, t.buf.data());
    }
    void Label(ue::UObject* w, ue::UObject* text, const std::string& faction) {
        const std::wstring label = LabelOf(faction);
        if (!text || label.empty()) return;
        auto it = names_.find(w);
        if (it == names_.end()) {
            std::wstring original;
            if (!ReadText(text, &original)) return;
            for (auto* l : {L"己方：", L"混战：", L"二队：", L"三队："})
                if (original.rfind(l, 0) == 0) original = original.substr(wcslen(l));
            it = names_.emplace(w, Name{text, original, -100}).first;
        }
        if (now_ - it->second.at < 3) return;
        it->second.at = now_;
        WriteText(text, label + it->second.original);
    }
    void RestoreNames() {
        for (auto& [w, n] : names_)
            if (bossCls_ && ue::IsA(w, bossCls_) && ue::Get<ue::UObject*>(w + Off(bossCls_, "BossName", offName_)) == n.text) WriteText(n.text, n.original);
        names_.clear();
    }

    // Boss 血条分行：BossPoint 是横向 DynamicEntryBox，条数多了每条被压窄。超过 4 条时用渲染偏移排成每行 4 条、每行居中，
    // 敌方系在顶部一行（最多 5 条，多了每 2 秒滚动一格），我方在右侧一列（最多 6 条，同样滚动）。位置一律实测：血条集合变化时先清掉偏移，等 1 秒布局稳定，
    // 再读每条的屏幕矩形（按实际从左到右排序）算目标位置。只改显示，不碰任何数值。
    void Widget2(ue::UObject* w, const char* fn, double x, double y) {
        auto* f = pe::FindFunction(ue::ClassOf(w), fn);
        if (!f) return;
        Args a(f);
        const double v[2] = {x, y};
        std::memcpy(a.buf.data(), v, 16);
        *a.At("bRemoveDPIScale") = 1;  // SetPositionInViewport：按像素给坐标；其他函数没有这个参数（写到占位缓冲）
        pe::Call(w, f, a.buf.data());
    }
    // 控件的屏幕矩形（绝对坐标）与本地尺寸
    struct Rect { double x, y, w, h, localW; };
    bool Measure(ue::UObject* w, Rect& r) {
        auto* geom = pe::FindFunction(ue::ClassOf(w), "GetCachedGeometry");
        auto* lib = Cdo("SlateBlueprintLibrary");
        auto* size = lib ? pe::FindFunction(ue::ClassOf(lib), "GetLocalSize") : nullptr;
        auto* l2a = lib ? pe::FindFunction(ue::ClassOf(lib), "LocalToAbsolute") : nullptr;
        if (!geom || !size || !l2a) return false;
        Args g(geom);
        pe::Call(w, geom, g.buf.data());
        double local[2], tl[2], br[2];
        Args sz(size);
        std::memcpy(sz.At("Geometry"), g.At("ReturnValue"), 0x38);
        pe::Call(lib, size, sz.buf.data());
        std::memcpy(local, sz.At("ReturnValue"), 16);
        auto toAbs = [&](double lx, double ly, double out[2]) {
            Args a(l2a);
            std::memcpy(a.At("Geometry"), g.At("ReturnValue"), 0x38);
            const double v[2] = {lx, ly};
            std::memcpy(a.At("LocalCoordinate"), v, 16);
            pe::Call(lib, l2a, a.buf.data());
            std::memcpy(out, a.At("ReturnValue"), 16);
        };
        toAbs(0, 0, tl);
        toAbs(local[0], local[1], br);
        r = {tl[0], tl[1], br[0] - tl[0], br[1] - tl[1], local[0]};
        return std::isfinite(r.x) && std::isfinite(r.y) && r.w > 1 && r.h > 1 && local[0] > 1;
    }
    // Boss 血条看板：有本工具生成的 Boss 时，不再动游戏自己那一排（它会自己压缩 / 重排，跟我们抢位置导致闪烁、重叠），
    // 而是把那一排藏起来（透明），为每个要显示的 Boss 自建一个 WB_BossHPBar 加到屏幕上，按像素直接放到固定位置：
    //   顶部：敌方 / 二队 / 三队 / 混战（本工具生成的非我方 Boss + 游戏自己挂上的 Boss），一行最多 5 条；
    //   右侧：我方 Boss 一列，最多 6 条；
    //   超过就每 2 秒整页轮换。新 Boss 直接出现在自己的位置，量好尺寸前保持透明，不闪。只改显示，不碰数值。
    static constexpr size_t kTopShow = 5, kSideShow = 6;
    struct Card { ue::UObject* w = nullptr; double natW = 0, natH = 0; double x = -1, y = -1, s = -1; bool visible = false; };
    ue::UObject* Pc(ue::UObject* context) {
        auto* gs = Cdo("GameplayStatics");
        auto* getPc = gs ? pe::FindFunction(ue::ClassOf(gs), "GetPlayerController") : nullptr;
        if (!getPc) return nullptr;
        Args g(getPc);
        std::memcpy(g.At("WorldContextObject"), &context, 8);
        pe::Call(gs, getPc, g.buf.data());
        return ue::Get<ue::UObject*>(g.At("ReturnValue"));
    }
    void Call0(ue::UObject* w, const char* fn) { if (auto* f = pe::FindFunction(ue::ClassOf(w), fn)) pe::Call(w, f, nullptr); }
    bool Viewport(ue::UObject* context, double size[2], double* dpi) {
        auto* lib = Cdo("WidgetLayoutLibrary");
        auto* fs = lib ? pe::FindFunction(ue::ClassOf(lib), "GetViewportSize") : nullptr;
        auto* fd = lib ? pe::FindFunction(ue::ClassOf(lib), "GetViewportScale") : nullptr;
        if (!fs || !fd) return false;
        Args a(fs);
        std::memcpy(a.At("WorldContextObject"), &context, 8);
        pe::Call(lib, fs, a.buf.data());
        std::memcpy(size, a.At("ReturnValue"), 16);
        Args d(fd);
        std::memcpy(d.At("WorldContextObject"), &context, 8);
        pe::Call(lib, fd, d.buf.data());
        *dpi = ue::Get<float>(d.At("ReturnValue"), 1.f);
        return size[0] > 100 && size[1] > 100 && *dpi > 0.1;
    }
    ue::UObject* NewCard(ue::UObject* context, ue::UObject* actor) {
        auto* lib = Cdo("WidgetBlueprintLibrary");
        auto* create = lib ? pe::FindFunction(ue::ClassOf(lib), "Create") : nullptr;
        auto* init = pe::FindFunction(bossCls_, "Init");
        auto* pc = Pc(context);
        if (!create || !init || !pc) return nullptr;
        Args c(create);
        std::memcpy(c.At("WorldContextObject"), &context, 8);
        std::memcpy(c.At("WidgetType"), &bossCls_, 8);
        std::memcpy(c.At("OwningPlayer"), &pc, 8);
        pe::Call(lib, create, c.buf.data());
        auto* w = ue::Get<ue::UObject*>(c.At("ReturnValue"));
        if (!w || !ue::IsA(w, bossCls_)) return nullptr;
        Opacity(w, 0.f);  // 量好尺寸、放到位置之前不显示
        if (auto* f = pe::FindFunction(ue::ClassOf(w), "SetAnchorsInViewport")) { Args a(f); pe::Call(w, f, a.buf.data()); }  // 锚点 (0,0)-(0,0)
        Widget2(w, "SetAlignmentInViewport", 0, 0);
        Widget2(w, "SetRenderTransformPivot", 0, 0);
        if (auto* f = pe::FindFunction(ue::ClassOf(w), "AddToViewport")) { uint8_t x[16] = {}; const int32_t z = 5; std::memcpy(x, &z, 4); pe::Call(w, f, x); }
        Args i(init);
        std::memcpy(i.At("Character"), &actor, 8);
        pe::Call(w, init, i.buf.data());
        bars_.insert(w);
        return w;
    }
    void DropCard(Card& c) {
        if (c.w && bossCls_ && ue::IsA(c.w, bossCls_)) Call0(c.w, "RemoveFromParent");
        if (c.w) { bars_.erase(c.w); tinted_.erase(c.w); names_.erase(c.w); }
    }
    void ClearBoard() {
        for (auto& [actor, c] : board_) DropCard(c);
        board_.clear();
        topOrder_.clear();
        sideOrder_.clear();
        if (hiddenPoint_) { if (ue::IsA(hiddenPoint_, ue::FindClass("UIExtensionPointWidget"))) Opacity(hiddenPoint_, 1.f); hiddenPoint_ = nullptr; }
    }
    // 游戏自己挂在那一排里的 Boss（关卡 Boss 等）
    std::vector<ue::UObject*> NativeBosses(ue::UObject* box) {
        std::vector<ue::UObject*> out;
        auto* pointCls = ue::FindClass("UIExtensionPointWidget");
        if (!box || !pointCls || !ue::IsA(box, pointCls)) return out;
        if (offMapping_ == -2) offMapping_ = ue::PropOffset(pointCls, "ExtensionMapping");
        Slots s;
        if (offMapping_ < 0 || !ReadSparse(box + offMapping_, s)) return out;
        const int offChar = Off(bossCls_, "Character", offBossChar_);
        for (int i = 0; i < s.count; ++i) {
            if (!Used(s, i)) continue;  // ExtensionMapping：TMap<FUIExtensionHandle(0x18), UUserWidget*>，元素 0x28
            auto* w = ue::Get<ue::UObject*>(s.data + i * 0x28 + 0x18);
            auto* a = w && ue::IsA(w, bossCls_) && offChar >= 0 ? ue::Get<ue::UObject*>(w + offChar) : nullptr;
            if (a && std::find(out.begin(), out.end(), a) == out.end()) out.push_back(a);
        }
        return out;
    }
    void Board(ue::UObject* context, const std::vector<ue::UObject*>& ownedBosses, const std::unordered_map<ue::UObject*, std::string>& factions) {
        if (!bossCls_) return;
        auto* hud = Hud(context);
        if (offPoint_ == -2 && hudCls_) offPoint_ = ue::PropOffset(hudCls_, "BossPoint");
        auto* box = hud && offPoint_ >= 0 ? ue::Get<ue::UObject*>(hud + offPoint_) : nullptr;
        // 要显示的 Boss：顶部 = 生成的非我方 + 游戏自己挂的；右侧 = 生成的我方。顺序按出现先后，新的排在后面
        std::vector<ue::UObject*> top, side;
        // 死了的（血量归零，尸体还在）不占位置
        for (auto* a : ownedBosses) {
            if (Health(a) <= 0) continue;
            auto f = factions.find(a);
            (f != factions.end() && f->second == "Faction.Player" ? side : top).push_back(a);
        }
        for (auto* a : NativeBosses(box))
            if (Health(a) > 0 && std::find(side.begin(), side.end(), a) == side.end() && std::find(top.begin(), top.end(), a) == top.end()) top.push_back(a);
        auto sync = [&](std::vector<ue::UObject*>& order, const std::vector<ue::UObject*>& want) {
            order.erase(std::remove_if(order.begin(), order.end(), [&](auto* a) { return std::find(want.begin(), want.end(), a) == want.end(); }), order.end());
            for (auto* a : want) if (std::find(order.begin(), order.end(), a) == order.end()) order.push_back(a);
        };
        sync(topOrder_, top);
        sync(sideOrder_, side);
        for (auto it = board_.begin(); it != board_.end();) {
            const bool keep = std::find(topOrder_.begin(), topOrder_.end(), it->first) != topOrder_.end() ||
                              std::find(sideOrder_.begin(), sideOrder_.end(), it->first) != sideOrder_.end();
            const bool alive = it->second.w && ue::IsA(it->second.w, bossCls_);
            if (keep && alive) { ++it; continue; }
            DropCard(it->second);
            it = board_.erase(it);
        }
        if (topOrder_.empty() && sideOrder_.empty()) { ClearBoard(); return; }
        if (box && box != hiddenPoint_) { Opacity(box, 0.f); hiddenPoint_ = box; }  // 藏起游戏那一排
        for (auto* list : {&topOrder_, &sideOrder_})
            for (auto* a : *list) {
                auto& c = board_[a];
                if (!c.w) c.w = NewCard(context, a);
                if (c.w && c.natW <= 0) {  // 自然尺寸：加到屏幕后下一轮才有
                    double d[2] = {};
                    if (auto* f = pe::FindFunction(ue::ClassOf(c.w), "GetDesiredSize")) { uint8_t x[32] = {}; pe::Call(c.w, f, x); std::memcpy(d, x, 16); }
                    if (d[0] > 10 && d[1] > 10 && std::isfinite(d[0]) && std::isfinite(d[1])) {
                        c.natW = d[0]; c.natH = d[1];
                        Widget2(c.w, "SetDesiredSizeInViewport", c.natW, c.natH);
                    }
                }
            }
        // 整页轮换
        if (now_ >= flipAt_) {
            flipAt_ = now_ + 2;
            topPage_ = topOrder_.size() > kTopShow ? (topPage_ + kTopShow) % topOrder_.size() : 0;
            sidePage_ = sideOrder_.size() > kSideShow ? (sidePage_ + kSideShow) % sideOrder_.size() : 0;
        }
        if (topPage_ >= std::max<size_t>(1, topOrder_.size())) topPage_ = 0;
        if (sidePage_ >= std::max<size_t>(1, sideOrder_.size())) sidePage_ = 0;
        double vp[2], dpi = 1;
        if (!Viewport(context, vp, &dpi)) return;
        auto put = [&](Card& c, double x, double y, double s, bool show) {
            if (!c.w || c.natW <= 0) return;
            if (show && (x != c.x || y != c.y)) { Widget2(c.w, "SetPositionInViewport", x, y); }  // 像素坐标（bRemoveDPIScale 见下）
            if (show && s != c.s) Widget2(c.w, "SetRenderScale", s, s);
            if (show != c.visible) Opacity(c.w, show ? 1.f : 0.f);
            if (show && !c.visible && !fitted_.count(c.w)) { FitName(c.w); fitted_.insert(c.w); }
            c.x = x; c.y = y; c.s = s; c.visible = show;
        };
        {   // 顶部一行
            const size_t n = topOrder_.size(), shown = std::min(n, kTopShow);
            // 按实际个数分格：1 个用原生大小（缩放上限 1），越多越小，5 个时与原来一致
            const double left = vp[0] * 0.08, right = vp[0] * 0.92, cell = (right - left) / std::max<size_t>(1, shown);
            const double rowLeft = vp[0] / 2 - shown * cell / 2, y = vp[1] * 0.065;
            for (size_t i = 0; i < n; ++i) {
                auto& c = board_[topOrder_[i]];
                const size_t slot = (i + n - topPage_) % n;
                const double w = c.natW * dpi, s = w > 0 ? std::min(1.0, cell * 0.88 / w) : 1;
                put(c, rowLeft + slot * cell + (cell - w * s) / 2, y, s, slot < shown);
            }
        }
        {   // 右侧一列
            const size_t n = sideOrder_.size(), shown = std::min(n, kSideShow);
            for (size_t i = 0; i < n; ++i) {
                auto& c = board_[sideOrder_[i]];
                const size_t slot = (i + n - sidePage_) % n;
                const double w = c.natW * dpi, h = c.natH * dpi;
                const double s = w > 0 ? std::min({0.6, vp[0] * 0.2 / w, vp[1] * 0.62 / (shown * h * 1.08)}) : 0.6;
                put(c, vp[0] * 0.985 - w * s, vp[1] * 0.2 + slot * h * s * 1.08, s, slot < shown);
            }
        }
    }
    void Opacity(ue::UObject* w, float v) {
        auto* f = pe::FindFunction(ue::ClassOf(w), "SetRenderOpacity");
        if (!f) return;
        uint8_t x[16] = {};
        std::memcpy(x, &v, 4);
        pe::Call(w, f, x);
    }
    // 名字比血条宽时（人多被压窄）把名字单独缩到血条宽度内，中心不变
    void FitName(ue::UObject* w) {
        auto* name = ue::Get<ue::UObject*>(w + Off(bossCls_, "BossName", offName_));
        Rect bar, text;
        if (!name || !Measure(w, bar) || !Measure(name, text)) return;
        const double fit = std::min(1.0, bar.localW * 0.96 / text.localW);
        Widget2(name, "SetRenderTransformPivot", 0.5, 0.5);
        Widget2(name, "SetRenderScale", fit, fit);
    }

    // 输入锁保护：人多时玩家只能走、转不了视角、按键无效。怀疑是 Boss 血条初始化带出的过场 / Boss 开场逻辑让控制器忽略输入。
    // 有本工具生成的怪时每 0.5 秒查一次：视角或移动输入被忽略超过 2 秒，记日志（含玩家当前状态）并复位输入忽略计数。
    void InputGuard(ue::UObject* context) {
        auto* gs = Cdo("GameplayStatics");
        auto* getPc = gs ? pe::FindFunction(ue::ClassOf(gs), "GetPlayerController") : nullptr;
        if (!getPc) return;
        Args g(getPc);
        std::memcpy(g.At("WorldContextObject"), &context, 8);
        pe::Call(gs, getPc, g.buf.data());
        auto* pc = ue::Get<ue::UObject*>(g.At("ReturnValue"));
        if (!pc) return;
        auto flag = [&](const char* fn) {
            auto* f = pe::FindFunction(ue::ClassOf(pc), fn);
            if (!f) return false;
            uint8_t x[16] = {};
            pe::Call(pc, f, x);
            return x[0] != 0;
        };
        const bool look = flag("IsLookInputIgnored"), move = flag("IsMoveInputIgnored");
        if (!look && !move) { lockedSince_ = 0; return; }
        if (!lockedSince_) { lockedSince_ = now_; return; }
        if (now_ - lockedSince_ < 2) return;
        lockedSince_ = 0;
        log::Info("玩家输入被锁（视角 {} 移动 {}），场上生成怪 {} 只：复位输入", look ? "锁" : "正常", move ? "锁" : "正常", enemies::OwnedActors().size());
        if (auto* f = pe::FindFunction(ue::ClassOf(pc), "ResetIgnoreInputFlags")) pe::Call(pc, f, nullptr);
    }

    // 角色当前血量（BasicAttributeSet.Health.CurrentValue）；读不到返回 1（当活着）
    // 看板血条的缩放每帧核对一次：血条蓝图自己（受击动画等）会把整条的渲染缩放改回 1，导致某一条突然变大
    void KeepCardScale() {
        if (board_.empty() || !bossCls_) return;
        if (offXform_ == -2) offXform_ = ue::PropOffset(ue::FindClass("Widget"), "RenderTransform");
        if (offXform_ < 0) return;
        for (auto& [actor, c] : board_) {
            if (!c.w || !c.visible || c.s <= 0) continue;
            const double sx = ue::Get<double>(c.w + offXform_ + 16, c.s);  // FWidgetTransform：Translation(16) 之后是 Scale
            if (std::abs(sx - c.s) > 1e-3) Widget2(c.w, "SetRenderScale", c.s, c.s);
        }
    }
    float Health(ue::UObject* actor) {
        if (!charCls_) charCls_ = ue::FindClass("PBZCharacter");
        if (!charCls_ || !ue::IsA(actor, charCls_)) return 1;
        if (offSet_ == -2) offSet_ = ue::PropOffset(charCls_, "AttributeSetBase");
        if (offHealth_ == -2) offHealth_ = attr::AttrOffset("Health");
        auto* set = offSet_ >= 0 ? ue::Get<uint8_t*>(actor + offSet_) : nullptr;
        if (!set || offHealth_ < 0) return 1;
        attr::Target t{actor, set, attr::Who::Enemies, ue::ClassOf(actor)};
        auto* hp = attr::AttrCurrent(t, offHealth_);
        return hp ? ue::Get<float>(hp, 1.f) : 1;
    }
    void Tint(ue::UObject* w, bool boss, const std::string& faction) {
        const Rgba c = ColorOf(faction);
        auto* bar = ue::Get<ue::UObject*>(w + Off(boss ? bossCls_ : enemyCls_, "hpbar", boss ? offBossBar_ : offEnemyBar_));
        if (bar && barCls_ && ue::IsA(bar, barCls_)) {
            // 我方：先切到游戏自带的友方血条材质（只做一次），再统一着色
            if (faction == "Faction.Player" && !tinted_.count(w))
                if (auto* npc = pe::FindFunction(barCls_, "InitNPCHelathBar")) { uint8_t a[16] = {1}; pe::Call(bar, npc, a); }
            CallColor(ue::Get<ue::UObject*>(bar + Off(barCls_, "RealBar", offReal_)), fnFill_, c, false);
            CallColor(ue::Get<ue::UObject*>(bar + Off(barCls_, "ImageBar", offImage_)), fnImg_, c, false);
        }
        if (boss) {
            auto* name = ue::Get<ue::UObject*>(w + Off(bossCls_, "BossName", offName_));
            CallColor(name, fnText_, c, true);
            Label(w, name, faction);
        }
        tinted_.insert(w);
    }

    ue::UObject *bossCls_ = nullptr, *enemyCls_ = nullptr, *barCls_ = nullptr, *fnFill_ = nullptr, *fnImg_ = nullptr, *fnText_ = nullptr;
    int offBossChar_ = -2, offEnemyActor_ = -2, offBossBar_ = -2, offEnemyBar_ = -2, offReal_ = -2, offImage_ = -2, offName_ = -2;
    std::unordered_set<ue::UObject*> bars_, tinted_;
    ue::UObject *targetingCls_ = nullptr, *targeting_ = nullptr;
    int offMap_ = -2, offWant_ = -2, offHandles_ = -2;
    ue::UObject *hudCls_ = nullptr, *hudCdo_ = nullptr;
    std::unordered_map<ue::UObject*, double> shown_;
    double now_ = 0;
    struct Name { ue::UObject* text; std::wstring original; double at; };
    std::unordered_map<ue::UObject*, Name> names_;
    std::unordered_map<ue::UObject*, Card> board_;
    std::vector<ue::UObject*> topOrder_, sideOrder_;
    ue::UObject* hiddenPoint_ = nullptr;
    size_t topPage_ = 0, sidePage_ = 0;
    double flipAt_ = 0;
    double lockedSince_ = 0;  // 输入锁保护：视角 / 移动输入被忽略的起始时间
    std::unordered_set<ue::UObject*> fitted_;
    std::unordered_map<std::string, ue::UObject*> cdo_;
    int offPoint_ = -2, offMapping_ = -2, offSet_ = -2, offHealth_ = -2, offXform_ = -2;
    ue::UObject* charCls_ = nullptr;
    std::vector<std::pair<uint8_t*, uint64_t>> removed_;
    bool logged_ = false;
    int cursor_ = 0;
    float since_ = 1;
};

}  // namespace

void RegisterFactionBarFeatures() { Features().push_back(std::make_unique<FactionBars>()); }

}  // namespace pbz
