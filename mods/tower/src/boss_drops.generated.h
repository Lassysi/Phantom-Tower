// 生成文件：mods/tower/tools/gen_boss_drops.py ← mods/tower/data/boss_item_drops.json，勿手改
// Boss 死亡 Flow 里静态可达的原生道具发放（记忆之花 / 碎片等永久属性道具不发：塔自己管成长）
#pragma once
#include <string_view>
namespace tower {
struct BossItemDrop { std::string_view stem, item; int count; std::string_view flow; };
inline constexpr BossItemDrop kBossItemDrops[] = {
    {"BP_Monster_Danghui", "I_05001", 1, "BPF_DangHuiDeath"},
    {"BP_Monster_GuWanYue", "TU_05001", 1, "BPF_GuWanYueChuChang"},
    {"BP_Monster_HuaShan_Blue", "K_03004", 1, "BPF_JianZhenDeath"},
    {"BP_Monster_HuaShan_JianOu", "K_03004", 1, "BPF_JianZhenDeath"},
    {"BP_Monster_HuoDaoDuiZhang", "TU_150012", 1, "BPF_HuoDaoDuiZhuang_Death"},
    {"BP_Monster_LionDancer", "I_06004", 1, "BPF_WuShiZunZhu_Death"},
    {"BP_Monster_LionDancer_P2", "I_06004", 1, "BPF_WuShiZunZhu_Death"},
    {"BP_Monster_MengHua", "TU_14001", 1, "BPF_MengHua_Death"},
    {"BP_Monster_MengHua", "TU_18011", 1, "BPF_MengHua_Death"},
    {"BP_Monster_MuXiaoKui", "K_05002", 1, "BPF_MuXiaoKuiDeath"},
    {"BP_Monster_MuXiaoKui", "TU_02002", 1, "BPF_MuXiaoKuiDeath"},
    {"BP_Monster_MuXiaoKui_P2", "K_05002", 1, "BPF_MuXiaoKuiDeath"},
    {"BP_Monster_MuXiaoKui_P2", "TU_02002", 1, "BPF_MuXiaoKuiDeath"},
    {"BP_Monster_PengMeng_Axe", "R_01002", 1, "BPF_FuLaoDaDeath"},
    {"BP_Monster_QiShu_Bing", "K_16001", 1, "BPF_DHS_SuiHanDeath"},
    {"BP_Monster_QiShu_Du", "K_08001", 1, "BPF_MGH_GuheDeath"},
    {"BP_Monster_QiShu_Huo", "K_05003", 1, "BPF_LJC_YanRuDeath"},
    {"BP_Monster_QiShu_Lei", "K_12001", 1, "BPF_YDZZ_LeiTingDeath"},
    {"BP_Monster_XiaoXue", "R_03007", 1, "BPF_XiaoXueDeath"},
    {"BP_Monster_YanQianSui", "I_08002", 1, "BPF_YanQianSuiDeath"},
    {"BP_Monster_YanQianSui", "TU_07002", 1, "BPF_YanQianSuiDeath"},
    {"BP_Monster_YouLong_Harpoon", "TU_10003", 1, "BPF_YouLong_Death"},
    {"BP_Monster_YuCong", "TU_150022", 1, "BPF_SanTouMoOuDeath"},
    {"BP_Monster_YuCong_Bing", "TU_150022", 1, "BPF_SanTouMoOuDeath"},
    {"BP_Monster_YuCong_HunDun", "TU_150022", 1, "BPF_SanTouMoOuDeath"},
    {"BP_Monster_YuCong_WuLi", "TU_150022", 1, "BPF_SanTouMoOuDeath"},
    {"BP_Monster_ZuoShang_P2", "TU_18008", 1, "TowerLink_ZiMingJian"},
};
}  // namespace tower
