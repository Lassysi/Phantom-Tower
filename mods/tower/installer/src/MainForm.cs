// 安装器主窗口：左侧游戏位置 / 安装 / 卸载，右侧使用说明，右上角赞赏。
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.IO;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Text;
using System.Windows.Forms;

namespace PBZTowerSetup
{
    sealed class MainForm : Form
    {
        const int Header = 76;
        readonly Image _fox = LoadImage("assets.fox.png");
        readonly Label _path = new Label(), _status = new Label(), _note = new Label();
        readonly InkButton _install = new InkButton { Text = "安装 Mod", Filled = true };
        readonly InkButton _uninstall = new InkButton { Text = "卸载 Mod" };
        readonly InkButton _browse = new InkButton { Text = "更改位置", Accent = Theme.Muted };
        readonly InkButton _donate = new InkButton { Text = "赞赏", Accent = Theme.Gold };
        readonly InkButton _records = new InkButton { Text = "战绩  ›", Accent = Theme.Gold };
        readonly InkButton _changes = new InkButton { Text = "更新说明", Accent = Theme.Gold };
        readonly InkButton _hotkey = new InkButton { Text = "打开键设置", Accent = Theme.Gold };
        readonly InkButton _seeds = new InkButton { Text = "种子", Accent = Theme.Gold };
        readonly InkButton _endless = new InkButton { Text = "无尽档", Accent = Theme.Gold };  // 3.5.0
        readonly InkButton _language = new InkButton { Text = "中文 / EN", Accent = Theme.Muted };
        Label _pathCaption, _statusCaption, _guideCaption;
        readonly CheckBox _wipe = new CheckBox();
        readonly RichTextBox _guide = new RichTextBox();
        readonly ContextMenuStrip _locations = new ContextMenuStrip();
        string _gameDir;

