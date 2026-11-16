using System;
using System.Collections.Generic;
using System.Drawing;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Text;
using System.Windows.Forms;

namespace PBZTowerSetup
{
    // 3.5.0 无尽档（用户 2026-11-14）：把无尽存档点（显示 100 / 120 / 140 …）的整局状态导出给别人；导入后在游戏「无尽模式」开局。
    // 文件格式与游戏 tower_endless_share.h 完全一致（统一换行 + FNV-1a 64 校验）。导入只放进 endless-import.txt，不改正在玩的局。
    sealed class EndlessForm : Form
    {
        const string Magic = "pbztower-endless-1";
        static string DataDir { get { return ModFiles.TowerDataDir; } }
        static string T(string zh, string en) { return UiLanguage.T(zh, en); }

        sealed class Point_ { public int Shown, Difficulty, Jade, Weapons; public string RunPath, GrowthPath, Id; public bool Current; public DateTime When; }
        readonly ListBox _list = new ListBox();
        readonly TextBox _author = new TextBox();
        readonly Label _info = new Label(), _imported = new Label();
        readonly List<Point_> _points = new List<Point_>();

        public EndlessForm()
        {
            Text = T("无尽档", "Endless save");
            StartPosition = FormStartPosition.CenterParent; ClientSize = new Size(600, 470);
            BackColor = Theme.Ink; ForeColor = Theme.Paper; Font = Theme.Text(10f);
            FormBorderStyle = FormBorderStyle.FixedDialog; MaximizeBox = false; MinimizeBox = false;
            _info.SetBounds(24, 16, 552, 62);
            _info.Text = T("无尽存档点（第 100、120、140 … 层）可以随时导出成文件：自己死了可以导回来从那一层重开，也可以分享给别人。导入后在游戏「无尽模式」开局，之后的 Boss、海克斯和驿站各自随机。",
                           "Export any Endless checkpoint (floor 100, 120, 140…) as a file: re-import it to restart from that floor, or share it. Later bosses, hexes and shops stay random.");
            Controls.Add(_info);
            var exportCap = new Label { Text = T("导出（本机无尽存档点）", "Export (your checkpoints)"), Left = 24, Top = 82, Width = 552, Height = 22, Font = Theme.Text(10f, FontStyle.Bold) };
            Controls.Add(exportCap);
            _list.SetBounds(24, 106, 552, 128);
            _list.BackColor = Theme.Ink; _list.ForeColor = Theme.Paper; _list.BorderStyle = BorderStyle.FixedSingle;
            Controls.Add(_list);
            var authorCap = new Label { Text = T("作者昵称（可不填）", "Author (optional)"), Left = 24, Top = 244, Width = 150, Height = 24 };
            Controls.Add(authorCap);
            _author.SetBounds(176, 240, 220, 26); _author.MaxLength = 20;
            Controls.Add(_author);
            var export = new Button { Text = T("导出选中", "Export"), Left = 410, Top = 238, Width = 166, Height = 32 };
            export.Click += (s, e) => Run(Export);
            Controls.Add(export);

            var importCap = new Label { Text = T("导入", "Import"), Left = 24, Top = 290, Width = 552, Height = 22, Font = Theme.Text(10f, FontStyle.Bold) };
            Controls.Add(importCap);
            _imported.SetBounds(24, 314, 552, 64);
            Controls.Add(_imported);
            var import = new Button { Text = T("选择文件导入", "Import file"), Left = 24, Top = 392, Width = 170, Height = 40 };
            var clear = new Button { Text = T("清除已导入", "Clear import"), Left = 210, Top = 392, Width = 170, Height = 40 };
            var close = new Button { Text = T("关闭", "Close"), Left = 406, Top = 392, Width = 170, Height = 40 };
            import.Click += (s, e) => Run(Import);
            clear.Click += (s, e) => Run(() => { var p = Path.Combine(DataDir, "endless-import.txt"); if (File.Exists(p)) File.Delete(p); Refresh_(); });
            close.Click += (s, e) => Close();
            Controls.AddRange(new Control[] { import, clear, close });
            Refresh_();
        }
        void Run(Action action) { try { action(); } catch (Exception ex) { MessageBox.Show(this, ex.Message, Text); } }

