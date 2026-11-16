# T1 加载器交接

## 2026-10-31 更新（优先于下方历史记录）

- 加载器已升ABI4、版本0.3.0；只打包Claude冻结的 `v0.3.0-abi4`。Core SHA-256 `e9f53d3a11dfcfcd6315cd146c84ac72109bd2ce499f144c092d140e8f74693a`，模块 `17bf8ffa69b601fa515749e844480736de85f183d8d8a7a765e4118a80116e91`。不打包后续开发模块。
- 加载器隔离检查116项、冻结载荷6项通过；单EXE/文件夹ZIP已生成。真实游戏的ABI4核心注入/hello及模块加载有实机证据，但使用开发部署工具，**不是已完成发布版加载器真实点击注入验收**。
- 开发模块23c8117的首帧修复通过；原生场地精确落点、生成、原生敌群隔离存在失败。同进程回标题恢复缓存失败，最终退出/重启还有真档写入；已离线恢复，见 `notes/tower-growth-plan.md` 与 `notes/tower-venue-test.md`。
- **当前构建不是可公开发布的完整影塔版本，未上传Nexus。** 下方ABI3产物和旧部署权限仅为历史记录；ABI3已作废，运行库部署现由Codex按用户授权执行。

## 历史交接（0.2.0 / ABI3）

- 功能 ID：T1；基线 `ee227c1`，当前范围为加载器与双形态发布。
- 证据等级：静态 + 隔离进程/管道与真实 WinForms 控件渲染。未在真实游戏注入、卸载、切换功能或修改存档，不标实机 on/off 或稳定。
- 外部界面原型仅在 `codex/tower-overlay-paused` / `a00465d`，本分支和发布白名单中均无覆盖层、WebView2、玩法面板或全局游戏操作输入。

## 发布输入

用户明确指定 Claude 冻结目录 `E:/PBZ-claude/build/release/tower-engine/v0.2.0-abi3/`。

| 文件 | SHA-256 |
|---|---|
| PBZCore.dll | d8fde7d4627b4026b7abd15b794f7eeca01d3fa1262c27088939481e187f9a14 |
| PBZTower.dll | c5051c44eeac083f4d00568c53ee74aa001ee98c8e15e0a08ac1847ec6ba10f6 |

打包入口 `mods/tower/tools/package.ps1`。只消费冻结目录，不构建 DLL；校验 manifest 的版本、ABI、文件大小与 SHA-256。后续更新由 Claude 冻结新目录并告知版本，本方只更改脚本中的 EngineVersion；ABI 改变须同步前端检查。

## 验证

- `dotnet run --project mods/tower/ui/tests/PBZTower.Loader.Tests.csproj -c Release -- build/tower-ui/loader-tests`：115 项通过，实际小缓冲命名管道覆盖复用/加载、PID 校验、只注入一次、不发送玩法/restore/unload；协议覆盖旧 ABI、慢初始化、boot 变化和加载失败。
- `python -I mods/tower/tools/test_loader_payload.py`：6 项通过，覆盖冻结目录限制、错误 ABI/版本、SHA 错误拒绝、白名单与两种载荷逐文件一致。
- 中英真实控件在 96/120/144/192 DPI 尺度渲染，并验证标签容纳与放大窗口后的列边界。品牌图与图标复用现有 MechanicalFox 原始素材，等比显示。主界面仅有状态、简短操作提示、版本及按钮。
- 桌面 --mock 预览只创建本方窗口，不连接真实管道。电脑控制工具返回 `failed to activate captured window`，因此不将其错误截图片段作为验收依据；用户提供的预览截图用于修正文案、Logo 和缩放。后续布局以程序内真实控件渲染及边界检查为证据。
- 正式包离线验收：`verify_loader_packages.py` 32 项通过。实际单 EXE 在中文/空格独立目录中冷启动；关闭机器级 .NET 回退，确认内嵌和文件夹两形态均成功。逐项比对冻结 DLL 与全部 8 个逻辑载荷，单文件损坏缓存可修复，文件夹 DLL 损坏在接触游戏前被拒绝。证据 `build/tower-ui/package-tests/3c795031fd304ae08bb409364ba5f87b/verification.json`。

## 当前发布候选

编译/载荷源码提交 `cfde2271972aaa7d19111ea3f0712e8fd6033793`；输出 `dist/tower/`。

| 文件 | 大小（字节） | SHA-256 |
|---|---:|---|
| PBZTower-0.2.0-win-x64.exe | 52838247 | cdd1caa2e0b5383a62a65d7b8d90ffb97d102f817a202f8db9bf65242978fd25 |
| PBZTower-0.2.0-win-x64.zip | 47427062 | 9b95dc99ad7d8b0f32d40038eea4b6be29a71d4d7d82506bfbeff370aca524cf |

用户预览截图后要求进一步精简与修正缩放：现有界面仅保留短标题/状态/操作提示、版本、两个按钮与说明入口，原始狐狸 PNG 与 ICO 均已嵌入；布局采用自适应行列与 DPI 基准，并支持窗口放大。检查已包含中英、100/125/150/200% 及放大后边界。

## 已知问题与实测步骤

1. 当前获准的冻结引擎早于原生 UI 工作，加载器“已加载”不代表钟菜单入口已经完成。原生 UI / 完整游戏流程由 Claude 后续验收。
2. 本方未做真实游戏注入。协调测试时先告知用户：正常启动游戏 → 运行已打包加载器 → 点击加载 → 核对 ABI/模块状态 → 关闭加载器，确认运行库和影塔继续存在；再补已有运行库、旧 ABI、游戏重启与不重复注入路径。运行库热换只由 Claude 进行。
3. 兼容性按 Build 26266538 的实际 EXE SHA-256；未知二进制不注入，不靠安装盘符或 SteamID 识别。
4. 自解包缓存与 .NET 运行时缓存属于应用运行依赖；最终产物仅放 `dist/tower/`。测试文件和预览用户配置均在 `build/tower-ui/`。未上传 Nexus。