        public MainForm()
        {
            Text = "影塔·百战 安装";
            FormBorderStyle = FormBorderStyle.None;
            StartPosition = FormStartPosition.CenterScreen;
            ClientSize = new Size(980, 640);
            BackColor = Theme.Ink;
            DoubleBuffered = true;
            Font = Theme.Text(10f);
            using (var s = Assembly.GetExecutingAssembly().GetManifestResourceStream("assets.fox.ico")) if (s != null) Icon = new Icon(s);

            AddWindowButtons();
            _language.SetBounds(ClientSize.Width - 300, 22, 100, 32);
            _language.Click += (s, e) => { UiLanguage.English = !UiLanguage.English; ApplyLanguage(); };
            Controls.Add(_language);
            _donate.SetBounds(ClientSize.Width - 186, 22, 76, 32);
            _donate.Font = Theme.Text(10.5f, FontStyle.Bold);
            _donate.Click += (s, e) => { using (var d = new DonateForm()) d.ShowDialog(this); };
            Controls.Add(_donate);

            // 左栏
            const int L = 36, W = 340;
            _pathCaption = Caption("游戏位置", L, Header + 32); Controls.Add(_pathCaption);
            Style(_path, L, Header + 60, W, 46, Theme.Paper, 9.5f);
            _browse.SetBounds(L, Header + 110, 86, 30);
            _browse.Font = Theme.Text(9.5f, FontStyle.Bold);
            _browse.Click += (s, e) => _locations.Show(_browse, new Point(0, _browse.Height));
            SetupLocationMenu();
            Controls.Add(_browse);
            _hotkey.SetBounds(L + 92, Header + 110, 112, 30);
            _hotkey.Font = Theme.Text(9.5f, FontStyle.Bold);
            _hotkey.Click += (s, e) => { using (var d = new PBZTowerHotkey.HotkeyForm(_gameDir)) d.ShowDialog(this); };
            Controls.Add(_hotkey);
            _seeds.SetBounds(L + 210, Header + 110, 60, 30);
            _seeds.Font = Theme.Text(9.5f, FontStyle.Bold);
            _seeds.Click += (s, e) => { using (var d = new SeedForm()) d.ShowDialog(this); };
            Controls.Add(_seeds);
            _endless.SetBounds(L + 276, Header + 110, 64, 30);
            _endless.Font = Theme.Text(9.5f, FontStyle.Bold);
            _endless.Click += (s, e) => { using (var d = new EndlessForm()) d.ShowDialog(this); };
            Controls.Add(_endless);

            _statusCaption = Caption("状态", L, Header + 170); Controls.Add(_statusCaption);
            Style(_status, L, Header + 198, 110, 28, Theme.Gold, 10f, FontStyle.Bold);
            // 战绩：本局打到第几层、每层用时（读 %LOCALAPPDATA%\PBZTower，没装 Mod 也能看）
            _records.SetBounds(L + 112, Header + 192, 105, 36);
            _records.Font = Theme.Text(10.5f, FontStyle.Bold);
            _records.Click += (s, e) => { using (var d = new RecordForm()) d.ShowDialog(this); };
            Controls.Add(_records);
            _changes.SetBounds(L + 222, Header + 192, 118, 36);
            _changes.Font = Theme.Text(10.5f, FontStyle.Bold);
            _changes.Click += (s, e) => { using (var d = new ChangesForm()) d.ShowDialog(this); };
            Controls.Add(_changes);

            _install.SetBounds(L, Header + 250, W, 50);
            _install.Font = Theme.Text(13f, FontStyle.Bold);
            _install.Click += (s, e) => Run(Install);
            Controls.Add(_install);
            _uninstall.SetBounds(L, Header + 312, W, 44);
            _uninstall.Click += (s, e) => Run(Uninstall);
            Controls.Add(_uninstall);

            _wipe.Text = "卸载时同时删除影塔进度";
            _wipe.ForeColor = Theme.Muted;
            _wipe.Font = Theme.Text(9.5f);
            _wipe.SetBounds(L, Header + 366, W, 24);
            _wipe.FlatStyle = FlatStyle.Flat;
            Controls.Add(_wipe);

            Style(_note, L, Header + 404, W, 120, Theme.Muted, 9f);
            _note.Text = "安装、卸载前请先关闭游戏。\n只会在游戏目录放入 avrt.dll 和 PBZTower 文件夹，不改动游戏本体和原存档。";

            // 右栏：使用说明
            _guideCaption = Caption("使用说明", 420, Header + 32); Controls.Add(_guideCaption);
            _guide.SetBounds(420, Header + 62, ClientSize.Width - 420 - 36, ClientSize.Height - Header - 100);
            _guide.ReadOnly = true;
            _guide.BorderStyle = BorderStyle.None;
            _guide.BackColor = Theme.Panel;
            _guide.ForeColor = Theme.Paper;
            _guide.ScrollBars = RichTextBoxScrollBars.Vertical;
            _guide.DetectUrls = false;
            _guide.TabStop = false;
            Controls.Add(_guide);
            SetWindowTheme(_guide.Handle, "DarkMode_Explorer", null);  // 深色滚动条（Win10 1809+，旧系统忽略）
            Locate(GameLocator.Find());
            ApplyLanguage();
        }

        static string T(string zh, string en) { return UiLanguage.T(zh, en); }
        void ApplyLanguage()
        {
            Text = T("影塔·百战 安装", "Phantom Tower Setup");
            _statusCaption.Text = T("状态", "Status");
            _guideCaption.Text = T("使用说明", "Guide");
            _browse.Text = T("更改位置", "Browse");
            _hotkey.Text = T("打开键设置", "Menu key");
            _seeds.Text = T("种子", "Seeds");
            _endless.Text = T("无尽档", "Endless");
            _donate.Text = T("赞赏", "Support");
            _records.Text = T("战绩  ›", "Records ›");
            _changes.Text = T("更新说明", "Changes");
            _uninstall.Text = T("卸载 Mod", "Uninstall Mod");
            _wipe.Text = T("卸载时同时删除影塔进度", "Also delete tower progress");
            FillGuide(); UpdateState(); Invalidate();
        }

        // ---------- 操作 ----------
        void Locate(string dir)
        {
            _gameDir = dir;
            UpdateState();
        }

