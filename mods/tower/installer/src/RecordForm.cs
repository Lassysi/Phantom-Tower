// 战绩页（1.0.1 用户 2026-11-06）：读 %LOCALAPPDATA%\PBZTower 下影塔自己写的文件，只读、不改。
//   run-log.txt  本局逐层记录（开新局清空；死亡 / 通关追加结尾）
//   run.txt      进行中的局（层数、魂玉、逐层用时）；旧版本没有 run-log 时用它
//   records.txt  本机每层最快用时
//   hexes.txt    3.0.0：本局种子 + 按顺序选过的海克斯（显示层、名字、品质）
//   foresight.txt 透视眼：下一层 Boss
// 每 2 秒重读一次：游戏开着时打完一层，这里自己刷新。
// 3.0.0（用户 2026-11-12）：无尽模式可以打几百层——进度条一页 100 格翻页；逐层列表无尽层按波分行（每层最多 3 波、每波最多 3 个 Boss），
// 列表可滚轮 / 拖动滑块；右上角列出选过的海克斯（每页 5 个，翻页）和种子（点一下复制，方便分享）。
using System;
using System.Collections.Generic;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Drawing.Text;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Text;
using System.Windows.Forms;

namespace PBZTowerSetup
{
    sealed class FloorRow
    {
        public int Floor;
        public int Tenths;          // 0 = 跳过 / 没有用时
        public string Boss = "";    // 无尽混编层：波之间用 |，同波 Boss 用 、
        public string Challenge = "";
        public bool ChallengeDone;
        public bool Lost;           // 倒在这一层
        public int ShownLost;       // 倒下那层的显示层号（2.0.0 日志才有，波次段倒下时和 Boss 层不同）
        public string[] Waves { get { return Boss.IndexOf('|') >= 0 ? Boss.Split('|') : null; } }
    }

    sealed class HexPick { public int Floor, Tier; public string Name = ""; }

    sealed class RunRecord
    {
        public enum State { None, Running, Lost, Won, Ended }
        public State Status = State.None;
        public string Id = "";
        public DateTime? Started;
        public int Floors = 50, Cleared, Jade = -1, MobFloor, MobWave;
        public bool Endless;            // 左殇之后的无尽层（第 51 层 = 显示 100 起）
        public string Difficulty = "";  // 困难 / 简单（旧记录没有）
        public string Seed = "";
        public string Source = "";      // 3.5.0：用导入的无尽档开的局（作者|起始显示层），只在战绩里标
        public readonly List<HexPick> Hexes = new List<HexPick>();
        public readonly List<FloorRow> Rows = new List<FloorRow>();
        public readonly Dictionary<int, int> Best = new Dictionary<int, int>();

        // PBZTOWER_DIR：预览 / 排查时指向别的目录（默认 %LOCALAPPDATA%\PBZTower）
        public static string Dir { get { var o = Environment.GetEnvironmentVariable("PBZTOWER_DIR"); return !string.IsNullOrEmpty(o) ? o : Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "PBZTower"); } }

        static string Read(string leaf)
        {
            try { var p = Path.Combine(Dir, leaf); return File.Exists(p) ? File.ReadAllText(p, Encoding.UTF8) : null; }
            catch (IOException) { return null; }
            catch (UnauthorizedAccessException) { return null; }
        }

        static int Int(string s, int fallback = 0)
        {
            int v;
            return int.TryParse((s ?? "").Trim(), NumberStyles.Integer, CultureInfo.InvariantCulture, out v) ? v : fallback;
        }

        public static RunRecord Load()
        {
            var r = new RunRecord();
            var best = Read("records.txt");
            var sight = Read("foresight.txt");
            if (!string.IsNullOrWhiteSpace(sight)) { var p = sight.Trim().Split('\t'); r.Foresight = p.Length >= 2 ? UiLanguage.T("透视眼 · 第 " + p[0] + " 层　" + p[1], "Foresight · Floor " + p[0] + "  " + p[1]) : sight.Trim(); }
            if (best != null)
                foreach (var line in best.Split('\n'))
                {
                    var p = line.Trim().Split(new[] { ' ', '\t' }, StringSplitOptions.RemoveEmptyEntries);
                    if (p.Length >= 2 && Int(p[1]) > 0) r.Best[Int(p[0])] = Int(p[1]);
                }

            // 进行中的局
            var run = new Dictionary<string, string>();
            var runText = Read("run.txt");
            if (runText != null)
                foreach (var line in runText.Replace("\r", "").Split('\n'))
                {
                    var eq = line.IndexOf('=');
                    if (eq > 0) run[line.Substring(0, eq)] = line.Substring(eq + 1);
                }
            string runId;
            run.TryGetValue("id", out runId);

            // 本局逐层记录
            var log = Read("run-log.txt");
            var rows = new Dictionary<int, FloorRow>();
            string logId = null;
            if (log != null)
                foreach (var raw in log.Replace("\r", "").Split('\n'))
                {
                    var f = raw.Split('\t');
                    if (f.Length < 2) continue;
                    switch (f[0])
                    {
                        case "start":
                            logId = f[1];
                            rows.Clear();
                            r.Status = State.Running;
                            if (f.Length > 2) { long t; if (long.TryParse(f[2], out t) && t > 0) r.Started = DateTimeOffset.FromUnixTimeSeconds(t).LocalDateTime; }
                            if (f.Length > 3) r.Floors = Math.Max(1, Int(f[3], 50));
                            r.Difficulty = f.Length > 4 ? (f[4] == "easy" ? "简单" : "困难") : "";
                            break;
                        case "floor":
                            {
                                var row = new FloorRow { Floor = Int(f[1]), Tenths = f.Length > 2 ? Int(f[2]) : 0, Boss = f.Length > 3 ? f[3] : "" };
                                if (f.Length > 4) row.Challenge = f[4];
                                if (f.Length > 5) row.ChallengeDone = f[5] == "1";
                                if (row.Floor > 0) rows[row.Floor] = row;
                                break;
                            }
                        case "lost":
                            {
                                var row = new FloorRow { Floor = Int(f[1]), Tenths = f.Length > 2 ? Int(f[2]) : 0, Boss = f.Length > 3 ? f[3] : "", Lost = true, ShownLost = f.Length > 4 ? Int(f[4]) : 0 };
                                if (row.Floor > 0) rows[row.Floor] = row;
                                r.Status = State.Lost;
                                break;
                            }
                        case "won":
                            r.Status = State.Won;
                            break;
                        case "rebirth":     // 1.0.1 重生符：倒在 f[1] 层，回到已通过 f[2] 层；之后的层重新打
                        case "checkpoint":  // 3.0.0 存档点：同上
                            {
                                var back = f.Length > 2 ? Int(f[2]) : 0;
                                foreach (var k in rows.Keys.Where(x => x > back).ToList()) rows.Remove(k);
                                r.Status = State.Running;
                                break;
                            }
                    }
                }

            var logMatches = logId != null && (runId == null || runId == logId);
            if (!logMatches) { rows.Clear(); r.Status = State.None; r.Started = null; }
            if (runId != null)
            {
                // 局存档里的用时为准（续局、旧版本没有 run-log 时也有）
                r.Id = runId;
                r.Floors = Int(run.ContainsKey("floors") ? run["floors"] : null, r.Floors);
                r.Cleared = Int(run.ContainsKey("floor") ? run["floor"] : null);
                r.Jade = Int(run.ContainsKey("jade") ? run["jade"] : null, -1);
                if (run.ContainsKey("endless")) r.Endless = run["endless"].Trim() == "1";
                if (run.ContainsKey("src")) r.Source = run["src"].Trim();
                if (run.ContainsKey("mob")) { var m = run["mob"].Split(','); if (m.Length == 2) { r.MobFloor = Int(m[0]); r.MobWave = Int(m[1]); } }
                if (run.ContainsKey("diff")) r.Difficulty = run["diff"].Trim() == "1" ? "简单" : "困难";
                if (run.ContainsKey("seed")) r.Seed = run["seed"].Trim();
                if (r.Status == State.None || r.Status == State.Lost || r.Status == State.Won) r.Status = State.Running;
                string times;
                if (run.TryGetValue("times", out times))
                {
                    var t = times.Split(',');
                    for (var i = 0; i < t.Length && i < r.Cleared; i++)
                    {
                        FloorRow row;
                        if (!rows.TryGetValue(i + 1, out row)) rows[i + 1] = row = new FloorRow { Floor = i + 1 };
                        var v = Int(t[i]);
                        if (v > 0) row.Tenths = v;
                    }
                }
                for (var f = 1; f <= r.Cleared; f++) if (!rows.ContainsKey(f)) rows[f] = new FloorRow { Floor = f };
            }
            else if (logMatches)
            {
                r.Id = logId;
                if (r.Status == State.Running) r.Status = State.Ended;  // 局存档没了又没有结尾：放弃 / 退出了
                r.Cleared = rows.Values.Where(x => !x.Lost).Select(x => x.Floor).DefaultIfEmpty(0).Max();
                var lowest = rows.Count == 0 ? 1 : rows.Keys.Min();
                for (var f = lowest; f <= r.Cleared; f++) if (!rows.ContainsKey(f)) rows[f] = new FloorRow { Floor = f };
            }
            if (r.Floors >= 50 && rows.Keys.Any(x => x > 50)) r.Endless = true;
            r.Rows.AddRange(rows.Values.OrderBy(x => x.Floor));

            // 海克斯 + 种子（只认本局的）
            var hex = Read("hexes.txt");
            if (hex != null)
            {
                var mine = true;
                foreach (var raw in hex.Replace("\r", "").Split('\n'))
                {
                    var f = raw.Split('\t');
                    if (f[0] == "seed" && f.Length >= 2)
                    {
                        mine = f.Length < 4 || r.Id.Length == 0 || f[3] == r.Id;
                        if (mine && r.Seed.Length == 0) r.Seed = f[1];
                    }
                    else if (f[0] == "hex" && f.Length >= 3 && mine)
                        r.Hexes.Add(new HexPick { Floor = Int(f[1]), Name = f[2], Tier = f.Length > 3 ? Int(f[3], 1) : 1 });
                }
            }
            return r;
        }

