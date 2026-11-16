# 影塔场地静态提取

证据等级：静态；运行时均未验证。本步不启动游戏、不连接管道、不修改 Core、引擎、UI 或 shared。

按用户最新方案：仅借用场地与雾门，Boss 由塔生成，原生遭遇不启动。商店／道具链验收暂缓。

## 统计口径

每条对应 32ffb7d 覆盖表中的一个精确类及其当时选中的出生点。备选出生点只保留引用，不自动替换。

| 项目 | 数量 |
|---|---:|
| 精确 Boss 类 | 77 |
| Boss 身份 | 57 |
| 找到明确激活雾门关联的类 | 72 |
| 未找到激活门关联（待核）的类 | 5 |
| 已证明原生没有门的类 | 0 |
| 可计算单元内边界的类 | 59 |
| 组指针与成员表冲突的类 | 3 |
| 出生点世界变换链静态闭合的类 | 19 |
| 选中组内多 Boss 身份组（按目录去重） | 3 |
| 选中组内多 Boss 精确类组（含同身份阶段） | 8 |
| 全部已查世界中的多 Boss 共图世界（含备选图） | 10 |
| 77 条选中场地中的多 Boss 共图世界 | 8 |
| 静态世界坐标证明 150m 内有其他 Boss 身份的选中点 | 5 |

**“无关联”不等于“确认没有门”。** 单个生成单元、未导出的邻近单元、未解析的默认脚本或动态 Flow 仍可能补出门。

**坐标分层。** 主 WP 持久关卡的未附着根可闭合到静态世界坐标；生成单元的组件链可得 level 坐标，但缺少独立的 LevelTransform/LevelInstance 证明时世界坐标留空。不将旧相对坐标重新命名为世界坐标。19 条出生点的静态世界链已闭合，其余 58 条待核；这不包含实机坐标验证。

**凸包。** 按游戏 JS 的 monotone chain（cross <= 0 去共线）、面积门槛 10000 uu²，再按边法线内缩 60 uu；保留原始多边形。Z 范围取激活门的控制点，不代表已经取得地面高度。

**成员。** 同组包含多个 Boss 类可能是换体／阶段；只有多个目录 identity 才另计为多 Boss 身份组，仍不证明同时开打。跨组串场不能据此改成双 Boss。统计使用目录中的 Boss 身份，组内其他成员（例如金面、铁面）仍完整列出，不擅自提升为池内 Boss。

## 关键结论

- **混沌与白赤练不同组。** YuCong_P3_1 / 2 / 3 是 spawner ID，分别刷冰、混沌、武力；三者同属 `.1509`，成员没有 ChiLian。ChiLian_Seq034 与 ChiLianP2_Seq035 分别属于主图 `.1375`、`.1376`，与混沌不是同组。它们的雾门位置十分接近，但坐标近不构成成员关系；不能把串场默认解释为混沌的原生增援。
- **玉鼎与雷当不同组。** Ximeng 的玉鼎 `.1522`，雷当本体和 P2 同属 `.1517`；两组使用不同雾门。此证据不支持“玉鼎＋雷当同组双 Boss”，也不证明完整剧情的车轮战顺序。
- **彭猛确实有关联门。** PengMeng_Seq014 的 `.1690` 组通过 LinkFogDoor 明确引用同单元的 `.775`、`.776` 两扇门；之前实机没有关门，不能归因于原生没门。
- **玉面郎君属于原生多人组。** JadeIntro_Jade 的 `.1297` 组还包含 GoldMaskMan、IronMaskMan。门 `.593` 为闭合样条。该单元中出生点与门控制点 Z 范围相差约 -19.5～45.0 uu，静态上没有明显层高错位；世界变换、地面和玩家落点仍待核，不能因此认定落点已可用。
- **骨婉约按脚本分别算。** 一个组挂两份 LinkFogDoor，不能合并为单一原生凸包。小雪、烟千岁、梦华也已保留全部成员、组脚本、激活／解除门和邻近风险引用。
- **未找到激活门的 5 类**：Danghui、EMei_Sword、MobMan_Elite、ZuoShang、MBoss。只表示本次选中摆放没有解析到关联，不是“全游戏确认无门”。