        void UpdateState()
        {
            _pathCaption.Text = T("游戏位置", "Game location") + " · " + GameLocator.PlatformName;
            _path.Text = _gameDir ?? T("请点「更改位置」选择游戏目录。", "Choose Browse to select your game folder.");
            if (_gameDir == null)
            {
                _status.Text = T("未找到游戏", "Not found");
                _install.Text = T("安装 Mod", "Install Mod");
                _install.Enabled = _uninstall.Enabled = _wipe.Enabled = false;
                _note.Text = "";
                return;
            }
            var mod = new ModFiles(_gameDir);
            var v = mod.InstalledVersion;
            _status.Text = mod.Installed ? T("已装 v", "v") + (v ?? "?") : T("未安装", "Not installed");
            var cmp = v == null ? -1 : ModFiles.CompareVersion(v, ModFiles.Version);
            _install.Text = !mod.Installed ? T("安装 Mod", "Install Mod") : cmp == 0 ? T("重新安装", "Reinstall") : cmp < 0 ? T("更新到 ", "Update to ") + ModFiles.Version : T("改装 ", "Install version ") + ModFiles.Version;
            _install.Enabled = true;
            _uninstall.Enabled = _wipe.Enabled = mod.Installed;
            _install.Invalidate();
            _note.Text = T("安装、卸载前请先关闭游戏。\n只会在游戏目录放入 avrt.dll 和 PBZTower 文件夹，不改动游戏本体和原存档。", "Close the game before installing or uninstalling. The mod uses avrt.dll and the PBZTower folder. Your campaign save is separate.") + Compat();
#if WEGAME_SETUP
            _note.Text = T("WeGame 测试版。安装完成后，请从 WeGame 启动游戏。\n", "WeGame test edition. Launch the game from WeGame after installation.\n") + _note.Text;
#endif
        }

        // 3.0.0 兼容：列出同目录其他 Mod（UE4SS / ReShade / 其他代理 DLL），以及上次进游戏时影塔核心的接入结果（core-status.txt）
        string Compat()
        {
            var found = new List<string>();
            Func<string, bool> has = name => { try { return File.Exists(Path.Combine(_gameDir, name)); } catch (Exception) { return false; } };
            if (has("dwmapi.dll") || Directory.Exists(Path.Combine(_gameDir, "ue4ss"))) found.Add("UE4SS");
            if (has("dxgi.dll") || has("ReShade.ini") || has("ReShade64.dll")) found.Add("ReShade");
            foreach (var proxy in new[] { "version.dll", "winmm.dll", "dinput8.dll", "xinput1_3.dll", "d3d11.dll" }) if (has(proxy)) found.Add(proxy);
            var text = found.Count > 0 ? T("\n检测到其他 Mod：", "\nOther mods found: ") + string.Join(T("、", ", "), found) + T("（可以共存）", " (can coexist)") : "";
            try
            {
                var status = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "PBZTower", "core-status.txt");
                if (File.Exists(status))
                {
                    var lines = File.ReadAllText(status, System.Text.Encoding.UTF8).Replace("\r", "").Split('\n');
                    if (lines.Length >= 2 && lines[0] == "conflict") text += T("\n上次进游戏：与其他 Mod 冲突，影塔未启用（", "\nLast launch: conflict with another mod; the tower was not loaded (") + lines[1] + T("）", ")");
                    else if (lines.Length >= 2 && lines[0] == "ok" && lines[1] != "正常") text += T("\n上次进游戏：影塔正常接入（", "\nLast launch: tower loaded (") + lines[1] + T("）", ")");
                }
            }
            catch (IOException) { }
            catch (UnauthorizedAccessException) { }
            return text;
        }

        void SetupLocationMenu()
        {
            var detected = new ToolStripMenuItem();
            var folder = new ToolStripMenuItem();
            var program = new ToolStripMenuItem();
            detected.Click += (s, e) => Locate((string)detected.Tag);
            folder.Click += (s, e) => BrowseFolder();
            program.Click += (s, e) => BrowseProgram();
            _locations.Items.AddRange(new ToolStripItem[] { detected, new ToolStripSeparator(), folder, program });
            _locations.Opening += (s, e) =>
            {
                detected.Tag = GameLocator.Find();
                detected.Enabled = detected.Tag != null;
                detected.Text = GameLocator.PlatformName + (detected.Enabled ? T("（自动识别）", " (detected)") : T("（未找到）", " (not found)"));
                folder.Text = T("手动选择游戏目录…", "Select game folder…");
                program.Text = T("手动选择游戏程序…", "Select game executable…");
            };
            Disposed += (s, e) => _locations.Dispose();
        }

        void BrowseFolder()
        {
            using (var d = new FolderBrowserDialog { Description = T("选择《影之刃零》的安装目录", "Select the Phantom Blade Zero installation folder"), ShowNewFolderButton = false })
            {
                if (_gameDir != null) d.SelectedPath = _gameDir;
                if (d.ShowDialog(this) == DialogResult.OK) SelectLocation(d.SelectedPath);
            }
        }

