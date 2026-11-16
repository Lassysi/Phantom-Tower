# 影塔·百战 Phantom Tower

《影之刃零》的爬塔 Mod，单人本地玩。下载安装包请去 [Releases](https://github.com/Lassysi/Phantom-Tower/releases)。

A boss-rush tower mod for Phantom Blade Zero. Installers are on the Releases page; the in-game text supports Chinese and English.

## 玩什么

一共 100 层：奇数层打 Boss，偶数层清小怪，清完进驿站买东西。打过 100 层进入无尽模式，层数不封顶，敌人一直变强。

- 用一份独立存档从新角色开始，不碰你原来的进度，退出影塔就回到原来的存档。
- 每 5 层选一次海克斯（三选一，可以刷新）。同一种海克斯可以升到 Lv2、Lv3。
- 兵器升级、修行、铸魂用影塔自己的点数，从 Boss 奖励和驿站里拿。
- 驿站卖兵器、饰品、药和影塔道具（定身符、复活符、重生符等），也能回收旧兵器。
- 倒下时有复活符就原地站起来，有重生符就退 10 层接着打。都没有就算失败，无尽模式可以回到最近的百层存档点。
- 爬塔进度随时可以保存退出，下次接着打。种子可以在安装器里导出，发给别人打同一局。

进游戏后按 Insert 打开影塔，手柄同时按下左右摇杆（L3 + R3）。

## 源码

这里放的是 4.1.1 的源码。放出来主要是想请有手柄的玩家帮忙。我自己没有手柄，现在手柄能用一部分，有些界面按了没反应，我没法测。

只有 Mod 自己的代码，没有游戏文件和解包资源。Mod 不改游戏 exe，靠 `avrt.dll` 加载 `PBZCore.dll`（运行库）和 `PBZTower.dll`（影塔）。

| 目录 | 内容 |
|---|---|
| `mods/tower/src/` | 影塔本体：爬塔逻辑、游戏内界面、输入 |
| `mods/tower/tests/` | 离线测试，不用开游戏 |
| `mods/tower/installer/` | 安装器和打包脚本 |
| `mods/tower/hotkey/` | 改打开键的小工具 |
| `src/core/` | 运行库、模块 SDK、`avrt.dll` 代理 |
| `shared/` | 共用数据表 |

### 手柄相关的代码

- `mods/tower/src/tower_ui.cpp`：界面里的手柄处理，搜"手柄"就能找到。XInput 四个槽都读。L3 + R3 开关界面，弹窗打开时把焦点给第一个按钮，A / X 确认、B 返回；游戏自己的导航没接住确认时，由影塔补一下。
- `mods/tower/src/tower_input_gate.h`：弹窗刚出来时先不收确认键（至少 400 ms，且 300 ms 内没按键），免得战斗里连按把奖励点掉。
- `mods/tower/src/tower_sony_pad.h`：PS4 手柄没走 XInput 时，用 DirectInput 读。

玩家反馈过的问题：

- 有的界面能点，有的点不动。
- 开 Steam 输入、用第三方手柄时键位不一样，还有摇杆漂移。
- 键鼠和手柄一起用时，焦点会被抢走。

日志在 `%LOCALAPPDATA%\PBZLab\core.log`，界面相关的行以"影塔界面："开头。

### 编译

需要 Visual Studio 2022 生成工具（C++ 桌面开发）和 .NET SDK。

脚本默认用 `E:\VSBuildTools`。装在别的地方，就把下面几个文件里的这个路径改掉：

- `mods/tower/tests/run.cmd`
- `src/core/build.bat`
- `src/core/sdk/build_module.bat`
- `src/core/proxy/build_proxy.bat`

```bat
:: 离线测试
mods\tower\tests\run.cmd
:: 完整打包（会先跑测试），输出到 dist\release\latest
powershell -ExecutionPolicy Bypass -File mods\tower\installer\build.ps1
```

只想换模块自己试的话，把编出来的 `build\release-mod\core\PBZTower.dll` 复制到游戏的 `platform\steam\PBZTower\`。换之前先关游戏，备份原文件。

### 提交改动

请写清楚：
- 手柄型号、怎么连的（有线 / 蓝牙、Steam 输入开没开）。
- 在哪个界面按了什么、结果怎样。

改动别影响键鼠玩家，提交前跑一遍 `mods\tower\tests\run.cmd`。

## 许可

Mod 本身暂时没有声明开源许可证。MinHook 的许可证在 `src/core/third_party/minhook/LICENSE.txt`。
