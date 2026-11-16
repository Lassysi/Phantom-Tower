# T1 当前范围：加载器 + 发布（2026-10-30）

用户已将游戏内操作改派 Claude，用钟菜单入口“影塔·百战”和原生 CommonUI。当前分支基于 `ee227c1`；T1 只实现独立桌面加载器与单 EXE / 文件夹 ZIP，不含外部覆盖层、热键/手柄接管、选卡、道具、兵器领取或存档操作。

原外部 UI 原型封存在 `codex/tower-overlay-paused` 的 `a00465d`，未合入本分支。该原型未完成构建/验收，不能作为正式实现或交付物。

加载链：用户点击加载 → 识别真实游戏（排除自身/商店启动器）→ 验证适配的游戏指纹 → 连接管道并核对服务 PID → hello 检查 ABI 3、boot、游戏线程就绪 → module list → 复用 tower 或 module load。已有核心初始化时只等待；每个游戏进程实例最多尝试注入一次，不自动停用/热换。加载成功只证明模块接入，原生菜单是否可用由 Claude 对应引擎版本验收。

窗口：原生 WinForms、石墨/金黄配色，中英切换；显示连接进度、加载器版本、包内引擎版本和运行库版本。不依赖 WebView2。关闭只取消本方连接，绝不发送 unload、restore_all、tower stop 或 suspend。

正式打包：`mods/tower/tools/package.ps1`，固定引擎版本 `v0.2.0-abi3`，只读取 `E:/PBZ-claude/build/release/tower-engine/<版本>/`。先按 manifest.json 的 module_abi、文件大小、SHA-256 校验；不一致停止。已由用户指定本批核心 d8fde7d4…87f9a14、模块 c5051c44…6ba10f6。发布器不编译核心和模块，不读取 build/core 开发产物。

两种形态共用同一审核载荷和版本清单：自包含单 EXE 内嵌引擎与说明，首次运行解到 LOCALAPPDATA/PBZTower/packages/<hash>；ZIP 顶层 PBZTower/ 含自包含 EXE、core/、modules/、中英 README/CHANGELOG 和第三方声明。输出到 `dist/tower/`，逐文件及容器打印 SHA-256。只按白名单打包，不带游戏资源、密钥、notes/dump、个人存档、备份、日志、修改器界面或已暂停的前端代码。

验收：实际 LoaderProtocol 测 ABI 不符、慢初始化、已有/缺少模块和 boot 变化；实际命名管道测服务 PID、复用、不重复注入及断线；包校验测冻结 SHA 错误拒绝、相同载荷、解包越界/损坏恢复、独立目录冷启动和中英窗口。所有自动检查均用隔离假管道或 --mock，不接触真实 PBZLab 管道。实机注入/热换仍由 Claude 协调，未擅自开始。

T2/T3 保持原任务：数据按 PROTOCOL.md §6 的 schema 1 / items 格式，不写引擎生成头；Boss 实测与擂台坐标需要另行协调游戏时段，静态候选不能冒充白名单。