        void BrowseProgram()
        {
            using (var d = new OpenFileDialog { Title = T("选择游戏程序", "Select game executable"), Filter = T("游戏程序 (*.exe)|*.exe", "Game executable (*.exe)|*.exe") })
            {
                if (d.ShowDialog(this) == DialogResult.OK) SelectLocation(d.FileName);
            }
        }

        void SelectLocation(string path)
        {
            var dir = GameLocator.Normalize(path);
            if (dir != null) { Locate(dir); return; }
            Say(T("未识别到 " + GameLocator.PlatformName + " 版《影之刃零》。请选择安装目录或实际运行的 PBZ.exe。", "Phantom Blade Zero was not found for " + GameLocator.PlatformName + ". Select the installation folder or PBZ.exe."));
        }

        void Install()
        {
            if (!CheckGameLocation()) return;
            if (ModFiles.GameRunning()) { Say(T("请先关闭游戏再安装。", "Close the game before installing.")); return; }
            var mod = new ModFiles(_gameDir);
            var conflict = mod.Conflict();
            if (conflict != null) { Say(conflict); return; }
            var before = mod.Installed ? mod.InstalledVersion : null;
            if (mod.Installed && before == null && !Ask(T("检测到手动安装的影塔，但无法识别版本。2.0 之前的爬塔局无法继续，回退旧版并不简单。请先备份旧版 Mod 文件与 %LOCALAPPDATA%\\PBZTower。\n\n继续更新吗？", "A manually installed tower mod was found, but its version is unknown. Pre-2.0 runs cannot continue, and downgrading is not straightforward. Back up your old mod files and %LOCALAPPDATA%\\PBZTower first.\n\nContinue updating?"))) return;
            if (before != null && ModFiles.CompareVersion(before, ModFiles.Version) > 0 &&
                !Ask(T("游戏里已装较新的 " + before + "。回退可能无法继续新版爬塔局，请先备份影塔数据。\n确定安装旧版吗？", "Version " + before + " is newer. Downgrading may prevent your tower run from continuing. Back up your tower data first.\nInstall the older version?"))) return;
            if (ModFiles.TowerSessionOpen() && !Ask(T("上次影塔没有正常退出。更新后进入游戏，在影塔菜单选择「恢复原进度」。\n\n现在继续吗？", "The previous tower session did not end normally. After updating, enter the game and select Restore Original Progress in the tower menu.\n\nContinue?"))) return;
            // 3.0.0（用户 2026-11-12）：大更新，旧爬塔局不继续，需新开塔局。玩家还在塔档里（原存档被换下）时不能动，先回原进度
            var pre3 = before != null && ModFiles.CompareVersion(before, "3.0.0") < 0;
            if (pre3 && ModFiles.TowerSessionOpen()) { Say(T("3.0.0 需要新开塔局。\n你上次还在影塔里：请先进游戏，在影塔菜单选择「保存并退出」（或「恢复原进度」）回到原存档，关掉游戏后再更新。", "3.0.0 needs a new tower run.\nYou are still inside a tower run: load the game, choose Save and Exit (or Restore Original Progress) in the tower menu, close the game, then update.")); return; }
            if (pre3 && !Ask(T("3.0.0 是大更新：旧的爬塔局不能继续，更新后从第 1 层新开一局。\n旧局文件会移到 %LOCALAPPDATA%\\PBZTower\\legacy-" + before + " 留底，战绩记录保留；游戏原存档不受影响。\n\n确定更新吗？", "3.0.0 is a major update: old tower runs cannot continue, start a new run at Floor 1.\nOld run files move to %LOCALAPPDATA%\\PBZTower\\legacy-" + before + "; records are kept; your campaign save is unaffected.\n\nUpdate now?"))) return;
            if (pre3) ModFiles.ArchiveRun("legacy-" + before);
            var oldRun = pre3 || (before != null && ModFiles.CompareVersion(before, "2.0.0") < 0);
            if (oldRun && !Ask(T("2.0 以后玩法大幅重做，旧版爬塔进度不能继续，需从第 1 层开新局。请先备份旧版 Mod 和影塔数据；回退并不简单。\n游戏原存档与影塔局分开。\n确定更新吗？", "Version 2.0 and later is a major redesign. Pre-2.0 tower runs cannot continue: start a new run at Floor 1. Back up the old mod and tower data first; downgrading is not straightforward.\nYour campaign save is separate.\nUpdate now?"))) return;
            mod.Install();
            Say(mod.Installed && oldRun ? T("已更新到 " + ModFiles.Version + "。\n进入游戏后按 Insert 开新局。", "Updated to " + ModFiles.Version + ".\nLoad your save and press Insert to start a new tower run.")
                : mod.Installed && before != null ? T("已更新到 " + ModFiles.Version + "。\n爬塔进度保留，进入游戏后按 Insert 继续。", "Updated to " + ModFiles.Version + ".\nLoad your save and press Insert to continue your tower run.")
                                                 : T("安装完成。\n进入游戏后按 Insert 打开影塔。", "Installed.\nLoad your save and press Insert to open the tower."));
        }

