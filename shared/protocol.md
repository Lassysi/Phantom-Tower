# PBZLab 面板 ↔ 核心 协议 v0

传输：命名管道 `\\.\pipe\PBZLab`，UTF-8 JSON，一行一条消息（JSON Lines）。核心是服务端，面板是客户端，断线后面板每 2 秒重连。

## 面板 → 核心

```json
{"op":"hello","client":"PBZLab","ver":"0.1.0"}
{"op":"list"}
{"op":"set","id":"player.god","value":true}
{"op":"set","id":"combat.dmg_mul","value":3.0}
{"op":"action","id":"world.pos_save","args":{"slot":1}}
{"op":"restore_all"}
```

## 核心 → 面板

```json
{"op":"hello","core":"PBZCore","ver":"0.1.0","game_build":"26213077","engine":"unknown"}
{"op":"list","features":[{"id":"player.god","available":true,"value":false}, ...]}
{"op":"state","id":"player.god","value":true,"ok":true}
{"op":"state","id":"res.money","ok":false,"error":"signature_not_found"}
{"op":"event","type":"damage_dealt","data":{"amount":120,"target":"Boss_xx"}}
{"op":"log","level":"warn","msg":"..."}
```

- `available:false` 表示当前版本该功能定位失败，面板置灰并显示原因，不得隐藏。
- `event` 流供训练场、直播互动、AI 说书人复用，类型在 `features.json` 的 `events` 里登记。

## Mock 模式

面板启动参数 `--mock`：面板内置假核心，读 `features.json` 返回全部 `available:true`，`set` 原样回显，每秒随机发 `damage_dealt` 事件。Codex 用它在游戏解锁前开发完整 UI。

## 数值范围

slider 的 `min`/`max` 只是界面滑条的常用范围。倍率类（除 `cam.fov`、`world.weather_time`）允许 `set` 0~1000 的任意值，核心不得按 min/max 截断；核心只对确实会崩溃或写坏存档的值做保护，并在 `log` 里说明。
