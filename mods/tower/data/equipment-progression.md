# 装备进度表使用说明（2026-11-01）

文件：`equipment-progression.json`；逐项查看 `equipment-progression-review.md`。重建：`python -I mods/tower/tools/build_equipment_progression.py`。本表是下一批数据依赖，不进入测试中的6ede7eb运行库。

**覆盖150项**：主兵器33、影之武装27、饰品73、原生药17；包含剧情专用和未命名目录项，明确排除而不漏列。92项查到原生掉落/商店静态引用，77项能从引用的地图记录定位章节；这不等于已经证明它在主线的首次获取路径。

## 字段与准入

| 字段 | 含义 |
|---|---|
| id / name_zh / kind | 与shared/items.json逐项对应；FName ID大小写按同一身份对照 |
| catalog_sources | 目录/原生定义位置；不能当作自然获得证据 |
| native_acquisition_sources | 原生掉落行、原生店铺货号/库存的文件与JSON指针、概率及可确定的地图上下文；Test行标记排除 |
| native_first_obtain_status | 本次导出不能穷尽正式获取路线的，明确标“待核”；不会因在数据表里找到物品就宣称是前期自然奖励 |
| earliest_located_native_chapter_order | 当前找到的静态来源中最早章节，不是已证明的原生首次获取 |
| first_tower_zone | **已经有明确塔规则依据**的首次塔区；未核实为null，禁止默认成一区 |
| first_zone_basis | 当前11项为用户认可的2+2+2起手及备选兵器/一档药规则；这是塔设计许可，不伪称原生早期路线已验证 |
| proposed_first_tower_zone | 依原生章节顺序生成的待审建议区；proposal_is_approved=false，**不可直接用于正式抽取** |
| boss_associated / boss_associations | 当前Boss关联设计、关联核对状态、原生自然发放是否已证实；只认角色关联不等于自然必掉 |
| allow_reward / allow_shop | 当前保守准入；待核项为false。Boss关联兵器默认商店禁入；来源不明不兜底开放 |
| eligibility_status | 起手规则可用 / 待核 / 排除 |
| snap_soul_supported | 原生定义是否有铸魂数据；不是有令实机成功的证明 |
| native_condition_fields / special_restrictions | 原生锁定/持有/装备标签原文证据，剧情专用、权益/无用饰品、未命名项等限制；未完成装备实测也保留说明 |
| source_fingerprints | 输入文件SHA256，来源文件变化时重新审查 |

## 需要Claude保留的边界

1. **待核项不能进正式奖励/商店。** 当前119项待核，20项排除，11项仅按明确起手规则可用。`allow_*`是保守布尔门，不是后续内容永久删除表；逐项补证据、审核塔区后才能开放。
2. 本地导出主要是查表定义，缺少很多地图Actor/任务节点的实际调用关系。比如`Xigu.001.*`有多个武器行和看似后期的药，不能仅据行名前缀断言原生第一章一定可拿。未把这些静态行自动批准为一区货架。
3. `DT_MapData.OwnChapterID`连接`DT_ChapterData.ChapterOrder`才是本表的原生顺序证据。**旧塔场地配对不等于装备解锁顺序**：庞镇是原生第4章，梅谷湖第7章；不能因旧塔一区包含庞镇、二区包含梅谷湖，就把对应装备提前卖。
4. 建议映射1章→一区、2章→二区、3–5章→三区、6–10章→四区、11–12章→五区，只是按章节顺序保守分段的**设计提案**。不把它写成游戏原有五区，需统一审核后批准；来源还没验证的保持null。
5. 地图引用只接受MapID/表行名大小写匹配。`LuJiacCun`与`LuJiaCun`等拼写差异、`LongZhu`/`ErCijingLin`无直接地图连接的，不按名称/缩写猜归属。商人标签的字母缩写也不自动展开为地图。
6. 原生返尘/材料、周目、付费权益、特殊场景装备条件不能因为塔购物接口存在就绕过。后续买入/装备时仍需原生使用验收。

验证：`python -I mods/tower/tools/validate_equipment_progression.py` 检查完整覆盖、ID去重、JSON指针真实存在、输入哈希、待核禁入、Boss商店禁入与来源/批准区分。该检查验证数据合同，不认证原生剧情路线。