- **三条组指针／成员表需要进一步核对**：PowerMan_P2 的 spawner.MobGroup 指向 Duanhun `.6436`，反向 Members 在 `.6437`；ZuoShang_P2 分别为 `.6438`／`.6439`；MuXiaoKui_P2 也出现在指针组以外的另一份 Members 中。相关完整组均保留，主 arena_hull 留 null，不猜应采用哪组。这可能是原生阶段换组或多组复用，不直接当成资源错误。

## 重点对象

| identity / 精确类 | 世界 | MobGroup source_ref | 激活门 | 凸包 | 备注 |
|---|---|---|---:|---|---|
| YanQianSui / BP_Monster_YanQianSui_C | PBZ_Pangzhen_WP | /Game/Maps/B08_09_10/PBZ_Pangzhen_WP.1653 | 1 | 静态 |  |
| YuCong / BP_Monster_YuCong_Bing_C | PBZ_Fujizhilin_WP | /Game/Maps/B03/PBZ_Fujizhilin_WP/_Generated_/DWMXU3XTXP9LU0AAT34MDS9X3.1509 | 1 | level 已算 / world 待核 | Phase/body/encounter variant name; native phase handoff dependencies 待核, never infer phase ordering from a suffix；WP cell LevelTransform omitted; native identity default and absence of a parent LevelInstance are not independently proven by this JSON export |
| YuCong / BP_Monster_YuCong_HunDun_C | PBZ_Fujizhilin_WP | /Game/Maps/B03/PBZ_Fujizhilin_WP/_Generated_/DWMXU3XTXP9LU0AAT34MDS9X3.1509 | 1 | level 已算 / world 待核 | Phase/body/encounter variant name; native phase handoff dependencies 待核, never infer phase ordering from a suffix；WP cell LevelTransform omitted; native identity default and absence of a parent LevelInstance are not independently proven by this JSON export |
| YuCong / BP_Monster_YuCong_WuLi_C | PBZ_Fujizhilin_WP | /Game/Maps/B03/PBZ_Fujizhilin_WP/_Generated_/DWMXU3XTXP9LU0AAT34MDS9X3.1509 | 1 | level 已算 / world 待核 | Phase/body/encounter variant name; native phase handoff dependencies 待核, never infer phase ordering from a suffix；WP cell LevelTransform omitted; native identity default and absence of a parent LevelInstance are not independently proven by this JSON export |
| XiaoXue / BP_Monster_XiaoXue_C | PBZ_Zhengdaodaying_WP | /Game/Maps/B04_05/PBZ_Zhengdaodaying_WP.1813 | 1 | 静态 |  |
| JadeMaskMan / BP_Monster_JadeMaskMan_C | PBZ_DengDingShanDao_WP | /Game/Maps/DengDingShanDao/PBZ_DengDingShanDao_WP/_Generated_/5DGL69S3OIGGAOMB80LU2RLYC.1297 | 1 | level 已算 / world 待核 | WP cell LevelTransform omitted; native identity default and absence of a parent LevelInstance are not independently proven by this JSON export |
| GuWanYue / BP_Monster_GuWanYue_C | PBZ_Houjie_WP | /Game/Maps/B09/PBZ_Houjie_WP/_Generated_/64Q0WR55YIK5YSQBU92NEAOS4.918 | 2 | level 已算 / world 待核 | WP cell LevelTransform omitted; native identity default and absence of a parent LevelInstance are not independently proven by this JSON export |
| LeiDang / BP_Monster_LeiDang_P2_C | PBZ_Ximeng_WP | /Game/Maps/B08_09_12_13/PBZ_Ximeng_WP.1517 | 1 | 静态 | Phase/body/encounter variant name; native phase handoff dependencies 待核, never infer phase ordering from a suffix |
| ChiLian / BP_Monster_ChiLian_C | PBZ_Fujizhilin_WP | /Game/Maps/B03/PBZ_Fujizhilin_WP.1375 | 1 | 静态 | Phase/body/encounter variant name; native phase handoff dependencies 待核, never infer phase ordering from a suffix；WP cell LevelTransform omitted; native identity default and absence of a parent LevelInstance are not independently proven by this JSON export |
| LeiDang / BP_Monster_LeiDang_C | PBZ_Ximeng_WP | /Game/Maps/B08_09_12_13/PBZ_Ximeng_WP.1517 | 1 | 静态 |  |
| ChiLian / BP_Monster_ChiLian_P2_C | PBZ_Fujizhilin_WP | /Game/Maps/B03/PBZ_Fujizhilin_WP.1376 | 1 | 静态 | Phase/body/encounter variant name; native phase handoff dependencies 待核, never infer phase ordering from a suffix；WP cell LevelTransform omitted; native identity default and absence of a parent LevelInstance are not independently proven by this JSON export |
| MengHua / BP_Monster_MengHua_C | PBZ_Duanhun_WP | /Game/Maps/B08_09_12_13/PBZ_Duanhun_WP.6435 | 2 | 静态 |  |
| YuCong / BP_Monster_YuCong_C | PBZ_Fujizhilin_WP | /Game/Maps/B03/PBZ_Fujizhilin_WP/_Generated_/DWMXU3XTXP9LU0AAT34MDS9X3.1508 | 1 | level 已算 / world 待核 | WP cell LevelTransform omitted; native identity default and absence of a parent LevelInstance are not independently proven by this JSON export |
| YuCong / BP_Monster_YuCong_FirstTime_C | PBZ_Xigu_WP | /Game/Maps/B02/PBZ_Xigu_WP/_Generated_/205I5TU4N4V6Q5ZKXD7E6662B.125 | 2 | level 已算 / world 待核 | Phase/body/encounter variant name; native phase handoff dependencies 待核, never infer phase ordering from a suffix；WP cell LevelTransform omitted; native identity default and absence of a parent LevelInstance are not independently proven by this JSON export |
| FemaleVessel_YuDing / BP_Monster_FemaleVessel_YuDing_C | PBZ_Ximeng_WP | /Game/Maps/B08_09_12_13/PBZ_Ximeng_WP.1522 | 1 | 静态 |  |
| PengMeng_Axe / BP_Monster_PengMeng_Axe_C | PBZ_Xigu_WP | /Game/Maps/B02/PBZ_Xigu_WP/_Generated_/82OBM6ICJZS18QDOP03KXFZHY.1690 | 2 | level 已算 / world 待核 | WP cell LevelTransform omitted; native identity default and absence of a parent LevelInstance are not independently proven by this JSON export |