        static Dictionary<string, string> Fields(string text)
        {
            var d = new Dictionary<string, string>(StringComparer.Ordinal);
            foreach (var line in (text ?? "").Replace("\r", "").Split('\n')) { int p = line.IndexOf('='); if (p > 0 && !d.ContainsKey(line.Substring(0, p))) d[line.Substring(0, p)] = line.Substring(p + 1); }
            return d;
        }
        static string Read(string path) { try { return File.Exists(path) ? File.ReadAllText(path, Encoding.UTF8) : null; } catch (IOException) { return null; } catch (UnauthorizedAccessException) { return null; } }
        static int Int(Dictionary<string, string> f, string k) { string v; int n; return f.TryGetValue(k, out v) && int.TryParse(v.Trim(), NumberStyles.Integer, CultureInfo.InvariantCulture, out n) ? n : 0; }
        static string Str(Dictionary<string, string> f, string k) { string v; return f.TryGetValue(k, out v) ? v.Trim() : ""; }
        static int ShownBoss(int floor) { return floor > 50 ? floor + 49 : 2 * floor - 1; }

        // 无尽存档：当前局（run.txt）或停放的（endless-run.txt）里是无尽存档的那一份
        static Dictionary<string, string> EndlessRun()
        {
            foreach (var leaf in new[] { "run.txt", "endless-run.txt" })
            {
                var f = Fields(Read(Path.Combine(DataDir, leaf)));
                if (Str(f, "eslot") == "1" || (Str(f, "endless") == "1" && Int(f, "floor") >= 51)) return f;
            }
            return null;
        }

        void Refresh_()
        {
            _points.Clear(); _list.Items.Clear();
            var run = EndlessRun();
            // 用户：存档点就是记录档——死了放弃后也能导出来重开，所以列出本机所有无尽存档点（不只当前这一局），并标出当前无尽存档的
            if (Directory.Exists(DataDir))
            {
                var id = run != null ? Str(run, "id") : "";
                var files = Directory.GetFiles(DataDir, "checkpoint-*-run.txt").ToList();
                var legacy = Path.Combine(DataDir, "checkpoint-run.txt");
                if (File.Exists(legacy)) files.Add(legacy);  // 旧版的单一存档点（第 100 / 200 层），属于这一局就列出
                foreach (var rp in files)
                {
                    var f = Fields(Read(rp));
                    if (Str(f, "endless") != "1") continue;
                    int shown = ShownBoss(Int(f, "floor") + 1);
                    var rid = Str(f, "id");
                    if (shown < 100 || shown % 20 != 0 || _points.Any(x => x.Shown == shown && x.Id == rid)) continue;
                    var gp = rp.EndsWith("checkpoint-run.txt") ? Path.Combine(DataDir, "checkpoint-growth.txt") : rp.Substring(0, rp.Length - "-run.txt".Length) + "-growth.txt";
                    var g = Fields(Read(gp));
                    var w = Str(g, "g.weapons");
                    if (!File.Exists(gp)) continue;
                    _points.Add(new Point_ { Shown = shown, Difficulty = Int(f, "diff"), Jade = Int(f, "jade"), Weapons = w.Length == 0 ? 0 : w.Split(';').Length, RunPath = rp, GrowthPath = gp, Id = rid, Current = rid.Length > 0 && rid == id, When = File.GetLastWriteTime(rp) });
                }
            }
            _points.Sort((a, b) => a.Current != b.Current ? (a.Current ? -1 : 1) : a.When != b.When ? b.When.CompareTo(a.When) : b.Shown.CompareTo(a.Shown));
            foreach (var p in _points)
                _list.Items.Add(T(string.Format("{0}第 {1} 层　·　{2}　·　魂玉 {3}　·　兵器 {4}　·　{5:MM-dd HH:mm}", p.Current ? "【当前】" : "", p.Shown, p.Difficulty == 1 ? "简单" : "困难", p.Jade, p.Weapons, p.When),
                                  string.Format("{0}Floor {1}  ·  {2}  ·  Jade {3}  ·  Weapons {4}  ·  {5:MM-dd HH:mm}", p.Current ? "[Current] " : "", p.Shown, p.Difficulty == 1 ? "Easy" : "Hard", p.Jade, p.Weapons, p.When)));
            if (_points.Count == 0) _list.Items.Add(T("还没有无尽存档点：打完第 99 层进入无尽模式后，第 100 层起每 20 层记一个。", "No endless checkpoints yet."));
            else _list.SelectedIndex = 0;

            Share sh; string why;
            var text = Read(Path.Combine(DataDir, "endless-import.txt"));
            if (text != null && Parse(text, out sh, out why))
                _imported.Text = T(string.Format("已就绪：{0} · 第 {1} 层起 · {2}\n在游戏里打开影塔菜单，选「无尽模式」→「用导入的无尽档开局」。开局后会覆盖你现有的无尽存档。", sh.Author.Length > 0 ? sh.Author : "佚名", sh.Shown, sh.Difficulty == 1 ? "简单" : "困难"),
                                   string.Format("Ready: {0} · from floor {1} · {2}\nIn game choose Endless Mode → start from the imported save. It replaces your current endless save.", sh.Author.Length > 0 ? sh.Author : "Anonymous", sh.Shown, sh.Difficulty == 1 ? "Easy" : "Hard"));
            else _imported.Text = T("没有已导入的无尽档。", "No endless save imported.");
        }

