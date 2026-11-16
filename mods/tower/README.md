# 影塔 · Boss Rush 爬塔肉鸽 Mod

方案：`notes/bossrush-plan.md`（50 层、五区、1v1 为主，团战 / 观看押注 / 调味层，强化流派、驿站商店、一次性神通）。
独立 Mod：作为功能模块由运行库（`PBZCore.dll`）加载，不依赖修改器；修改器同时开着也可以。见 `src/core/sdk/README.md`。

## 分工（用户 2026-10-30：Claude 主导，Codex 协作）

| 部分 | 负责 | 位置 |
|---|---|---|
| 塔引擎：层管理、战力归一、刷 Boss / 清场、死亡接管、强化效果、词缀、团战与观看局、神通、局存档、管道协议 | Claude | `mods/tower/src/` |
| 游戏内原生界面（Insert / 钟菜单"影塔·百战"） | Claude | `mods/tower/src/tower_ui.cpp` |
| 加载器（注入运行库、加载模块）与打包；外部覆盖层已作废 | Codex | `mods/tower/ui/` |
| 数据：Boss 分档与白名单、擂台坐标、卡 / 商店 / 奇遇 / 50 层模板内容表、数据校验 | Codex | `mods/tower/data/`、`mods/tower/tools/` |

**交接**：`notes/handoff-tower.md`（现状、决策、热换、坑、待办），提示词 `notes/handoff-tower-prompts.md`。Codex 的任务单：`TASK-codex.md`。协议 v2 与接口请求答复：`PROTOCOL.md`。需要引擎新增接口时写到 `docs/api-requests.md`。

## 进度

| 阶段 | 内容 | 状态 |
|---|---|---|
| M0 | 层管理器、战力归一、N 层 1v1、3 选 1（12 张卡）、死亡接管、管道协议、开发控制台 | 已做：离线 24 项通过，模块在游戏里加载通过；**未实机打过一局** |
| M1 | 一区可玩：区模板、擂台传送、驿站商店、神通、调味层、局存档、覆盖层 | 进行中：统一起点、1v2（1v3 已去掉）、只清本塔的怪、魂玉、三选一（卡/道具/魂玉）、道具、神通（极稀有、不碰用户自己开的）、Boss 兵器必掉与入存档、局存档已做并实机验证；擂台传送、驿站、调味层、读 Codex 数据表、塔内换兵器待做 |
| M2 | 团战 2v2/3v3、魂契、观看押注局 | |
| M3 | 五区 50 层、全部玩法与词缀、塔主 | |
| M4 | 局外成长、种子分享、天梯、无尽 | |

## 编译与测试

```bat
mods\tower\tests\run.cmd      :: 纯逻辑离线测试 → TOWER_LOGIC_OK
mods\tower\build.cmd          :: → build\core\modules\PBZTower.dll
```

`tools/gen_pool.py` 从 `shared/enemies.json` 生成 Boss 池（`src/boss_pool.generated.h`），build.cmd 会检查它是否过期。

## 开发控制台（覆盖层做好前）

运行库在游戏里之后（打开修改器，或 `python -I tools/core/dev.py inject`）：

```bat
python -I mods/tower/tools/tower.py load
python -I mods/tower/tools/tower.py start 3
python -I mods/tower/tools/tower.py next
python -I mods/tower/tools/tower.py pick 2
python -I mods/tower/tools/tower.py stop
```

## 管道协议（M0）

请求：`{"op":"tower","cmd":"start","floors":3,"seed":0}`、`next`、`pick`（`"slot":0..2`）、`stop`、`state`、`probe`、`use`（`"item":"heal|freeze|slow|sunder|shachi"`）、`reroll`、`st`（`"slot":0..2`，用神通）、`resume`、`grant`（`"backup_ok":true`，前端先备份存档）。

state 里另有：`zone`、`jade`、`items`、`shentong`、`weapons`（含 `granted`）、`gains`（本层收获），`offer` 每项带 `kind`：card / item / jade。

每次状态变化广播：

```json
{"op":"tower","event":"state","phase":"idle|ready|fighting|reward|won|lost","floor":1,"floors":3,"seed":123,
 "boss":"昆仑叛教者：荒行子","msg":"第 1 层：…","offer":[{"id":"blade_1","name":"锋芒","school":"刀势","text":"伤害 +15%","rarity":0}],
 "picked":["铁骨"],"mods":{"dealt":1.15,"taken":0.85,"max_hp":1.0,"shachi":1.0,"speed":1.0,"posture":1.0,"heal":0.0}}
```

界面只靠这条消息渲染，不自己推断状态。

## M0 行为说明

- 本层 Boss 在你身边 8 米左右生成（M1 改成传送到擂台）。实机读它的最大生命，缩放到目标血量（第 1 层 25000，每层 +12%），Boss 伤害每层 +5%。
- 加成只作用于玩家和本塔的 Boss。修改器的倍率照常叠在外面。
- 战斗中把玩家生命下限抬到 1：掉到 1 判负，回满血后结束本局，不走游戏原生死亡。
- 清场用的是运行库的"清理本工具生成的怪"，会连修改器刷出来的怪一起清（M1 改成只清本塔的）。