        // 显示用总层数：50 层的局写 100；进了无尽就写当前打到的显示层（没有上限）
        public int Shown { get { return Floors >= 50 ? (Endless ? Math.Max(100, ShownFloor(Cleared + 1)) : 100) : Floors; } }
        // 进度条一格一个 Boss 层：常规 50 格；无尽层按已通过的层数往后加（翻页显示，一页 100 格）
        public int Cells { get { return Floors >= 50 ? (Endless ? Math.Max(51, Cleared + 1) : 50) : Floors; } }
        // Boss 层（内部 1–50）→ 显示层号，与游戏内 ShownBoss 相同；无尽层：第 51 层 = 显示 100
        public int ShownFloor(int f) { return Floors < 50 || f <= 0 ? f : f > 50 ? f + 49 : 2 * f - 1; }
        public string Foresight = "";   // foresight.txt：透视眼看到的下一层 Boss（显示层 \t 文本）
        public int ShownOf(FloorRow x) { return x.Lost && x.ShownLost > 0 ? x.ShownLost : ShownFloor(x.Floor); }
        // 已通过的显示层数：Boss 层，或打完了的波次段（波数 1 / 2 / 3 与 tower_mobs.h 相同）
        public int ShownCleared
        {
            get
            {
                var s = ShownFloor(Cleared);
                if (Floors >= 50 && Cleared < 50 && MobFloor == Cleared + 1 && MobFloor >= 2 && MobWave >= (MobFloor <= 15 ? 1 : MobFloor <= 35 ? 2 : 3)) s = Math.Max(s, 2 * MobFloor - 2);
                return s;
            }
        }
        public IEnumerable<FloorRow> Timed { get { return Rows.Where(x => x.Tenths > 0 && !x.Lost); } }
        public int Total { get { return Timed.Sum(x => x.Tenths); } }

        public static string Clock(int tenths)
        {
            if (tenths <= 0) return "--:--";
            var s = tenths / 10;
            return s >= 3600 ? string.Format("{0}:{1:00}:{2:00}", s / 3600, s / 60 % 60, s % 60)
                             : string.Format("{0:00}:{1:00}.{2}", s / 60, s % 60, tenths % 10);
        }