        void Uninstall()
        {
            if (!CheckGameLocation()) return;
            if (ModFiles.GameRunning()) { Say(T("请先关闭游戏再卸载。", "Close the game before uninstalling.")); return; }
            if (ModFiles.TowerSessionOpen() && !Ask(T("上次影塔没有正常退出。建议先进游戏恢复原进度。\n\n现在卸载吗？", "The previous tower session did not end normally. Restoring your original progress in game first is recommended.\n\nUninstall now?"))) return;
            if (_wipe.Checked && !Ask(T("会同时删除影塔的爬塔进度、记录和塔内存档，无法恢复。确定吗？", "This permanently deletes tower progress, records and tower saves. Continue?"))) return;
            var kept = new ModFiles(_gameDir).Uninstall(_wipe.Checked);
            Say(kept.Count == 0 ? T("已卸载。", "Uninstalled.") : T("已卸载，以下文件没有删除：\n", "Uninstalled. These files were kept:\n") + string.Join("\n", kept));
        }

        bool CheckGameLocation()
        {
            if (_gameDir != null && GameLocator.Normalize(_gameDir) == _gameDir) return true;
            Locate(null);
            Say(T("游戏位置已失效，请重新选择。", "The game location is no longer valid. Select it again."));
            return false;
        }

        void Run(Action action)
        {
            try { action(); }
            catch (UnauthorizedAccessException) { Elevate(); }
            catch (IOException ex) { Say(T("文件被占用或无法写入：\n", "File in use or cannot be written:\n") + ex.Message + T("\n\n请关闭游戏后重试。", "\n\nClose the game and try again.")); }
            catch (Exception ex) { Say(T("出错了：\n", "Error:\n") + ex.Message); }
            UpdateState();
        }

        void Elevate()
        {
            if (!Ask(T("没有写入游戏目录的权限。\n以管理员身份重新打开安装器吗？", "Cannot write to the game directory.\nRestart the installer as administrator?"))) return;
            try { Process.Start(new ProcessStartInfo(Application.ExecutablePath) { UseShellExecute = true, Verb = "runas" }); Close(); }
            catch { }
        }

        void Say(string text) { MessageBox.Show(this, text, T("影塔·百战", "Phantom Tower"), MessageBoxButtons.OK); }
        bool Ask(string text) { return MessageBox.Show(this, text, T("影塔·百战", "Phantom Tower"), MessageBoxButtons.YesNo) == DialogResult.Yes; }

        // ---------- 说明 ----------
        void FillGuide()
        {
            string text;
            using (var s = Assembly.GetExecutingAssembly().GetManifestResourceStream("payload.guide.txt"))
            using (var r = new StreamReader(s, Encoding.UTF8)) text = r.ReadToEnd();
            const string englishHeading = "Phantom Tower — English guide";
            var englishAt = text.IndexOf(englishHeading, StringComparison.Ordinal);
            if (englishAt >= 0) text = UiLanguage.English ? text.Substring(englishAt) : text.Substring(0, englishAt);
            _guide.Clear();
            var head = Theme.Title(13f);
            var body = Theme.Text(10f);
            _guide.SelectionIndent = 14;
            _guide.SelectionRightIndent = 10;
            foreach (var raw in text.Replace("\r", "").Split('\n'))
            {
                var line = raw.TrimEnd();
                if (line.Length == 0 || line.Trim('=').Length == 0) continue;  // 空行 / 中英分隔线
                if (line.StartsWith("【") || (line.StartsWith("[") && line.EndsWith("]")) || line.StartsWith("Phantom Tower —"))  // 1.3.4：英文段的 [标题] 同样按标题显示
                {
                    if (_guide.TextLength > 0) Append("\n", body, Theme.Paper);
                    Append(line.Trim('【', '】', '[', ']') + "\n", head, Theme.Gold);
                }
                else Append(line + "\n", body, line.StartsWith("·") ? Theme.Paper : Color.FromArgb(214, 205, 190));
            }
            _guide.Select(0, 0);
        }