## 重建与检查

```powershell
python -I mods/tower/tools/build_venues.py
python -I mods/tower/tools/build_venues.py --check
```

`--dump <root>` 可重复，改用迁移后的完整 CUE4Parse 导出目录；按包路径去重，相同包不同 SHA256 直接失败。--check 重建后比较 JSON 与本摘要，失败不写文件。

输入包、文件 SHA256、export index 引用、脚本来源均在 venues.json。story_risk.nearby 仅含静态世界坐标可比较且在 150m 内的对象；same_level_radius_candidates 是同单元坐标中的候选，世界距离仍待核；distance_pending 引用顶层 story_risk_objects，不把它们算成“已在 150m 内”。

## 补充导出的复现

现有 30 个 bossmap 包之外，本轮补出两个主 WP 包和 29 个组脚本蓝图，均来自本机游戏归档。读取现有本机密钥及 usmap，不输出密钥，不启动游戏。原始导出留在 build 工作目录；它们是重建输入，不是发布内容。

```powershell
& E:/PBZ-claude/tools/extract/bin/Release/net10.0/PBZExtract.exe grep E:/Phantom_Blade_Zero/build/tower-venues/maps '^ProjectPBZ/Content/Maps/(B01/PBZ_WP_ZhuLinGuWu|DengDingShanDao/PBZ_DengDingShanDao_WP)\.umap$' '.'
& E:/PBZ-claude/tools/extract/bin/Release/net10.0/PBZExtract.exe export E:/Phantom_Blade_Zero/build/tower-venues/defaults '/(MobGroupScript_[^/]+|BossCombat_[^/]+|GroupScript_[^/]+)\.uasset$'
```

本机旧的主工作区 PBZExtract.exe 仍不支持 grep；上面的路径固定到 Claude 已有的 grep 版本，未重建或改动提取器。

后续必须补核邻近未导出单元、实例变换、有效地面／玩家落点、门能否单独开关，以及场地依赖和阶段换体。当前不得直接将全表作为运行时白名单。

## 离线校验

`python -I mods/tower/tools/test_venues.py`：包含对游戏原始 JS 的独立 Node 执行对照、负坐标和旋转／非均匀缩放、附着父级、缺失单元变换不冒充世界坐标、DefaultGroup／反向成员和重复组件名、PlayerDead 字段及退化凸包检查。此类检查不提升为实机证据。