        // Boss 名（中英）：无尽层每段用 、 分隔
        public static string Names(string raw) { return string.Join(UiLanguage.T("、", ", "), raw.Split('、').Select(BossNames.Display)); }
    }

    sealed class RecordForm : Form
    {
        const int Pad = 28, PageCells = 100, HexPerPage = 5;
        RunRecord _rec = RunRecord.Load();
        string _sig;
        readonly FloorList _list = new FloorList();
        readonly Timer _poll = new Timer { Interval = 2000 };
        int _hoverCell = -1, _page = -1, _hexPage = -1;
        Rectangle _strip, _pagePrev, _pageNext, _hexBox, _hexPrev, _hexNext, _seedRect;
        string _copied = "";

        public RecordForm()
        {
            FormBorderStyle = FormBorderStyle.None;
            StartPosition = FormStartPosition.CenterParent;
            ClientSize = new Size(1080, 720);
            BackColor = Theme.Ink;
            DoubleBuffered = true;
            ShowInTaskbar = false;
            KeyPreview = true;
            Font = Theme.Text(10f);
            KeyDown += (s, e) => { if (e.KeyCode == Keys.Escape) Close(); };

            var close = new InkButton { Text = "✕", Accent = Theme.Muted };
            close.SetBounds(ClientSize.Width - 52, 20, 32, 32);
            close.Click += (s, e) => Close();
            Controls.Add(close);

            _list.SetBounds(Pad, 340, ClientSize.Width - Pad * 2, ClientSize.Height - 340 - 44);
            Controls.Add(_list);

            _sig = Signature();
            _list.Set(_rec);
            _poll.Tick += (s, e) => { var sig = Signature(); if (sig != _sig) { _sig = sig; _rec = RunRecord.Load(); _list.Set(_rec); Invalidate(); } };
            _poll.Start();
            FormClosed += (s, e) => _poll.Dispose();
        }

        static string Signature()
        {
            var sb = new StringBuilder();
            foreach (var leaf in new[] { "run-log.txt", "run.txt", "records.txt", "foresight.txt", "hexes.txt" })
            {
                try { var fi = new FileInfo(Path.Combine(RunRecord.Dir, leaf)); sb.Append(fi.Exists ? fi.LastWriteTimeUtc.Ticks + ":" + fi.Length : "-").Append('|'); }
                catch (IOException) { sb.Append("?|"); }
            }
            return sb.ToString();
        }

        int Pages { get { return Math.Max(1, (Math.Max(1, _rec.Cells) + PageCells - 1) / PageCells); } }
        // 默认翻到正在打的那一页；玩家自己翻过就停在那页
        int Page { get { var p = _page >= 0 ? _page : Math.Min(Pages - 1, Math.Max(0, _rec.Cleared) / PageCells); return Math.Min(p, Pages - 1); } }
        int HexPages { get { return Math.Max(1, (_rec.Hexes.Count + HexPerPage - 1) / HexPerPage); } }
        int HexPage { get { var p = _hexPage >= 0 ? _hexPage : HexPages - 1; return Math.Min(p, HexPages - 1); } }

        // ---------- 绘制 ----------
        protected override void OnPaint(PaintEventArgs e)
        {
            var g = e.Graphics;
            g.SmoothingMode = SmoothingMode.AntiAlias;
            g.TextRenderingHint = TextRenderingHint.AntiAliasGridFit;
            var r = _rec;

            // 背景：顶部暖色晕染 + 右上一抹朱红
            using (var b = new LinearGradientBrush(new Rectangle(0, 0, Width, 310), Color.FromArgb(34, 30, 26), Theme.Ink, 90f)) g.FillRectangle(b, 0, 0, Width, 310);
            using (var path = new GraphicsPath())
            {
                path.AddEllipse(Width - 360, -220, 520, 420);
                using (var pg = new PathGradientBrush(path) { CenterColor = Color.FromArgb(40, Theme.Crimson), SurroundColors = new[] { Color.FromArgb(0, Theme.Crimson) } })
                    g.FillPath(pg, path);
            }

            // 标题
            using (var f = Theme.Title(24f)) TextRenderer.DrawText(g, UiLanguage.T("战 绩", "Records"), f, new Point(Pad - 4, 16), Theme.Gold);
            using (var p = new Pen(Color.FromArgb(160, Theme.Crimson), 3f)) g.DrawLine(p, Pad, 62, Pad + 92, 62);
            using (var b = new SolidBrush(Theme.Cyan)) g.FillEllipse(b, Pad + 98, 59, 6, 6);
            var sub = r.Started.HasValue ? UiLanguage.T("本局开始于 ", "Run started ") + r.Started.Value.ToString("yyyy-MM-dd HH:mm") : r.Status == RunRecord.State.None ? UiLanguage.T("还没有爬塔记录", "No tower records yet") : UiLanguage.T("本局", "Current run");
            // 难度印章（标题右侧）：困难朱红、简单青色；旧记录没有难度就不画
            var infoX = Pad + 120;
            if (r.Difficulty.Length > 0 && r.Status != RunRecord.State.None) { DrawSeal(g, new Rectangle(Pad + 116, 18, 52, 52), r.Difficulty); infoX = Pad + 184; }
            using (var f = Theme.Text(9.5f)) TextRenderer.DrawText(g, sub, f, new Point(infoX, 30), Theme.Muted);
            DrawStatusChip(g, new Point(infoX, 52), r);

            // 右上：海克斯 + 种子
            _hexBox = new Rectangle(Width - Pad - 268, 64, 268, 186);
            DrawHexes(g, _hexBox, r);

            if (r.Foresight.Length > 0)
                using (var f = Theme.Text(9f, FontStyle.Bold)) TextRenderer.DrawText(g, r.Foresight, f, new Rectangle(infoX, 77, _hexBox.X - infoX - 12, 20), Theme.Gold, TextFormatFlags.EndEllipsis);

            // 层数徽章
            var badge = new Rectangle(Pad, 100, 150, 150);
            DrawBadge(g, badge, r);

            // 数据块（夹在徽章和海克斯栏之间）
            var tiles = TileData(r);
            int tx = badge.Right + 22, tw = (_hexBox.X - 18 - tx - 3 * 12) / 4;
            for (var i = 0; i < tiles.Length; i++) DrawTile(g, new Rectangle(tx + i * (tw + 12), 110, tw, 134), tiles[i]);

            // 塔层进度（一页 100 格）
            _strip = new Rectangle(Pad, 280, Width - Pad * 2, 34);
            DrawStrip(g, _strip, r);

            // 列表标题
            using (var f = Theme.Text(9f, FontStyle.Bold)) TextRenderer.DrawText(g, UiLanguage.T("逐层记录", "Floor records"), f, new Point(Pad, 320), Theme.Muted);
            using (var f = Theme.Text(8.5f))
                TextRenderer.DrawText(g, UiLanguage.T("滚轮或拖动右侧滑块　·　无尽层按波显示　·　★ = 本机该层最快", "Scroll or drag the bar · Endless floors list each wave · ★ = local best"), f, new Rectangle(Pad, 320, Width - Pad * 2, 20), Color.FromArgb(110, 102, 92), TextFormatFlags.Right);

            // 底部
            using (var p = new Pen(Color.FromArgb(90, Theme.Crimson), 2f)) g.DrawLine(p, Pad, Height - 22, Pad + 110, Height - 22);
            using (var f = Theme.Text(8.5f)) TextRenderer.DrawText(g, UiLanguage.T(@"记录来自本机 %LOCALAPPDATA%\PBZTower，只读，不影响游戏", @"Records: %LOCALAPPDATA%\PBZTower (read only)"), f, new Point(Pad + 120, Height - 31), Color.FromArgb(100, 94, 86));
            using (var p = new Pen(Theme.Line)) g.DrawRectangle(p, 0, 0, Width - 1, Height - 1);
            using (var p = new Pen(Color.FromArgb(70, Theme.Gold))) g.DrawLine(p, 1, 1, Width - 2, 1);
        }

        static Color TierColor(int tier) { return tier >= 3 ? Color.FromArgb(240, 150, 60) : tier == 2 ? Color.FromArgb(150, 120, 240) : Color.FromArgb(120, 190, 220); }

        void DrawHexes(Graphics g, Rectangle box, RunRecord r)
        {
            using (var path = Theme.Round(box, 10))
            {
                using (var b = new LinearGradientBrush(box, Color.FromArgb(36, 33, 29), Color.FromArgb(24, 22, 20), 90f)) g.FillPath(b, path);
                using (var p = new Pen(Theme.Line)) g.DrawPath(p, path);
            }
            using (var p = new Pen(Theme.Gold, 3f) { StartCap = LineCap.Round, EndCap = LineCap.Round }) g.DrawLine(p, box.X + 16, box.Y + 1.5f, box.X + 46, box.Y + 1.5f);
            using (var f = Theme.Text(9f, FontStyle.Bold))
                TextRenderer.DrawText(g, UiLanguage.T("海克斯", "Hexes") + (r.Hexes.Count > 0 ? "  " + r.Hexes.Count : ""), f, new Point(box.X + 14, box.Y + 10), Theme.Muted);
            // 翻页
            var pages = HexPages; var page = HexPage;
            _hexPrev = _hexNext = Rectangle.Empty;
            if (pages > 1)
                using (var f = Theme.Text(9f, FontStyle.Bold))
                {
                    _hexPrev = new Rectangle(box.Right - 92, box.Y + 6, 22, 22);
                    _hexNext = new Rectangle(box.Right - 30, box.Y + 6, 22, 22);
                    TextRenderer.DrawText(g, "‹", f, _hexPrev, page > 0 ? Theme.Gold : Theme.Line, TextFormatFlags.HorizontalCenter | TextFormatFlags.VerticalCenter);
                    TextRenderer.DrawText(g, (page + 1) + "/" + pages, f, new Rectangle(_hexPrev.Right, box.Y + 6, _hexNext.X - _hexPrev.Right, 22), Theme.Paper, TextFormatFlags.HorizontalCenter | TextFormatFlags.VerticalCenter);
                    TextRenderer.DrawText(g, "›", f, _hexNext, page < pages - 1 ? Theme.Gold : Theme.Line, TextFormatFlags.HorizontalCenter | TextFormatFlags.VerticalCenter);
                }
            var y = box.Y + 34;
            if (r.Hexes.Count == 0)
                using (var f = Theme.Text(9f)) TextRenderer.DrawText(g, UiLanguage.T("本局还没有选海克斯", "No hexes picked yet"), f, new Rectangle(box.X + 14, y + 30, box.Width - 28, 22), Color.FromArgb(110, 102, 92));
            using (var fFloor = Theme.Text(8.5f))
            using (var fName = Theme.Text(10f, FontStyle.Bold))
                for (var i = page * HexPerPage; i < Math.Min(r.Hexes.Count, (page + 1) * HexPerPage); i++, y += 24)
                {
                    var h = r.Hexes[i];
                    using (var b = new SolidBrush(TierColor(h.Tier))) g.FillEllipse(b, box.X + 16, y + 8, 7, 7);
                    TextRenderer.DrawText(g, (i + 1) + ".", fFloor, new Point(box.X + 28, y + 3), Theme.Muted);
                    TextRenderer.DrawText(g, h.Name, fName, new Rectangle(box.X + 50, y, box.Width - 130, 22), TierColor(h.Tier), TextFormatFlags.VerticalCenter | TextFormatFlags.SingleLine | TextFormatFlags.EndEllipsis);
                    TextRenderer.DrawText(g, UiLanguage.T("第 " + h.Floor + " 层", "F" + h.Floor), fFloor, new Rectangle(box.Right - 82, y, 70, 22), Color.FromArgb(130, 122, 110), TextFormatFlags.Right | TextFormatFlags.VerticalCenter);
                }
            // 种子：点一下复制
            _seedRect = Rectangle.Empty;
            if (r.Seed.Length > 0)
            {
                _seedRect = new Rectangle(box.X + 10, box.Bottom - 30, box.Width - 20, 24);
                using (var path = Theme.Round(_seedRect, 6)) using (var b = new SolidBrush(Color.FromArgb(40, Theme.Cyan))) g.FillPath(b, path);
                var text = _copied == r.Seed ? UiLanguage.T("种子 " + r.Seed + "　已复制", "Seed " + r.Seed + "  copied") : UiLanguage.T("种子 " + r.Seed + "　点击复制", "Seed " + r.Seed + "  click to copy");
                using (var f = Theme.Text(9f, FontStyle.Bold)) TextRenderer.DrawText(g, text, f, _seedRect, Theme.Cyan, TextFormatFlags.HorizontalCenter | TextFormatFlags.VerticalCenter | TextFormatFlags.SingleLine);
            }
        }

        static void DrawStatusChip(Graphics g, Point at, RunRecord r)
        {
            string text; Color c;
            switch (r.Status)
            {
                case RunRecord.State.Running: text = r.Endless ? UiLanguage.T("无尽中", "Endless") : UiLanguage.T("进行中", "In progress"); c = Theme.Cyan; break;
                case RunRecord.State.Lost: text = UiLanguage.T("已殒命 · 倒在第 ", "Defeated · Floor ") + r.Rows.Where(x => x.Lost).Select(x => r.ShownOf(x)).DefaultIfEmpty(r.ShownFloor(r.Cleared + 1)).First() + UiLanguage.T(" 层", ""); c = Color.FromArgb(220, 70, 64); break;
                case RunRecord.State.Won: text = UiLanguage.T("已通关", "Cleared"); c = Theme.Gold; break;
                case RunRecord.State.Ended: text = UiLanguage.T("本局已结束", "Run ended"); c = Theme.Muted; break;
                default: text = UiLanguage.T("暂无进行中的局", "No active run"); c = Theme.Muted; break;
            }
            if (r.Source.Length > 0) { var sp = r.Source.Split('|'); text += UiLanguage.T("　·　导入无尽档 · " + sp[0] + (sp.Length > 1 ? " · 第 " + sp[1] + " 层起" : ""), "  ·  Imported endless save · " + sp[0] + (sp.Length > 1 ? " · from floor " + sp[1] : "")); }
            using (var f = Theme.Text(9f, FontStyle.Bold))
            {
                var size = TextRenderer.MeasureText(g, text, f);
                var rect = new RectangleF(at.X, at.Y, size.Width + 26, 22);
                using (var path = Theme.Round(rect, 11))
                {
                    using (var b = new SolidBrush(Color.FromArgb(34, c))) g.FillPath(b, path);
                    using (var p = new Pen(Color.FromArgb(150, c), 1f)) g.DrawPath(p, path);
                }
                using (var b = new SolidBrush(c)) g.FillEllipse(b, rect.X + 9, rect.Y + 8, 6, 6);
                TextRenderer.DrawText(g, text, f, new Point((int)rect.X + 19, (int)rect.Y + 3), c);
            }
        }

        // 印章：双线方框 + 楷体两字，略微倾斜
        static void DrawSeal(Graphics g, Rectangle rect, string text)
        {
            var hard = text == "困难";
            var c = hard ? Color.FromArgb(214, 58, 52) : Theme.Cyan;
            var state = g.Save();
            g.TranslateTransform(rect.X + rect.Width / 2f, rect.Y + rect.Height / 2f);
            g.RotateTransform(-6f);
            var box = new RectangleF(-rect.Width / 2f, -rect.Height / 2f, rect.Width, rect.Height);
            using (var path = Theme.Round(box, 6))
            {
                using (var b = new SolidBrush(Color.FromArgb(hard ? 46 : 34, c))) g.FillPath(b, path);
                using (var p = new Pen(c, 2f)) g.DrawPath(p, path);
            }
            using (var inner = Theme.Round(RectangleF.Inflate(box, -4, -4), 4)) using (var p = new Pen(Color.FromArgb(150, c), 1f)) g.DrawPath(p, inner);
            using (var f = Theme.Title(13f))
            using (var b = new SolidBrush(c))
            using (var sf = new StringFormat { Alignment = StringAlignment.Center, LineAlignment = StringAlignment.Center })
                g.DrawString(UiLanguage.English ? (hard ? "Hard" : "Easy") : text, f, b, box, sf);
            g.Restore(state);
        }

        static void DrawBadge(Graphics g, Rectangle b, RunRecord r)
        {
            // 外圈：进度弧（金）+ 底圈；无尽层按当前这一百层画
            var ring = Rectangle.Inflate(b, -6, -6);
            using (var p = new Pen(Theme.Line, 6f)) g.DrawEllipse(p, ring);
            var frac = r.Endless ? (r.ShownCleared % 100) / 100f : r.Shown > 0 ? Math.Min(1f, r.ShownCleared / (float)r.Shown) : 0f;
            if (frac > 0)
                using (var p = new Pen(r.Status == RunRecord.State.Lost ? Color.FromArgb(200, 60, 56) : r.Endless ? Theme.Cyan : Theme.Gold, 6f) { StartCap = LineCap.Round, EndCap = LineCap.Round })
                    g.DrawArc(p, ring, -90, 360 * frac);
            var inner = Rectangle.Inflate(b, -18, -18);
            using (var path = new GraphicsPath())
            {
                path.AddEllipse(inner);
                using (var pg = new PathGradientBrush(path) { CenterColor = Color.FromArgb(44, 39, 33), SurroundColors = new[] { Color.FromArgb(24, 22, 20) } }) g.FillPath(pg, path);
            }
            using (var p = new Pen(Color.FromArgb(60, Theme.Gold), 1f)) g.DrawEllipse(p, inner);
            var big = r.ShownCleared.ToString();
            using (var f = Theme.Title(big.Length >= 4 ? 28f : 38f))
                TextRenderer.DrawText(g, big, f, new Rectangle(inner.X, inner.Y + (big.Length >= 4 ? 20 : 14), inner.Width, 60), Theme.Paper, TextFormatFlags.HorizontalCenter);
            var under = r.Endless ? UiLanguage.T("已通过 · 无尽", "Cleared · Endless") : UiLanguage.English ? "Cleared / " + r.Shown : "已通过 / " + r.Shown + " 层";
            using (var f = Theme.Text(9f))
                TextRenderer.DrawText(g, under, f, new Rectangle(inner.X, inner.Y + 76, inner.Width, 20), Theme.Muted, TextFormatFlags.HorizontalCenter);
            if (r.Difficulty.Length > 0 && r.Status != RunRecord.State.None)
                using (var f = Theme.Text(8.5f, FontStyle.Bold))
                    TextRenderer.DrawText(g, UiLanguage.English ? (r.Difficulty == "困难" ? "Hard" : "Easy") : r.Difficulty + "难度", f, new Rectangle(inner.X, inner.Y + 94, inner.Width, 18), r.Difficulty == "困难" ? Color.FromArgb(230, 96, 86) : Theme.Cyan, TextFormatFlags.HorizontalCenter);
        }

        struct Tile { public string Label, Value, Note; public Color Accent; }

        static Tile[] TileData(RunRecord r)
        {
            var timed = r.Timed.ToList();
            var fastest = timed.OrderBy(x => x.Tenths).FirstOrDefault();
            var chal = r.Rows.Where(x => x.Challenge.Length > 0).ToList();
            return new[]
            {
                new Tile { Label = UiLanguage.T("总用时", "Total time"), Value = timed.Count > 0 ? RunRecord.Clock(r.Total) : "--:--", Note = timed.Count > 0 ? UiLanguage.T("计入 " + timed.Count + " 层战斗", timed.Count + " fights") : UiLanguage.T("打完一层后显示", "After a floor"), Accent = Theme.Gold },
                new Tile { Label = UiLanguage.T("平均每层", "Average"), Value = timed.Count > 0 ? RunRecord.Clock(r.Total / timed.Count) : "--:--", Note = UiLanguage.T("只算战斗时间", "Combat time only"), Accent = Theme.Paper },
                new Tile { Label = UiLanguage.T("最快一层", "Fastest floor"), Value = fastest != null ? RunRecord.Clock(fastest.Tenths) : "--:--", Note = fastest != null ? UiLanguage.T("第 " + r.ShownFloor(fastest.Floor) + " 层", "Floor " + r.ShownFloor(fastest.Floor)) + (fastest.Boss.Length > 0 ? " · " + RunRecord.Names(fastest.Boss.Split('|')[0]) : "") : "", Accent = Theme.Cyan },
                chal.Count > 0
                    ? new Tile { Label = UiLanguage.T("挑战达成", "Challenges"), Value = chal.Count(x => x.ChallengeDone) + " / " + chal.Count, Note = UiLanguage.T("限时、无伤、禁药等", "Time, no hit, no medicine"), Accent = Color.FromArgb(220, 120, 70) }
                    : new Tile { Label = UiLanguage.T("魂玉", "Soul Jade"), Value = r.Jade >= 0 ? r.Jade.ToString() : "--", Note = UiLanguage.T("当前持有", "Held"), Accent = Color.FromArgb(220, 120, 70) },
            };
        }

        static void DrawTile(Graphics g, Rectangle rect, Tile t)
        {
            using (var path = Theme.Round(rect, 10))
            {
                using (var b = new LinearGradientBrush(rect, Color.FromArgb(36, 33, 29), Color.FromArgb(26, 24, 22), 90f)) g.FillPath(b, path);
                using (var p = new Pen(Theme.Line)) g.DrawPath(p, path);
            }
            using (var p = new Pen(t.Accent, 3f) { StartCap = LineCap.Round, EndCap = LineCap.Round }) g.DrawLine(p, rect.X + 16, rect.Y + 1.5f, rect.X + 46, rect.Y + 1.5f);
            using (var f = Theme.Text(9f)) TextRenderer.DrawText(g, t.Label, f, new Point(rect.X + 14, rect.Y + 16), Theme.Muted);
            var vf = t.Value.Length > 7 ? 15f : 19f;
            using (var f = Theme.Text(vf, FontStyle.Bold)) TextRenderer.DrawText(g, t.Value, f, new Rectangle(rect.X + 12, rect.Y + 44, rect.Width - 20, 40), t.Accent, TextFormatFlags.SingleLine | TextFormatFlags.EndEllipsis);
            using (var f = Theme.Text(8.5f)) TextRenderer.DrawText(g, t.Note, f, new Rectangle(rect.X + 14, rect.Y + 98, rect.Width - 22, 22), Color.FromArgb(120, 112, 102), TextFormatFlags.SingleLine | TextFormatFlags.EndEllipsis);
        }

        // 本页第 i 格（0 起）
        Rectangle Cell(Rectangle strip, int i)
        {
            float gap = 2f, w = (strip.Width - gap * (PageCells - 1)) / PageCells;
            var x = strip.X + i * (w + gap);
            return Rectangle.Round(new RectangleF(x, strip.Y + 6, w, strip.Height - 12));
        }
        int CellsOnPage { get { return Math.Max(0, Math.Min(PageCells, _rec.Cells - Page * PageCells)); } }

        void DrawStrip(Graphics g, Rectangle strip, RunRecord r)
        {
            var first = Page * PageCells;   // 本页第一格的内部层号 - 1
            var n = CellsOnPage;
            var byFloor = new Dictionary<int, FloorRow>();
            foreach (var x in r.Rows) byFloor[x.Floor] = x;
            for (var i = 0; i < PageCells; i++)
            {
                var c = Cell(strip, i);
                var f = first + i + 1;
                FloorRow row;
                byFloor.TryGetValue(f, out row);
                Color fill, edge;
                if (i >= n) { fill = Color.FromArgb(22, 21, 19); edge = Color.FromArgb(40, 37, 33); }  // 这一页还没开到的格子
                else if (row != null && row.Lost) { fill = Color.FromArgb(200, 56, 52); edge = Color.FromArgb(240, 110, 100); }
                else if (f <= r.Cleared) { fill = row != null && row.Tenths > 0 ? (f > 50 ? Theme.Cyan : Theme.Gold) : Color.FromArgb(120, 96, 50); edge = Color.FromArgb(255, 214, 120); }
                else if (f == r.Cleared + 1 && r.Status == RunRecord.State.Running) { fill = Color.FromArgb(40, Theme.Cyan); edge = Theme.Cyan; }
                else { fill = Color.FromArgb(34, 31, 28); edge = Theme.Line; }
                using (var path = Theme.Round(c, 1.5f))
                {
                    using (var b = new SolidBrush(fill)) g.FillPath(b, path);
                    using (var p = new Pen(i == _hoverCell ? Theme.Paper : Color.FromArgb(f <= r.Cleared ? 120 : 255, edge), i == _hoverCell ? 1.6f : 1f)) g.DrawPath(p, path);
                }
                if ((i + 1) % 10 == 0 && i + 1 < PageCells)  // 每十格一道分隔
                    using (var p = new Pen(Color.FromArgb(90, Theme.Crimson), 1.5f)) g.DrawLine(p, c.Right + 1f, strip.Y, c.Right + 1f, strip.Bottom);
            }
            using (var f = Theme.Text(8f))
                for (var z = 0; z < 10; z++)
                {
                    var c = Cell(strip, z * 10);
                    TextRenderer.DrawText(g, r.ShownFloor(first + z * 10 + 1).ToString(), f, new Point(c.X - 2, strip.Y - 12), Color.FromArgb(100, 94, 86));
                }
            // 翻页（只有一页时不画）
            _pagePrev = _pageNext = Rectangle.Empty;
            if (Pages > 1)
                using (var f = Theme.Text(9f, FontStyle.Bold))
                {
                    var label = UiLanguage.T("第 " + (Page + 1) + " / " + Pages + " 页", "Page " + (Page + 1) + " / " + Pages);
                    var sz = TextRenderer.MeasureText(g, label, f);
                    _pageNext = new Rectangle(strip.Right - 22, strip.Y - 22, 22, 18);
                    var mid = new Rectangle(_pageNext.X - sz.Width - 6, strip.Y - 22, sz.Width + 6, 18);
                    _pagePrev = new Rectangle(mid.X - 22, strip.Y - 22, 22, 18);
                    TextRenderer.DrawText(g, "‹", f, _pagePrev, Page > 0 ? Theme.Gold : Theme.Line, TextFormatFlags.HorizontalCenter | TextFormatFlags.VerticalCenter);
                    TextRenderer.DrawText(g, label, f, mid, Theme.Paper, TextFormatFlags.HorizontalCenter | TextFormatFlags.VerticalCenter);
                    TextRenderer.DrawText(g, "›", f, _pageNext, Page < Pages - 1 ? Theme.Gold : Theme.Line, TextFormatFlags.HorizontalCenter | TextFormatFlags.VerticalCenter);
                }
            if (_hoverCell >= 0 && _hoverCell < n)
            {
                FloorRow row;
                byFloor.TryGetValue(first + _hoverCell + 1, out row);
                var shown = row != null ? r.ShownOf(row) : r.ShownFloor(first + _hoverCell + 1);
                var text = UiLanguage.T("第 " + shown + " 层", "Floor " + shown) + (row == null ? (first + _hoverCell + 1 <= r.Cleared ? "" : UiLanguage.T("　未到达", " · Not reached"))
                    : (row.Boss.Length > 0 ? " · " + RunRecord.Names(row.Boss.Replace("|", "、")) : "") + " · " + (row.Lost ? UiLanguage.T("倒在此层", "Defeated") : row.Tenths > 0 ? RunRecord.Clock(row.Tenths) : UiLanguage.T("跳过", "Skipped")));
                using (var f = Theme.Text(8.5f, FontStyle.Bold))
                {
                    var size = TextRenderer.MeasureText(g, text, f);
                    var c = Cell(strip, _hoverCell);
                    var w = Math.Min(size.Width + 16, strip.Width);
                    var x = Math.Max(strip.X, Math.Min(strip.Right - w, c.X + c.Width / 2 - w / 2));
                    var rect = new Rectangle(x, strip.Bottom + 2, w, 22);
                    using (var path = Theme.Round(rect, 5))
                    {
                        using (var b = new SolidBrush(Color.FromArgb(240, 44, 40, 36))) g.FillPath(b, path);
                        using (var p = new Pen(Theme.Gold)) g.DrawPath(p, path);
                    }
                    TextRenderer.DrawText(g, text, f, new Rectangle(rect.X + 8, rect.Y + 3, rect.Width - 12, 18), Theme.Paper, TextFormatFlags.SingleLine | TextFormatFlags.EndEllipsis);
                }
            }
        }

        protected override void OnMouseMove(MouseEventArgs e)
        {
            var hover = -1;
            var n = CellsOnPage;
            if (_strip.Contains(e.Location))
                for (var i = 0; i < n; i++) { var c = Cell(_strip, i); if (e.X >= c.X - 1 && e.X <= c.Right + 1) { hover = i; break; } }
            if (hover != _hoverCell) { _hoverCell = hover; Invalidate(new Rectangle(0, _strip.Y - 24, Width, _strip.Height + 52)); }
            var hand = _pagePrev.Contains(e.Location) || _pageNext.Contains(e.Location) || _hexPrev.Contains(e.Location) || _hexNext.Contains(e.Location) || _seedRect.Contains(e.Location);
            Cursor = hand ? Cursors.Hand : Cursors.Default;
            base.OnMouseMove(e);
        }

        protected override void OnMouseLeave(EventArgs e) { if (_hoverCell >= 0) { _hoverCell = -1; Invalidate(); } base.OnMouseLeave(e); }

        // 翻页 / 复制种子 / 拖动无边框窗口（标题区）
        protected override void OnMouseDown(MouseEventArgs e)
        {
            if (e.Button == MouseButtons.Left)
            {
                if (_pagePrev.Contains(e.Location) && Page > 0) { _page = Page - 1; _hoverCell = -1; Invalidate(); return; }
                if (_pageNext.Contains(e.Location) && Page < Pages - 1) { _page = Page + 1; _hoverCell = -1; Invalidate(); return; }
                if (_hexPrev.Contains(e.Location) && HexPage > 0) { _hexPage = HexPage - 1; Invalidate(_hexBox); return; }
                if (_hexNext.Contains(e.Location) && HexPage < HexPages - 1) { _hexPage = HexPage + 1; Invalidate(_hexBox); return; }
                if (_seedRect.Contains(e.Location) && _rec.Seed.Length > 0)
                {
                    try { Clipboard.SetText(_rec.Seed); _copied = _rec.Seed; } catch (System.Runtime.InteropServices.ExternalException) { }
                    Invalidate(_hexBox); return;
                }
                if (e.Y < 64 && e.X < _hexBox.X) { NativeDrag.Begin(Handle); }
            }
            base.OnMouseDown(e);
        }
    }

    static class NativeDrag
    {
        [System.Runtime.InteropServices.DllImport("user32.dll")] static extern bool ReleaseCapture();
        [System.Runtime.InteropServices.DllImport("user32.dll")] static extern IntPtr SendMessage(IntPtr h, int msg, int w, int l);
        public static void Begin(IntPtr h) { ReleaseCapture(); SendMessage(h, 0xA1, 2, 0); }
    }

    // 逐层列表：自绘、滚轮滚动、可拖动的细滚动条；无尽层一行按波分成多行
    sealed class FloorList : Control
    {
        const int RowH = 46, WaveH = 20, BarW = 12;
        RunRecord _rec;
        List<FloorRow> _rows = new List<FloorRow>();
        readonly List<int> _top = new List<int>();   // 每行顶部的 y（不含滚动）
        int _scroll, _hover = -1, _maxTenths = 1, _total = 8;
        int _dragFrom = -1, _dragScroll;

        public FloorList()
        {
            SetStyle(ControlStyles.AllPaintingInWmPaint | ControlStyles.OptimizedDoubleBuffer | ControlStyles.UserPaint | ControlStyles.Selectable, true);
            BackColor = Theme.Panel;
        }

        static int RowHeight(FloorRow r) { var w = r.Waves; return w == null ? RowH : Math.Max(RowH, 12 + w.Length * WaveH); }

        public void Set(RunRecord r)
        {
            _rec = r;
            _rows = r.Rows.OrderByDescending(x => x.Floor).ToList();  // 最新的在上
            _maxTenths = Math.Max(1, r.Rows.Select(x => x.Tenths).DefaultIfEmpty(1).Max());
            _top.Clear();
            var y = 4;
            foreach (var row in _rows) { _top.Add(y); y += RowHeight(row); }
            _total = y + 4;
            _scroll = Math.Max(0, Math.Min(_scroll, MaxScroll));
            Invalidate();
        }

        int MaxScroll { get { return Math.Max(0, _total - Height); } }
        new int Height { get { return ClientSize.Height; } }

        RectangleF Thumb()
        {
            var h = Math.Max(36f, Height * (float)Height / Math.Max(1, _total));
            var y = MaxScroll > 0 ? (Height - h) * _scroll / MaxScroll : 0;
            return new RectangleF(Width - BarW, y, BarW, h);
        }

        protected override void OnMouseWheel(MouseEventArgs e)
        {
            _scroll = Math.Max(0, Math.Min(MaxScroll, _scroll - e.Delta / 120 * RowH));
            Invalidate();
            base.OnMouseWheel(e);
        }

        protected override void OnMouseEnter(EventArgs e) { Focus(); base.OnMouseEnter(e); }
        protected override void OnMouseDown(MouseEventArgs e)
        {
            if (e.Button == MouseButtons.Left && MaxScroll > 0 && e.X >= Width - BarW - 4)
            {
                var t = Thumb();
                if (e.Y < t.Y || e.Y > t.Bottom)  // 点在轨道上：滑块中心跳到那里
                    _scroll = Math.Max(0, Math.Min(MaxScroll, (int)((e.Y - t.Height / 2) / Math.Max(1f, Height - t.Height) * MaxScroll)));
                _dragFrom = e.Y; _dragScroll = _scroll; Capture = true; Invalidate();
            }
            base.OnMouseDown(e);
        }
        protected override void OnMouseUp(MouseEventArgs e) { _dragFrom = -1; Capture = false; base.OnMouseUp(e); }
        protected override void OnMouseMove(MouseEventArgs e)
        {
            if (_dragFrom >= 0)
            {
                var t = Thumb();
                var track = Math.Max(1f, Height - t.Height);
                _scroll = Math.Max(0, Math.Min(MaxScroll, _dragScroll + (int)((e.Y - _dragFrom) / track * MaxScroll)));
                Invalidate();
                return;
            }
            var h = -1;
            var y = e.Y + _scroll;
            for (var i = 0; i < _rows.Count; i++) if (y >= _top[i] && y < _top[i] + RowHeight(_rows[i])) { h = i; break; }
            if (h != _hover) { _hover = h; Invalidate(); }
            Cursor = MaxScroll > 0 && e.X >= Width - BarW - 4 ? Cursors.Hand : Cursors.Default;
            base.OnMouseMove(e);
        }
        protected override void OnMouseLeave(EventArgs e) { _hover = -1; Invalidate(); base.OnMouseLeave(e); }

        protected override void OnPaint(PaintEventArgs e)
        {
            var g = e.Graphics;
            g.SmoothingMode = SmoothingMode.AntiAlias;
            g.TextRenderingHint = TextRenderingHint.AntiAliasGridFit;
            using (var path = Theme.Round(new RectangleF(0, 0, Width - 1, Height - 1), 10))
            {
                using (var b = new SolidBrush(Theme.Panel)) g.FillPath(b, path);
                using (var p = new Pen(Theme.Line)) g.DrawPath(p, path);
                g.SetClip(path);
            }
            if (_rows.Count == 0)
            {
                using (var f = Theme.Title(15f)) TextRenderer.DrawText(g, UiLanguage.T("尚无战绩", "No records yet"), f, new Rectangle(0, Height / 2 - 34, Width, 30), Theme.Muted, TextFormatFlags.HorizontalCenter);
                using (var f = Theme.Text(9.5f)) TextRenderer.DrawText(g, UiLanguage.T("进入游戏开一局影塔，打完一层这里就会出现记录", "Start a tower run and clear a floor to see records here."), f, new Rectangle(0, Height / 2 + 2, Width, 24), Color.FromArgb(110, 102, 92), TextFormatFlags.HorizontalCenter);
                return;
            }
            int nameX = 112, barX = Width - 390, barW = 170, timeX = Width - 206;
            using (var fFloor = Theme.Title(13f))
            using (var fName = Theme.Text(10.5f, FontStyle.Bold))
            using (var fWave = Theme.Text(9.5f, FontStyle.Bold))
            using (var fSmall = Theme.Text(8.5f))
            using (var fTime = Theme.Text(11f, FontStyle.Bold))
                for (var i = 0; i < _rows.Count; i++)
                {
                    var row = _rows[i];
                    var rh = RowHeight(row);
                    var y = _top[i] - _scroll;
                    if (y + rh < 0 || y > Height) continue;
                    var rect = new Rectangle(6, y, Width - 22, rh - 4);
                    int best;
                    var hasBest = _rec.Best.TryGetValue(row.Floor, out best);
                    var isBest = hasBest && row.Tenths > 0 && row.Tenths <= best && !row.Lost;
                    using (var path = Theme.Round(rect, 7))
                    {
                        var bg = row.Lost ? Color.FromArgb(50, 160, 32, 36) : i == _hover ? Color.FromArgb(40, 37, 33) : i % 2 == 0 ? Color.FromArgb(33, 31, 28) : Color.FromArgb(29, 27, 25);
                        using (var b = new SolidBrush(bg)) g.FillPath(b, path);
                        if (i == _hover || row.Lost) using (var p = new Pen(row.Lost ? Color.FromArgb(150, 200, 60, 56) : Color.FromArgb(90, Theme.Gold))) g.DrawPath(p, path);
                    }
                    // 左侧色条
                    var tag = row.Lost ? Color.FromArgb(220, 70, 64) : row.Tenths > 0 ? (row.Floor > 50 ? Theme.Cyan : Theme.Gold) : Theme.Line;
                    using (var b = new SolidBrush(tag)) g.FillRectangle(b, rect.X, rect.Y + 9, 3, rect.Height - 18);
                    // 层号
                    TextRenderer.DrawText(g, UiLanguage.English ? "Floor " + _rec.ShownOf(row) : "第 " + _rec.ShownOf(row) + " 层", fFloor, new Rectangle(rect.X + 16, rect.Y, 96, rect.Height), row.Lost ? Color.FromArgb(240, 120, 110) : Theme.Paper, TextFormatFlags.VerticalCenter | TextFormatFlags.SingleLine);
                    // 挑战标签
                    var nameW = barX - nameX - 16;
                    var chalW = 0;
                    if (row.Challenge.Length > 0)
                    {
                        var raw = row.Challenge.Replace("额外挑战：", "");
                        var label = (UiLanguage.English ? (raw == "限时" ? "Timed" : raw == "无伤" ? "No hit" : raw == "禁药" ? "No medicine" : raw) : raw) + (row.ChallengeDone ? " ✓" : " ✗");
                        var sz = TextRenderer.MeasureText(g, label, fSmall);
                        chalW = sz.Width + 16;
                        var cr = new Rectangle(barX - 16 - chalW, rect.Y + (rect.Height - 20) / 2, chalW, 20);
                        var cc = row.ChallengeDone ? Theme.Cyan : Theme.Muted;
                        using (var cp = Theme.Round(cr, 10))
                        {
                            using (var b = new SolidBrush(Color.FromArgb(30, cc))) g.FillPath(b, cp);
                            using (var p = new Pen(Color.FromArgb(140, cc))) g.DrawPath(p, cp);
                        }
                        TextRenderer.DrawText(g, label, fSmall, cr, cc, TextFormatFlags.HorizontalCenter | TextFormatFlags.VerticalCenter | TextFormatFlags.SingleLine);
                    }
                    var waves = row.Waves;
                    if (waves != null)
                    {
                        // 无尽混编层：一波一行（每层最多 3 波、每波最多 3 个 Boss）
                        var wy = rect.Y + (rect.Height - waves.Length * WaveH) / 2;
                        for (var w = 0; w < waves.Length; w++, wy += WaveH)
                        {
                            TextRenderer.DrawText(g, UiLanguage.T("第" + (w + 1) + "波", "W" + (w + 1)), fSmall, new Rectangle(rect.X + nameX, wy, 40, WaveH), Theme.Cyan, TextFormatFlags.VerticalCenter | TextFormatFlags.SingleLine);
                            TextRenderer.DrawText(g, RunRecord.Names(waves[w]), fWave, new Rectangle(rect.X + nameX + 42, wy, nameW - chalW - 50, WaveH),
                                Color.FromArgb(222, 212, 196), TextFormatFlags.VerticalCenter | TextFormatFlags.SingleLine | TextFormatFlags.EndEllipsis);
                        }
                    }
                    else
                    {
                        var name = row.Boss.Length > 0 ? RunRecord.Names(row.Boss) : row.Tenths > 0 || row.Lost ? "Boss" : UiLanguage.T("跳过", "Skipped");
                        TextRenderer.DrawText(g, name, fName, new Rectangle(rect.X + nameX, rect.Y, nameW - chalW - 8, rect.Height),
                            row.Boss.Length > 0 ? Color.FromArgb(222, 212, 196) : Theme.Muted, TextFormatFlags.VerticalCenter | TextFormatFlags.SingleLine | TextFormatFlags.EndEllipsis);
                    }
                    // 用时条
                    var track = new RectangleF(barX, rect.Y + rect.Height / 2f - 3, barW, 6);
                    using (var tp = Theme.Round(track, 3)) using (var b = new SolidBrush(Color.FromArgb(44, 40, 36))) g.FillPath(b, tp);
                    if (row.Tenths > 0)
                    {
                        var w = Math.Max(6f, barW * row.Tenths / (float)_maxTenths);
                        var fill = new RectangleF(barX, track.Y, w, 6);
                        using (var fp = Theme.Round(fill, 3))
                        using (var b = new LinearGradientBrush(new RectangleF(barX, track.Y, barW, 6), row.Lost ? Color.FromArgb(150, 40, 40) : Color.FromArgb(170, 120, 30), row.Lost ? Color.FromArgb(230, 90, 80) : Color.FromArgb(255, 204, 92), 0f))
                            g.FillPath(b, fp);
                    }
                    // 用时 + 最佳
                    var time = row.Lost ? UiLanguage.T("殒命", "Defeated") : row.Tenths > 0 ? RunRecord.Clock(row.Tenths) : "—";
                    TextRenderer.DrawText(g, time, fTime, new Rectangle(timeX, rect.Y, 96, rect.Height), row.Lost ? Color.FromArgb(240, 110, 100) : row.Tenths > 0 ? Theme.Paper : Theme.Muted, TextFormatFlags.VerticalCenter | TextFormatFlags.SingleLine);
                    var bestText = isBest ? UiLanguage.T("★ 最佳", "★ Best") : hasBest ? UiLanguage.T("最佳 ", "Best ") + RunRecord.Clock(best) : "";
                    TextRenderer.DrawText(g, bestText, fSmall, new Rectangle(timeX + 92, rect.Y, Width - timeX - 106, rect.Height), isBest ? Theme.Gold : Color.FromArgb(110, 102, 92), TextFormatFlags.VerticalCenter | TextFormatFlags.SingleLine);
                }
            // 可拖动的滚动条
            g.ResetClip();
            if (MaxScroll > 0)
            {
                using (var tp = Theme.Round(new RectangleF(Width - BarW + 4, 6, 4, Height - 12), 2f)) using (var b = new SolidBrush(Color.FromArgb(40, Theme.Gold))) g.FillPath(b, tp);
                var t = Thumb();
                using (var tp = Theme.Round(new RectangleF(t.X + 3, t.Y + 4, 6, t.Height - 8), 3f)) using (var b = new SolidBrush(Color.FromArgb(_dragFrom >= 0 ? 230 : 160, Theme.Gold))) g.FillPath(b, tp);
            }
        }
    }
}