        void Append(string s, Font f, Color c)
        {
            _guide.SelectionStart = _guide.TextLength;
            _guide.SelectionLength = 0;
            _guide.SelectionFont = f;
            _guide.SelectionColor = c;
            _guide.SelectedText = s;  // AppendText 不带当前格式，标题会变回正文样式
        }

        // ---------- 外观 ----------
        protected override void OnPaint(PaintEventArgs e)
        {
            var g = e.Graphics;
            g.SmoothingMode = SmoothingMode.AntiAlias;
            g.TextRenderingHint = System.Drawing.Text.TextRenderingHint.AntiAliasGridFit;
            // 顶栏：深色带 + 朱红笔触
            using (var b = new LinearGradientBrush(new Rectangle(0, 0, Width, Header), Color.FromArgb(30, 27, 24), Theme.Ink, 90f)) g.FillRectangle(b, 0, 0, Width, Header);
            using (var p = new Pen(Color.FromArgb(150, Theme.Crimson), 3f)) g.DrawLine(p, 0, Header - 1, Width * 0.62f, Header - 1);
            using (var p = new Pen(Color.FromArgb(60, Theme.Gold), 1f)) g.DrawLine(p, Width * 0.62f, Header - 1, Width, Header - 1);
            if (_fox != null)
            {
                using (var clip = new GraphicsPath()) { clip.AddEllipse(22, 12, 52, 52); g.SetClip(clip); g.DrawImage(_fox, 22, 12, 52, 52); g.ResetClip(); }
                using (var p = new Pen(Theme.Gold, 1.5f)) g.DrawEllipse(p, 22, 12, 52, 52);
            }
            using (var f = Theme.Title(22f)) TextRenderer.DrawText(g, T("影塔·百战", "Phantom Tower"), f, new Point(86, 12), Theme.Paper);
            using (var f = Theme.Text(9.5f)) TextRenderer.DrawText(g, T("影之刃零 · Boss 爬塔 Mod  v", "Phantom Blade Zero · Boss Rush Mod  v") + ModFiles.Version, f, new Point(90, 48), Theme.Muted);
            // 分栏：右侧说明底板
            using (var b = new SolidBrush(Theme.Panel)) using (var path = Theme.Round(new RectangleF(404, Header + 52, Width - 404 - 24, Height - Header - 76), 8)) g.FillPath(b, path);
            using (var p = new Pen(Theme.Line)) g.DrawLine(p, 388, Header + 32, 388, Height - 30);
            // 左下角细墨线 + 青色点
            using (var p = new Pen(Color.FromArgb(90, Theme.Crimson), 2f)) g.DrawLine(p, 36, Height - 28, 160, Height - 28);
            using (var b = new SolidBrush(Theme.Cyan)) g.FillEllipse(b, 164, Height - 31, 6, 6);
            using (var p = new Pen(Theme.Line)) g.DrawRectangle(p, 0, 0, Width - 1, Height - 1);
        }

        Label Caption(string text, int x, int y)
        {
            return new Label { Text = text, Bounds = new Rectangle(x, y, 200, 24), ForeColor = Theme.Muted, BackColor = Color.Transparent, Font = Theme.Text(9.5f, FontStyle.Bold) };
        }

        void Style(Label l, int x, int y, int w, int h, Color c, float size, FontStyle style = FontStyle.Regular)
        {
            l.SetBounds(x, y, w, h);
            l.ForeColor = c;
            l.BackColor = Color.Transparent;
            l.Font = Theme.Text(size, style);
            Controls.Add(l);
        }

