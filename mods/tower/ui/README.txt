影塔·百战 / Shadow Tower — Loader 0.3.0
=====================================
Bundled engine / 包内引擎: @ENGINE_VERSION@
Runtime/module ABI / 运行库与模块接口: 4

中文
----
这是加载器，不是修改器。影塔的游戏内入口与操作由引擎的原生 UI 提供。
1. 启动已适配版本的《影之刃零》，进入游戏。
2. 单文件版直接运行 EXE；文件夹版解压到任意位置，运行 PBZTower.exe。
3. 点击“加载影塔”，等待“影塔模块已连接”。已有运行库或影塔模块会复用。
4. 加载完成后可以关闭窗口。关闭只断开本窗口，不结束影塔，也不卸载运行库。

包内引擎为 v0.3.0-abi4。加载器使用游戏原生菜单，实机验收范围见更新记录。

仅限本地单人。不修改游戏 EXE、签名文件或安装目录。
适配 Build 26266538，加载前核对实际游戏 EXE 的 SHA-256；不同安装盘符、Windows
用户名或 SteamID 不影响识别，其他二进制版本需要重新适配。
本版是原生桌面窗口，不依赖 WebView2；自带 .NET 10 运行依赖，玩家不需要 SDK 或 Python。
加载器可以在主界面切换中文/英文。原生游戏界面的语言由引擎负责。

单文件首次运行会将必要组件解到：
  %LOCALAPPDATA%\PBZTower\packages\<载荷哈希>\
两种形态共用 %LOCALAPPDATA%\PBZTower\ 下的加载器语言设置，均不读取或修改游戏存档。
塔档案/局存档仍由引擎管理。不要从其他版本拼接 PBZCore.dll 和 PBZTower.dll。
出现接口不兼容时重启游戏；本加载器不执行运行库热换，也不反复注入。

卸载：先关闭加载器并退出游戏，再删除 EXE 或解压文件夹。
如还要清除个人塔进度和设置，可删除 %LOCALAPPDATA%\PBZTower\；这会同时删除该目录
中的局存档、账本及旧版备份，请先保留需要的文件。运行库宿主可能在
%LOCALAPPDATA%\PBZLab\modules\ 留有本模块的缓存副本；不要删除其他 Mod 或修改器的文件。
清理 .NET 自解包缓存时仅处理与本应用明确关联的缓存，不清空系统共享缓存目录。

README、更新记录和第三方许可声明位于包内 / 单文件展开目录内。
包内无游戏资产、密钥、资源导出、用户账号、存档或修改器操作界面。

English
-------
This is a loader, not a trainer. Gameplay and menus belong to the engine's native in-game UI.
1. Start the supported version of Phantom Blade Zero and enter the game.
2. Run the standalone EXE, or extract the folder ZIP anywhere and run PBZTower.exe.
3. Select Load Shadow Tower and wait for "Shadow Tower is connected". Existing compatible
   runtime and module instances are reused.
4. You may close this window afterward. Closing disconnects the loader only; it neither
   ends a run nor unloads the runtime.

Bundled engine: v0.3.0-abi4. The mod uses native game menus; see the changelog for validation scope.

Local single-player only. No game EXE, signed file or game installation file is modified.
Supported build: 26266538. The actual game executable SHA-256 is checked before loading.
Installation drive, Windows username and SteamID are not fixed. Other executable versions
need separate compatibility validation.
The native desktop loader does not use WebView2. The .NET 10 runtime is bundled; players
do not need a development SDK or Python. Switch the loader language in the main window;
the engine controls the language of its own in-game UI.

The standalone EXE extracts verified components to:
  %LOCALAPPDATA%\PBZTower\packages\<payload hash>\
Both packages share loader language settings under %LOCALAPPDATA%\PBZTower\.
The loader does not read or modify game saves. Tower progress remains engine-owned.
Never mix PBZCore.dll and PBZTower.dll from different frozen releases. Restart the game
for an incompatible runtime; this loader does not hot-swap or repeatedly inject it.

Uninstall: close the loader and exit the game, then delete the EXE or extracted folder.
Optionally remove %LOCALAPPDATA%\PBZTower\ to erase tower progress and settings. This also
deletes run saves, the reward ledger and any backups stored there; preserve wanted files first.
The runtime host may leave this module's cache in %LOCALAPPDATA%\PBZLab\modules\.
Do not remove other mods' or the trainer's files. Only clear .NET extraction caches
explicitly associated with this application, not a shared system cache directory.

README, changelog and third-party notices are included in the folder / extracted payload.
No game assets, keys, resource dumps, player accounts, saves or trainer UI are included.
Mod redistribution permissions are separate from the included third-party licenses.
