// 无尽层场地表与选场规则。条目是 Boss 原生场地的类路径（场地键），或固定场地键 fixed:。
// 3.6.0（用户 2026-11-14，方案第 2 / 3 条）：
//   - 无尽层只要真正站得住的超大圆形 / 方形场地（真实可站半径 ≥ 30 米、形状比 ≥ 0.7），越大越好，宁少勿小；
//     名单由测试包 qa_measure 实测后定（notes/venue-measure-20261114.jsonl）。实测前先用先验候选（塔画的圈 45 米）。
//   - 人面榕树（榕树之心）约占 30% 的无尽层，其余按真实可站面积加权轮换，不连续两层同一场地。
//   - 小场地名单（花千笑、囚锋、全部鬼差类、实测 < 20 米的）：Boss 层照旧原生场地，后面那段小怪层改去榕树之心。
#pragma once
#include <algorithm>
#include <cstdint>
#include <string_view>
#include <vector>
namespace tower::endless {
inline constexpr std::string_view kRongshu = "fixed:rongshu_zdy";
struct Venue { std::string_view key; double area; };  // area：真实可站面积（平方米）；实测前为先验估计
// 3.6.0 先验候选（待 qa_measure 实测后按结果改写；没过线的删掉，至少留最大的一个）
// BEGIN GENERATED VENUES（tools/gen_venue_real.py：实测 10% 分位半径 ≥ 30 米、不在小场地名单；用户 2026-11-15 定放宽到 ≥ 30 米，不看形状比）
inline constexpr Venue kVenues[] = {
    {kRongshu, 2252},  // 人面榕树（正道大营）：固定约 30%
    {"/PBZCombatAsset/ActorData/Mobs/035_HuaShan/BP_Monster_HuaShan_RedTroop_BigBro.BP_Monster_HuaShan_RedTroop_BigBro_C", 13744},  // 悲痛欲绝的阿瑾：10% 分位 57.0 米、中位 70.0
    {"/PBZCombatAsset/ActorData/Mobs/YuCong/BP_Monster_YuCong_Bing.BP_Monster_YuCong_Bing_C", 11847},  // “玉琮”：寒冰：10% 分位 42.0 米、中位 70.0
    {"/PBZCombatAsset/ActorData/Mobs/QiShu/BP_Monster_QiShu_Bing.BP_Monster_QiShu_Bing_C", 8090},  // 灭法之法：寒岁：10% 分位 36.0 米、中位 46.0
    {"/PBZCombatAsset/ActorData/Mobs/073_YanQianSui/BP_Monster_YanQianSui.BP_Monster_YanQianSui_C", 8048},  // 琴女烟千岁：10% 分位 35.0 米、中位 51.0
    {"/PBZCombatAsset/ActorData/Mobs/077_LengTu/BP_Monster_LengTu.BP_Monster_LengTu_C", 8039},  // 梦差：冷荼：10% 分位 32.0 米、中位 43.0
    {"/Game/RenewalContent/Actors/Mobs/Normal/BP_Monster_PowerMan.BP_Monster_PowerMan_C", 5576},  // 力者和锯者：10% 分位 30.5 米、中位 35.0
    {"/PBZCombatAsset/ActorData/Mobs/YuBi/BP_Monster_YuBi.BP_Monster_YuBi_C", 4448},  // 廿一号·剑偶：“玉璧”：10% 分位 33.0 米、中位 36.6
    {"/PBZCombatAsset/ActorData/Mobs/008_GreatBladeMan/BP_Monster_HuoDaoDuiZhang.BP_Monster_HuoDaoDuiZhang_C", 4363},  // 火刀：祝原：10% 分位 30.1 米、中位 35.1
    {"/PBZCombatAsset/ActorData/Mobs/QiShu/BP_Monster_QiShu_Du.BP_Monster_QiShu_Du_C", 4345},  // 灭法之法：蛊鹤：10% 分位 34.4 米、中位 37.0
};
// END GENERATED VENUES
inline constexpr double kRongshuShare = 0.30;

// 小场地：后面那段小怪层去榕树之心。kMeasuredSmall = 实测真实可站半径 < 20 米的场地（量完补进来）
// BEGIN GENERATED SMALL（实测 10% 分位半径 < 20 米，用户 2026-11-15 定）
inline constexpr std::string_view kMeasuredSmall[] = {
    "/Game/RenewalContent/Actors/Mobs/Normal/BP_Monster_Yuu.BP_Monster_Yuu_C",  // 少主护卫：玉妖：10% 分位 15.0 米
    "/Game/RenewalContent/Actors/Mobs/Normal/BP_Monster_MobMan_Elite_Sword.BP_Monster_MobMan_Elite_Sword_C",  // 痴狂暴徒：残钢：10% 分位 10.0 米
    "/PBZCombatAsset/ActorData/Mobs/034_EMei/BP_Monster_EMei_Sword_JianZhen.BP_Monster_EMei_Sword_JianZhen_C",  // 红衣女剑客：10% 分位 17.0 米
    "/PBZCombatAsset/ActorData/Mobs/Luanshi/BP_Monster_LuanShi.BP_Monster_LuanShi_C",  // 拳痴：铁狂：10% 分位 16.0 米
    "/PBZCombatAsset/ActorData/Mobs/070_GuiChai_Cyan/BP_Monster_GuiChai_Cyan_ShuangYue.BP_Monster_GuiChai_Cyan_ShuangYue_C",  // 鬼差高手：断肠：10% 分位 14.0 米
    "/PBZCombatAsset/ActorData/Mobs/060_GuiChai_Yellow/BP_Monster_GuiChai_Yellow_ShuangGou.BP_Monster_GuiChai_Yellow_ShuangGou_C",  // 鬼差高手：勾魂：10% 分位 15.0 米
    "/PBZCombatAsset/ActorData/Mobs/FengXueDaoKe/BP_Monster_FengXueDaoKe.BP_Monster_FengXueDaoKe_C",  // 风雪刀客：10% 分位 18.5 米
    "/PBZCombatAsset/ActorData/Mobs/CanQu_BiHai/BP_Monster_CanQu_BiHai.BP_Monster_CanQu_BiHai_C",  // 苦叶寺：碧海大师残躯：10% 分位 8.5 米
    "/Game/RenewalContent/Actors/Mobs/Normal/BP_Monster_Zhuobi.BP_Monster_Zhuobi_C",  // 守门人拙壁：10% 分位 13.0 米
    "/PBZCombatAsset/ActorData/Mobs/068_YuZan/BP_Monster_FemaleVessel_YuZan.BP_Monster_FemaleVessel_YuZan_C",  // 三号·乐师偶：“玉瓒”：10% 分位 15.8 米
    "/PBZCombatAsset/ActorData/Mobs/027_XiaoXue/BP_Monster_XiaoXue.BP_Monster_XiaoXue_C",  // 吞噬尸体的小雪：10% 分位 2.0 米
    "/PBZCombatAsset/ActorData/Mobs/DangHui/BP_Monster_Danghui.BP_Monster_Danghui_C",  // 操偶师：荡秽：10% 分位 11.0 米
    "/PBZCombatAsset/ActorData/Mobs/042_AHua/BP_Monster_AHua_GuiTouDao.BP_Monster_AHua_GuiTouDao_C",  // 新娘阿花：10% 分位 15.4 米
    "/PBZCombatAsset/ActorData/Mobs/045_JingYe/BP_Monster_JingYe.BP_Monster_JingYe_C",  // 亡命徒：荆野：10% 分位 10.6 米
    "/PBZCombatAsset/ActorData/Mobs/YuDing_00/BP_Monster_FemaleVessel_YuDing.BP_Monster_FemaleVessel_YuDing_C",  // 十二号·偃月刀偶：“玉鼎”：10% 分位 18.0 米
    "/PBZCombatAsset/ActorData/Mobs/059_DuYan/BP_Monster_DuYanBoss.BP_Monster_DuYanBoss_C",  // 独眼弓客：10% 分位 14.4 米
    "/PBZCombatAsset/ActorData/Mobs/008_GreatBladeMan/BP_Monster_GreatBladeMan.BP_Monster_GreatBladeMan_C",  // 横刀：毕峰：10% 分位 1.8 米
    "/Game/RenewalContent/Actors/Mobs/Normal/BP_Monster_MBoss.BP_Monster_MBoss_C",  // 昆仑叛教者：荒行子：10% 分位 4.2 米
    "/PBZCombatAsset/ActorData/Mobs/057_WanJun/BP_Monster_WanJun_JinGua.BP_Monster_WanJun_JinGua_C",  // 巨锤客：万钧：10% 分位 15.0 米
    "/PBZCombatAsset/ActorData/Mobs/YingMei/BP_Monster_YingMei_RedShadowBoneBlade.BP_Monster_YingMei_RedShadowBoneBlade_C",  // 赤影骨刃：囚锋：10% 分位 7.2 米
    "/PBZCombatAsset/ActorData/Mobs/058_MadDoctor/BP_Monster_Maddoctor_Elite.BP_Monster_Maddoctor_Elite_C",  // 神医：谢秋冬：10% 分位 12.2 米
    "/PBZCombatAsset/ActorData/Mobs/072_CanQu_BiYuan/BP_Monster_BiYuan.BP_Monster_BiYuan_C",  // 天玄派：毕远掌门残躯：10% 分位 11.4 米
    "/PBZCombatAsset/ActorData/Mobs/QiShu/BP_Monster_QiShu_Lei.BP_Monster_QiShu_Lei_C",  // 灭法之法：雷婷：10% 分位 14.3 米
    "/Game/RenewalContent/Actors/Mobs/Normal/BP_Monster_JadeMaskMan.BP_Monster_JadeMaskMan_C",  // 玉面郎君：10% 分位 13.8 米
    "/PBZCombatAsset/ActorData/Mobs/039_GuiChai_White/BP_Monster_GuiChai_White_PangZhenBoss.BP_Monster_GuiChai_White_PangZhenBoss_C",  // 白衣鬼差（变体）：10% 分位 8.2 米
    "/PBZCombatAsset/ActorData/Mobs/067_MoTian/BP_Monster_MoTian_LongZhu_01.BP_Monster_MoTian_LongZhu_01_C",  // 金面具黑衣人：10% 分位 6.6 米
    "/PBZCombatAsset/ActorData/Mobs/HuaQianXiao/BP_Monster_HuaQianXIao.BP_Monster_HuaQianXIao_C",  // 剑痴：花千笑：10% 分位 7.1 米
    "/Game/RenewalContent/Actors/Mobs/Normal/BP_Monster_MobMan_Elite.BP_Monster_MobMan_Elite_C",  // 癫狂暴徒：铁杀：10% 分位 5.7 米
    "/PBZCombatAsset/ActorData/Mobs/060_GuiChai_Yellow/BP_Monster_GuiChai_Yellow_MoDao.BP_Monster_GuiChai_Yellow_MoDao_C",  // 鬼差高手：斩骑：10% 分位 10.6 米
    "/Game/RenewalContent/Actors/Mobs/Normal/BP_Monster_IronMaskMan_Boss.BP_Monster_IronMaskMan_Boss_C",  // 铁面杀手：10% 分位 4.7 米
    "/PBZCombatAsset/ActorData/Mobs/028_YanYan/BP_Monster_YanYan_Club.BP_Monster_YanYan_Club_C",  // 疯婆子奎艳艳：10% 分位 1.2 米
    "/PBZCombatAsset/ActorData/Mobs/034_EMei/BP_Monster_EMei_Sword_Top.BP_Monster_EMei_Sword_Top_C",  // 红衣女剑客：10% 分位 0.0 米
};
// END GENERATED SMALL
// 3.6.0 打包前（用户 2026-11-15：前 100 层刷怪出问题的只有断肠 / 囚锋，阿花、花千笑等照旧原场地）：
// 小怪层改去榕树之心只认这两处；kMeasuredSmall 只留作实测记录。无尽层另由 kVenues（实测 ≥ 30 米）决定，小场地本来就不在里面
inline bool SmallVenue(std::string_view path) {
    if (path.empty()) return false;
    if (path.find("BP_Monster_GuiChai_Cyan_ShuangYue") != std::string_view::npos) return true;    // 鬼差高手：断肠（两个版本）
    if (path.find("BP_Monster_YingMei_RedShadowBoneBlade") != std::string_view::npos) return true;  // 赤影骨刃：囚锋（小灵堂）
    return false;
}
inline bool MeasuredSmall(std::string_view path) {
    return std::any_of(std::begin(kMeasuredSmall), std::end(kMeasuredSmall), [&](std::string_view s) { return !s.empty() && s == path; });
}

// 选场（纯函数，离线测试）：cands = 本层可用的场地（kVenues 的子集，顺序不限）；last = 上一层的场地键；x = 本层随机数
// 返回 cands 下标；cands 为空返回 -1
inline int Pick(const std::vector<Venue>& cands, std::string_view last, uint64_t x) {
    if (cands.empty()) return -1;
    if (cands.size() == 1) return 0;
    int rong = -1;
    for (size_t i = 0; i < cands.size(); ++i) if (cands[i].key == kRongshu) rong = static_cast<int>(i);
    const uint64_t roll = x % 1000;
    x /= 1000;
    // 上一层是榕树时这一层不能再选它，所以「不是榕树之后」选中的概率取 30% ÷ 70%，长期正好约 30%
    if (rong >= 0 && last != kRongshu && roll < static_cast<uint64_t>(kRongshuShare / (1.0 - kRongshuShare) * 1000)) return rong;
    double total = 0;
    for (size_t i = 0; i < cands.size(); ++i) if (static_cast<int>(i) != rong && cands[i].key != last) total += std::max(1.0, cands[i].area);
    if (total <= 0) {  // 只剩榕树和上一层的场地：榕树优先，否则只能重复
        if (rong >= 0 && last != kRongshu) return rong;
        for (size_t i = 0; i < cands.size(); ++i) if (cands[i].key != last) return static_cast<int>(i);
        return 0;
    }
    double t = static_cast<double>(x % 1000003) / 1000003.0 * total;
    int lastOk = -1;
    for (size_t i = 0; i < cands.size(); ++i) {
        if (static_cast<int>(i) == rong || cands[i].key == last) continue;
        lastOk = static_cast<int>(i);
        if ((t -= std::max(1.0, cands[i].area)) < 0) return lastOk;
    }
    return lastOk;
}
}  // namespace tower::endless
