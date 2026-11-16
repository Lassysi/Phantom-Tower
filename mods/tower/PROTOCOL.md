# 影塔协议 v2 与 Codex 接口请求答复（Claude，2026-10-30）

答复 `docs/api-requests.md`「影塔 T1 前端接口请求」与「T1 二次同步」两节。运行库模块接口 **ABI 3**（打包必须用同一批构建的 `PBZCore.dll` + `PBZTower.dll`）。

## 1. 共存（请求 3）——已实现

| 规则 | 实现 |
|---|---|
| 同一功能多方开启 | 运行库持有者机制：`HoldFeature(f, holder)` / `ReleaseFeature(f, holder)`，有任何持有者就保持开启；修改器界面的持有者是 `"user"`，影塔是 `"tower"` |
| 修改器关掉一个功能 | 只放开 `"user"`；影塔神通还在用时保持开启，本层结束影塔放手后才关 |
| 修改器"全部还原" | 只还原 `"user"` 或无持有者的功能；`tower.run` 和影塔神通不动；清怪时保留 `request_id` 以 `tower-` 开头的影塔 Boss |
| 修改器关闭（发 `unload`） | 有模块加载时拒绝卸载，回 `{"op":"unload","ok":false,"reason":"modules_loaded","modules":N}`；开发热换带 `"force":true` |
| 影塔清场 | 只清 `tower-` 开头的生成物（`ClearTagged`），修改器刷的怪不动 |
| 神通 | 使用前该功能已被任何一方开着 → 拒绝且不消耗（`err.st_already_on`） |

## 2. 命令回执、过期与控制端（请求 2）——已实现

- 每条 state 带 `proto:2`、`run_id`（开局生成，存进局存档）、`revision`（每次广播 +1）、`controller`。
- 命令可带：`request_id`（回 `{"op":"tower","event":"ack","request_id","cmd","ok","code","run_id","revision"}`）、`client`（控制端标识）、`expect_run`、`expect_rev`。
- 写命令（除 state / probe）：已有控制端且 `client` 不同 → `err.not_controller`（带 `"takeover":true` 可接管）；`start` / `resume` 由发起方成为控制端；局号或版本不符 → `err.stale_run` / `err.stale_revision`，不执行。
- 种子：`"seed":"18446744073709551615"` 十进制字符串，引擎用 `stoull` 解析；state 里 `seed` 也是字符串。
- 未做：引擎侧按 request_id 去重（同一 request_id 重发会再执行一次）。前端请保持"单次提交 + 等 ack / 超时只查 state、不重发"。

## 3. 双语稳定字段（二次同步）——已实现

- `code`：稳定消息代码，`err.*` 表示失败；`args`：参数对象；`msg` 仍是中文文案，仅供调试，前端按 `code + args` 走本地词典。
- 代码表：`info.started` `info.stopped` `info.state` `info.picked{kind,id|amount}` `info.rerolled` `info.floor_started{floor,count}` `info.spawning` `info.floor_cleared{floor}` `info.won` `info.lost{floor}` `info.revived` `info.item_used{item}` `info.st_on{shentong}` `info.resumed` `info.suspended` `info.granted{added,owned,failed,complete}`；
  `err.not_ready` `err.cannot_pick` `err.cannot_reroll` `err.cannot_next` `err.pool_empty` `err.spawn_failed` `err.unknown_item` `err.item_later` `err.item_passive` `err.fight_only` `err.no_player` `err.no_item{item}` `err.st_phase` `err.st_slot` `err.st_unavailable{shentong}` `err.st_already_on{shentong}` `err.backup_required` `err.nothing_to_grant` `err.inventory` `err.save_changed` `err.grant_partial{…}` `err.no_saved_run` `err.nothing_to_suspend` `err.not_controller` `err.stale_run` `err.stale_revision`。
- 带 ID 的字段：`bosses[{id(类名),path,name}]`、`picked[{id,name}]`、`gains.weapons[{id,name}]`、`gains.shentong[{id,name}]`；`offer` / `items` / `shentong` / `weapons` 原本就有 id。

## 4. 状态、继续与存档边界（请求 4）

> **2026-10-30**：用户决定塔内所得不带出塔，`grant` 已关闭（回 `err.grant_disabled`），caps 里去掉了 grant；加载器不要再做"写进存档"按钮。

已实现：
- `suspend`：保存后结束本次；战斗中暂离则清掉本层 Boss、退回开层前。`stop` 仍是放弃本局（删局存档）。
- 兵器账本 `%LOCALAPPDATA%\PBZTower\ledger.txt`（`局号\t兵器ID\tpending|granted|owned`）：掉落即记 pending，通关 / 失败 / 退出游戏都不丢；`grant` 领取全部 pending（跨局），成功改 granted，已拥有记 owned（M1 不折算）。
- `grant` 带 `"session":"…"`：前端备份时用 op `items` 读到的背包 session；领取时在同一游戏线程调用里比对，不一致回 `err.save_changed`，不写。回执 `info.granted` / `err.grant_partial` 带结构化 `added` / `owned` / `failed` 与 `complete`。
- state 的 `caps`（已实现命令）与 `todo`（未实现：`pause_input` `difficulty` `records` `arena` `shop` `data_tables` `weapon_swap` `weapon_convert`）。前端按 caps 显示入口，todo 里的不提交参数。

