using System;
using System.Collections.Generic;
using System.Drawing;
using System.Globalization;
using System.IO;
using System.Text;
using System.Text.RegularExpressions;
using System.Web.Script.Serialization;
using System.Windows.Forms;

namespace PBZTowerSetup
{
    // A share contains a recipe only. Import never writes run.txt or growth.txt.
    sealed class SeedForm : Form
    {
        const string Format = "pbztower-seed-2", LegacyFormat = "pbztower-seed-1", GameBuild = "26367565";
        const int Plan = 14;
        static string DataDir { get { return ModFiles.TowerDataDir; } }
        static string T(string zh, string en) { return UiLanguage.T(zh, en); }
        readonly Label info = new Label();

        public SeedForm()
        {
            Text = T("分享种子", "Share seed");
            StartPosition = FormStartPosition.CenterParent; ClientSize = new Size(550, 250);
            BackColor = Theme.Ink; ForeColor = Theme.Paper; Font = Theme.Text(10f);
            info.SetBounds(24, 20, 500, 115);
            info.Text = T("导出当前影塔局的种子，不包含进度和个人存档。\n导入后在游戏选择“开始种子局”，从第 1 层开始，原塔档自动备份。\n需要相同游戏和 Mod 版本；随机兜底可能造成内容不同。", "Export a seed, not your progress or campaign save.\nImport it, then choose Start Seed Run in game to begin at floor 1. Your previous tower run is backed up.\nMatching game and mod versions are required; runtime fallbacks may vary.");
            Controls.Add(info);
            var export = new Button { Text = T("导出当前种子", "Export seed"), Left = 24, Top = 152, Width = 150, Height = 42 };
            var import = new Button { Text = T("导入种子", "Import seed"), Left = 190, Top = 152, Width = 150, Height = 42 };
            var cancel = new Button { Text = T("关闭", "Close"), Left = 356, Top = 152, Width = 150, Height = 42 };
            export.Click += (s, e) => Run(Export); import.Click += (s, e) => Run(Import); cancel.Click += (s, e) => Close();
            Controls.AddRange(new Control[] { export, import, cancel });
        }
        void Run(Action action) { try { action(); } catch (Exception ex) { MessageBox.Show(this, ex.Message, Text); } }
        static Dictionary<string, string> ReadRun()
        {
            var path = Path.Combine(DataDir, "run.txt");
            var f = new FileInfo(path);
            // 3.1.0（玩家反馈）：run.txt 只在有进行中的局时存在；说清楚原因
            if (!f.Exists || f.Length > 65536) throw new InvalidOperationException(T("现在没有进行中的影塔局（找不到 " + path + "）。\n请先在游戏里开一局（到第 1 层准备页即可）再导出；已结束的局不能导出。", "No tower run in progress (" + path + " not found).\nStart a run in game first; finished runs cannot be exported."));
            var fields = new Dictionary<string, string>(StringComparer.Ordinal);
            foreach (var line in File.ReadAllLines(path, Encoding.UTF8)) {
                int p = line.IndexOf('='); if (p <= 0) continue;
                var key = line.Substring(0, p);
                if (fields.ContainsKey(key)) throw new InvalidDataException(T("塔档字段重复。", "Duplicate tower run field."));
                fields.Add(key, line.Substring(p + 1));
            }
            return fields;
        }
        static string Get(Dictionary<string, string> d, string key) { string v; if (!d.TryGetValue(key, out v)) throw new InvalidDataException("Missing " + key); return v; }
        static ulong Seed(string s) { ulong n; if (string.IsNullOrEmpty(s) || s.Length > 20 || !ulong.TryParse(s, NumberStyles.None, CultureInfo.InvariantCulture, out n) || n == 0) throw new InvalidDataException(T("种子数字无效。", "Invalid seed.")); return n; }
        internal static string PendingFromJson(string input)
        {
            if (input == null || input.Length > 4096) throw new InvalidDataException("Seed too large");
            // JavaScriptSerializer silently accepts duplicate keys; reject them
            // so a shared seed has exactly one unambiguous value per field.
            foreach (var key in new[] { "format", "mod_version", "game_build", "plan", "seed", "difficulty" })
                if (Regex.Matches(input, "\\\"" + key + "\\\"\\s*:").Count != 1) throw new InvalidDataException("Duplicate or missing seed field: " + key);
            if (Regex.Matches(input, "\\\"shuffle\\\"\\s*:").Count > 1) throw new InvalidDataException("Duplicate seed field: shuffle");
            var json = new JavaScriptSerializer().DeserializeObject(input) as Dictionary<string, object>;
            // 3.1.0（玩家反馈：种子导不出 / 导不进）：Mod 版本只作记录，不再要求完全相同（游戏端原来写死 2.2.0，3.0.0 的种子永远导不进）；
            // 格式 2 带 Boss 排序方案 shuffle（1 / 2 / 3），导出局怎么排、导入就怎么排；旧格式 1 照收（按新局规则排）
            object format, mod, game, plan, seed, difficulty, shuffle = null;
            if (json == null || !json.TryGetValue("format", out format) || !(format is string) || ((string)format != Format && (string)format != LegacyFormat))
                throw new InvalidDataException(T("这不是影塔种子文件。", "Not a tower seed file."));
            var v2 = (string)format == Format;
            if (json.Count != (v2 ? 7 : 6) || !json.TryGetValue("mod_version", out mod) || !json.TryGetValue("game_build", out game) || !json.TryGetValue("plan", out plan) || !json.TryGetValue("seed", out seed) || !json.TryGetValue("difficulty", out difficulty) ||
                (v2 && (!json.TryGetValue("shuffle", out shuffle) || !(shuffle is int) || (int)shuffle < 1 || (int)shuffle > 4)) ||
                !(mod is string) || ((string)mod).Length == 0 || ((string)mod).Length > 32 || !(game is string) || !(plan is int) || (int)plan != Plan || !(seed is string) || !(difficulty is string) || ((string)difficulty != "easy" && (string)difficulty != "hard"))
                throw new InvalidDataException(T("种子文件内容不完整，或来自不同的关卡方案。", "Seed file is incomplete or from a different floor plan."));
            var number = Seed((string)seed);
            return "format=" + (v2 ? Format : LegacyFormat) + "\nmod=" + (string)mod + "\nplan=" + Plan + "\nseed=" + number.ToString(CultureInfo.InvariantCulture) + "\ndifficulty=" + (string)difficulty + "\n" + (v2 ? "shuffle=" + (int)shuffle + "\n" : "");
        }
        void Export()
        {
            var run = ReadRun(); var number = Seed(Get(run, "seed")); var diff = Get(run, "diff");
            // 3.1.0：原来只认 shuffle=1（2.x 十场一组），3.0.0 以后的新局（2 / 3）全都导不出；3.5.0：3.4.4 起新局用方案 4，一并认
            string shuffle; run.TryGetValue("shuffle", out shuffle);
            if (Get(run, "plan") != Plan.ToString(CultureInfo.InvariantCulture) || Get(run, "floors") != "50" || (shuffle != "1" && shuffle != "2" && shuffle != "3" && shuffle != "4") || (diff != "0" && diff != "1"))
                throw new InvalidDataException(T("当前塔档不是 50 层完整局（或关卡方案不同），不能导出种子。", "Only full 50-floor runs of the current floor plan can be exported."));
            var payload = new Dictionary<string, object> { {"format", Format}, {"mod_version", ModFiles.Version}, {"game_build", GameBuild}, {"plan", Plan}, {"seed", number.ToString(CultureInfo.InvariantCulture)}, {"difficulty", diff == "1" ? "easy" : "hard"}, {"shuffle", int.Parse(shuffle, CultureInfo.InvariantCulture)} };
            using (var d = new SaveFileDialog { Filter = "JSON (*.json)|*.json", FileName = "PBZTower-seed-" + number + ".json", Title = T("导出种子", "Export seed") }) {
                if (d.ShowDialog(this) != DialogResult.OK) return;
                File.WriteAllText(d.FileName, new JavaScriptSerializer().Serialize(payload), new UTF8Encoding(false));
                MessageBox.Show(this, T("已导出种子，可把下面的文件发给其他玩家：\n", "Seed exported. Share this file:\n") + d.FileName + T("\n\n正在游戏时导出的是最近一次保存的塔档。", "\n\nThis is the most recently saved tower run."), Text);
            }
        }
        void Import()
        {
            if (ModFiles.GameRunning()) throw new InvalidOperationException(T("请先关闭游戏，再导入种子。", "Close the game before importing a seed."));
            using (var d = new OpenFileDialog { Filter = "JSON (*.json)|*.json", Title = T("导入种子", "Import seed") }) {
                if (d.ShowDialog(this) != DialogResult.OK) return;
                var f = new FileInfo(d.FileName); if (!f.Exists || f.Length > 4096) throw new InvalidDataException(T("种子文件过大或不存在。", "Seed file missing or too large."));
                var text = PendingFromJson(File.ReadAllText(d.FileName, Encoding.UTF8));
                Directory.CreateDirectory(DataDir); var target = Path.Combine(DataDir, "pending-seed.txt"); var temp = target + "." + Guid.NewGuid().ToString("N") + ".tmp";
                try { File.WriteAllText(temp, text, new UTF8Encoding(false)); if (File.Exists(target)) File.Replace(temp, target, null); else File.Move(temp, target); }
                finally { if (File.Exists(temp)) File.Delete(temp); }
                MessageBox.Show(this, T("已导入文件：\n", "Imported file:\n") + d.FileName + T("\n\n进入游戏后选择“开始新局” → “开始种子局”。原塔档在开局时备份。", "\n\nIn game choose New Run → Start Seed Run. The previous run is backed up at start."), Text);
            }
        }
    }
}