        void AddWindowButtons()
        {
            var close = new InkButton { Text = "✕", Accent = Theme.Muted };
            close.SetBounds(ClientSize.Width - 52, 22, 32, 32);
            close.Click += (s, e) => Close();
            var min = new InkButton { Text = "—", Accent = Theme.Muted };
            min.SetBounds(ClientSize.Width - 92, 22, 32, 32);
            min.Click += (s, e) => WindowState = FormWindowState.Minimized;
            Controls.Add(close);
            Controls.Add(min);
        }

        // 拖动无边框窗口
        [DllImport("uxtheme.dll", CharSet = CharSet.Unicode)] static extern int SetWindowTheme(IntPtr h, string app, string id);
        [DllImport("user32.dll")] static extern bool ReleaseCapture();
        [DllImport("user32.dll")] static extern IntPtr SendMessage(IntPtr h, int msg, int w, int l);
        protected override void OnMouseDown(MouseEventArgs e)
        {
            if (e.Button == MouseButtons.Left && e.Y < Header) { ReleaseCapture(); SendMessage(Handle, 0xA1, 2, 0); }
            base.OnMouseDown(e);
        }

        static Image LoadImage(string name)
        {
            using (var s = Assembly.GetExecutingAssembly().GetManifestResourceStream(name))
                return s == null ? null : new Bitmap(Image.FromStream(s));
        }
    }

    sealed class ChangesForm : Form
    {
        // 只显示中文段；发布目录里的更新说明是同一份文件
        static string ReadChanges()
        {
            using (var s = Assembly.GetExecutingAssembly().GetManifestResourceStream("payload.changes.txt"))
            {
                if (s == null) return "";
                using (var r = new StreamReader(s, Encoding.UTF8))
                {
                    var text = r.ReadToEnd().Replace("\r", "");
                var cut = text.IndexOf("\n---");
                return (cut < 0 ? text : UiLanguage.English ? text.Substring(cut + 4) : text.Substring(0, cut)).Trim();
                }
            }
        }
        public ChangesForm()
        {
            Text = UiLanguage.T("影塔·百战 " + ModFiles.Version + " 更新说明", "Phantom Tower " + ModFiles.Version + " Changes");
            StartPosition = FormStartPosition.CenterParent; ClientSize = new Size(660, 470);
            BackColor = Theme.Ink; ForeColor = Theme.Paper;
            var text = new Label { Location = new Point(28, 24), Size = new Size(604, 384), Font = Theme.Text(11f), ForeColor = Theme.Paper,
                Text = ReadChanges() };
            Controls.Add(text);
            var close = new InkButton { Text = UiLanguage.T("关闭", "Close"), Filled = true }; close.SetBounds(275, 420, 110, 36);
            close.Click += (s, e) => Close(); Controls.Add(close);
        }
    }

    sealed class DonateForm : Form
    {
        public DonateForm()
        {
            FormBorderStyle = FormBorderStyle.None;
            StartPosition = FormStartPosition.CenterParent;
            ClientSize = new Size(360, 470);
            BackColor = Theme.Ink;
            ShowInTaskbar = false;
            var img = new PictureBox { SizeMode = PictureBoxSizeMode.Zoom, BackColor = Color.Transparent };
            img.SetBounds(30, 54, 300, 360);
            using (var s = Assembly.GetExecutingAssembly().GetManifestResourceStream("assets.donate.jpg")) if (s != null) img.Image = new Bitmap(Image.FromStream(s));
            Controls.Add(img);
            var ok = new InkButton { Text = UiLanguage.T("关闭", "Close"), Filled = true };
            ok.SetBounds(130, 424, 100, 32);
            ok.Click += (s, e) => Close();
            Controls.Add(ok);
            KeyPreview = true;
            KeyDown += (s, e) => { if (e.KeyCode == Keys.Escape) Close(); };
        }

        protected override void OnPaint(PaintEventArgs e)
        {
            using (var f = Theme.Title(15f)) TextRenderer.DrawText(e.Graphics, UiLanguage.T("感谢支持", "Thank you"), f, new Rectangle(0, 14, Width, 30), Theme.Gold, TextFormatFlags.HorizontalCenter);
            using (var p = new Pen(Theme.Gold)) e.Graphics.DrawRectangle(p, 0, 0, Width - 1, Height - 1);
        }
    }

    static class Program
    {
        [STAThread]
        static void Main()
        {
            Application.EnableVisualStyles();
            Application.SetCompatibleTextRenderingDefault(false);
            Application.Run(new MainForm());
        }
    }
}