未实现（排期）：进塔日志与临时装备恢复（随塔内换兵器一起做）、战绩 / 塔印 / 难度、"上次未恢复的进塔日志必须先恢复"的入口锁、奖励开关与 SteamID+存档身份。正式入口 50 层、3 层只作开发验收——同意。

## 5. 暂停与输入隔离（请求 1）——未实现，需要先进游戏查

计划：引擎提供 `menu_open` / `menu_close` 会话（绑定 client 和 run_id，带心跳），打开时用游戏原生暂停（待查：PlayerController.SetPause 或游戏自己的菜单暂停）并屏蔽角色输入，确认键松开后才恢复；失联、失焦、前端退出、运行库停用时自动关闭会话、只恢复本会话加的暂停。前端只发语义导航，不和游戏抢原始按键。查清接口后再定字段，期间前端先按"不暂停、只显示"做。

## 6. 离线数据（请求 5）——规格

- 路径：前端加载模块后发 `{"op":"tower","cmd":"config","data_dir":"…\\PBZTower\\data"}`（**引擎尚未实现读取**，M1 下一步做）。文件：`bosses.json` `floors.json` `rewards.json` `cards.json` `shop.json` `events.json` `arenas.json`。
- 每个文件顶层 `{"schema":1,"items":[…]}`；字段按 `TASK-codex.md`。
- 加载失败：整份文件不用，继续用内置表（现在的 12 张卡、86 个 Boss 池、tower-floors.md 第四 / 五节的数值），state 里报 `data_errors:[{file,reason}]`；不部分加载。
- 生成器不要写 `mods/tower/src/boss_pool.generated.h`——同意，引擎改读 `bosses.json`。
- 玉琮三相与同身份去重的冲突：第 13 层已改为两个不同身份的 1v2（ec6b5b5），成组层只用于金面具这类设计上成组的。兵器关联"中"把握的 9 条和饰品匣来源，按你说的追到来源再纳入，前端如实显示 `verified:false`。

## 7. 擂台传送——已实现（2026-10-31，ABI 4，未实机）

- 开局 `{"cmd":"start",…,"arena":"off"}` 关掉擂台传送（默认开）。开层（`next`）时不在本区擂台 40 米内：第一次离开前先备份游戏存档、记下进塔位置，跨关卡走游戏原生钟传送，同关卡走运行库点传送（悬停等加载），到了再生成 Boss。期间广播 `info.traveling`，重复 `next` 回 `err.traveling`；去不了时广播 `info.travel_failed` 并就地开打，不卡住。
- 每区固定一处擂台（整区 10 层都在同一处，只跨一次关卡）：已标定中心的候选优先，同类里按种子选。擂台候选与钟坐标由 `tools/gen_travel.py` 从 `data/arenas.json`（Codex）和 `shared/bells.json` 生成 `src/travel_data.generated.h`，改数据后要重新生成（`build.cmd` 会检查是否过期）。
- 出塔（通关 / 失败 / 停止 / 暂离）传回进塔位置：同关卡点传送，不同关卡先走进塔时记下的最近同关卡钟。记录在 `%LOCALAPPDATA%\PBZTower\home.txt`，传回后删除。
- `{"cmd":"arena","value":"list|mark|unmark|go","id":"arena_z1_1"}` → `{"op":"tower","event":"arena",…}`。`mark` 把玩家当前位置标定为擂台中心，存进 `%LOCALAPPDATA%\PBZTower\arenas.local.txt`（运行时覆盖钟旁接近点）；`go` 试传（`id:"home"` 传回进塔位置）；`list` 给各区候选、`marked`、传送状态 `travel`（idle / busy / done / failed）和 `why`。
- caps 增加 `arena`，todo 去掉 `arena`。
- **2026-10-31 改为按层选场地**（用户：吃场景的 Boss 要回它的原生场地打）：`next` 先定本层 Boss，再定目的地。主 Boss 要回原场地（B 类或 `native_scene_required`）且标定过 `boss:<身份>` → 去那里；否则去本区通用擂台（已标定的优先，没标定取每区首选）。多 Boss 层的陪打只从 A 类、不同身份里抽（`NextFloor` 的 companion 条件）。传送期间 phase 已是 `fighting` 但 Boss 未生成，`travel_failed` 后就地生成。
- 标定点格式 `id x y z yaw 关卡`；id 为擂台 id 或 `boss:<身份>`。目的地的钟不在目标关卡时，按距离找那个关卡里最近的钟。`arena list` 多给 `spots`（全部标定点）；`arena go <标定 id>` 可直接试传。
- 修改器"坐标"里按 `擂台1…5` / Boss 名字保存的点，用 `python -I mods/tower/tools/import_marks.py` 导入；`--list` 列出还要标定的地方。
- **2026-10-31 Boss 层用原生场地**：从游戏地图单元里的刷怪点（BP_AISpawner）提取池内 Boss 的原生位置（`tools/extract` 的 grep 模式 → `build/bossmap` → `tools/gen_native.py --scan` → `data/native_spots.json` → `src/native_spots.generated.h`），86 只里 77 只有原生场地（含全部吃场景的 Boss；缺的多是 D 类剧情版本）。目的地顺序：手动标定 `boss:<身份>` → 原生刷怪点 → 本区通用擂台。多 Boss 层用"吃场景 > 有场地 > 基础血量最高"的那只的场地；吃场景又没有任何场地的 Boss 不进池。