        void Export()
        {
            if (_list.SelectedIndex < 0 || _list.SelectedIndex >= _points.Count) throw new InvalidOperationException(T("先在上面选一个存档点。", "Pick a checkpoint first."));
            var p = _points[_list.SelectedIndex];
            var sh = new Share { Author = _author.Text.Trim(), Shown = p.Shown, Difficulty = p.Difficulty, Mod = ModFiles.Version, Run = File.ReadAllText(p.RunPath, Encoding.UTF8), Growth = File.ReadAllText(p.GrowthPath, Encoding.UTF8) };
            using (var d = new SaveFileDialog { Filter = T("影塔无尽档|*.txt", "Endless save|*.txt"), FileName = T("影塔无尽档-第" + p.Shown + "层.txt", "PhantomTower-Endless-F" + p.Shown + ".txt") })
            {
                if (d.ShowDialog(this) != DialogResult.OK) return;
                File.WriteAllText(d.FileName, Build(sh), new UTF8Encoding(false));
                MessageBox.Show(this, T("已导出：" + d.FileName, "Exported: " + d.FileName), Text);
            }
        }

        void Import()
        {
            using (var d = new OpenFileDialog { Filter = T("影塔无尽档|*.txt|所有文件|*.*", "Endless save|*.txt|All files|*.*") })
            {
                if (d.ShowDialog(this) != DialogResult.OK) return;
                var f = new FileInfo(d.FileName);
                if (!f.Exists || f.Length > 512 * 1024) throw new InvalidDataException(T("文件不存在或过大。", "File missing or too large."));
                var text = File.ReadAllText(d.FileName, Encoding.UTF8);
                Share sh; string why;
                if (!Parse(text, out sh, out why)) throw new InvalidDataException(why);
                if (EndlessRun() != null && MessageBox.Show(this, T("你已经有一个无尽存档。在游戏里用这个导入档开局时会覆盖它（游戏里还会再确认一次）。\n建议先在上方把自己的存档点导出备份，以后可以再导回来。\n继续导入吗？", "You already have an endless save. Starting from this import in game will replace it. Export your own checkpoint first as a backup. Continue?"), Text, MessageBoxButtons.YesNo) != DialogResult.Yes) return;
                Directory.CreateDirectory(DataDir);
                File.WriteAllText(Path.Combine(DataDir, "endless-import.txt"), Build(sh), new UTF8Encoding(false));
                Refresh_();
            }
        }

        // ---- 与 tower_endless_share.h 相同的规则 ----
        sealed class Share { public string Author = "", Mod = "", Run = "", Growth = ""; public int Shown, Difficulty; }
        static bool Marker(string l) { return l == "[run]" || l == "[growth]" || l == "[end]"; }
        static string Normalize(string t)
        {
            var sb = new StringBuilder();
            foreach (var line in (t ?? "").Replace("\r", "").Split('\n')) if (line.Length > 0 && !Marker(line)) sb.Append(line).Append('\n');
            return sb.ToString();
        }
        static string Check(string run, string growth)
        {
            ulong h = 1469598103934665603UL;
            foreach (var b in Encoding.UTF8.GetBytes(run + "\n--\n" + growth)) unchecked { h ^= b; h *= 1099511628211UL; }
            return h.ToString("x16");
        }
        static string Build(Share s)
        {
            var run = Normalize(s.Run); var growth = Normalize(s.Growth);
            var author = (s.Author ?? "").Replace("\r", " ").Replace("\n", " ");
            if (author.Length > 40) author = author.Substring(0, 40);
            var sb = new StringBuilder();
            sb.Append(Magic).Append('\n').Append("author=").Append(author).Append('\n');
            sb.Append("shown=").Append(s.Shown).Append('\n').Append("difficulty=").Append(s.Difficulty).Append('\n');
            sb.Append("mod=").Append(s.Mod).Append('\n').Append("check=").Append(Check(run, growth)).Append('\n');
            sb.Append("[run]\n").Append(run).Append("[growth]\n").Append(growth).Append("[end]\n");
            return sb.ToString();
        }
        static bool Parse(string text, out Share s, out string why)
        {
            s = new Share(); why = "";
            var lines = (text ?? "").TrimStart('﻿').Replace("\r", "").Split('\n');
            if (lines.Length == 0 || lines[0] != Magic) { why = T("不是影塔无尽档，或格式版本不对。", "Not a Phantom Tower endless save."); return false; }
            int section = 0; string check = ""; bool shown = false, diff = false;
            var run = new StringBuilder(); var growth = new StringBuilder();
            for (int i = 1; i < lines.Length; i++)
            {
                var l = lines[i];
                if (l == "[run]") { if (section != 0) { why = T("无尽档段落顺序不对。", "Bad section order."); return false; } section = 1; continue; }
                if (l == "[growth]") { if (section != 1) { why = T("无尽档段落顺序不对。", "Bad section order."); return false; } section = 2; continue; }
                if (l == "[end]") { if (section != 2) { why = T("无尽档段落顺序不对。", "Bad section order."); return false; } section = 3; break; }
                if (section == 1) { if (l.Length > 0) run.Append(l).Append('\n'); continue; }
                if (section == 2) { if (l.Length > 0) growth.Append(l).Append('\n'); continue; }
                int eq = l.IndexOf('='); if (eq <= 0) continue;
                var k = l.Substring(0, eq); var v = l.Substring(eq + 1); int n;
                if (k == "author") s.Author = v.Length > 40 ? v.Substring(0, 40) : v;
                else if (k == "shown" && int.TryParse(v, out n)) { s.Shown = n; shown = true; }
                else if (k == "difficulty" && int.TryParse(v, out n)) { s.Difficulty = n; diff = true; }
                else if (k == "mod") s.Mod = v;
                else if (k == "check") check = v;
            }
            s.Run = run.ToString(); s.Growth = growth.ToString();
            if (section != 3) { why = T("无尽档不完整。", "Incomplete endless save."); return false; }
            if (!shown || s.Shown < 100 || s.Shown % 20 != 0) { why = T("无尽档的起始层无效。", "Invalid start floor."); return false; }
            if (!diff || (s.Difficulty != 0 && s.Difficulty != 1)) { why = T("无尽档的难度无效。", "Invalid difficulty."); return false; }
            if (s.Run.Length == 0 || s.Growth.Length == 0) { why = T("无尽档缺少局档或成长档。", "Missing run data."); return false; }
            if (check != Check(s.Run, s.Growth)) { why = T("无尽档校验失败（文件被改动或损坏）。", "Checksum mismatch (edited or damaged)."); return false; }
            return true;
        }
    }
}
