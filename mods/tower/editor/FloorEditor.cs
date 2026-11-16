// 影塔跳层工具（自用，用户 2026-11-06：录视频演示后面层的 Boss，不用一层层打上来）。不进安装包 / Mod 压缩包。
// 通过游戏里运行库的管道 \\.\pipe\PBZLab 给影塔发带 editor:"floor" 的开局：发布版影塔只对这种请求接受起始层、
// 补记限时 / 禁药挑战次数（开放第 51 层）和补发跳过层的固定收获。开局会放弃当前局，在塔档里重新载入。
using System;
using System.Drawing;
using System.IO;
using System.IO.Pipes;
using System.Text;
using System.Text.RegularExpressions;
using System.Threading;
using System.Windows.Forms;

namespace PBZTowerFloorEditor
{
    static class Program
    {
        [STAThread]
        static void Main()
        {
            Application.EnableVisualStyles();
            Application.SetCompatibleTextRenderingDefault(false);
            Application.Run(new EditorForm());
        }
    }

    sealed class EditorForm : Form
    {
        static readonly Color Bg = Color.FromArgb(18, 18, 20), Panel = Color.FromArgb(28, 28, 32), Ink = Color.FromArgb(230, 226, 216),
                              Muted = Color.FromArgb(140, 136, 128), Gold = Color.FromArgb(232, 180, 52), Red = Color.FromArgb(190, 48, 48);
        readonly Label status = new Label(), current = new Label(), hint = new Label();
        readonly NumericUpDown floor = new NumericUpDown();
        readonly CheckBox catchup = new CheckBox(), hidden = new CheckBox();
        readonly Button go = new Button();
        readonly TextBox log = new TextBox();
        readonly object pipeLock = new object();
        NamedPipeClientStream pipe;
        StreamWriter writer;
        Thread reader;
        readonly System.Windows.Forms.Timer poll = new System.Windows.Forms.Timer { Interval = 2000 };

        public EditorForm()
        {
            Text = "影塔·跳层（自用）";
            ClientSize = new Size(520, 470);
            FormBorderStyle = FormBorderStyle.FixedSingle;
            MaximizeBox = false;
            StartPosition = FormStartPosition.CenterScreen;
            BackColor = Bg; ForeColor = Ink;
            Font = new Font("Microsoft YaHei UI", 10f);

            var title = new Label { Text = "影塔·跳层", Font = new Font("KaiTi", 20f, FontStyle.Bold), ForeColor = Gold, AutoSize = true, Location = new Point(24, 16) };
            var sub = new Label { Text = "自用演示工具：直接从指定层开始爬塔", ForeColor = Muted, AutoSize = true, Location = new Point(28, 56) };
            status.SetBounds(28, 90, 470, 22); status.ForeColor = Muted; status.Text = "正在连接游戏…";
            current.SetBounds(28, 114, 470, 22); current.Text = "";

            var lf = new Label { Text = "起始层", AutoSize = true, Location = new Point(28, 156) };
            floor.SetBounds(100, 152, 80, 28); floor.Minimum = 1; floor.Maximum = 50; floor.Value = 30;
            floor.BackColor = Panel; floor.ForeColor = Ink; floor.BorderStyle = BorderStyle.FixedSingle;

            catchup.Text = "补发前面各层的修为点和药瓶等级"; catchup.Checked = true;
            catchup.SetBounds(28, 192, 470, 26); catchup.ForeColor = Ink;
            hidden.Text = "开放第 51 层（补记限时、禁药各 2 次，打完第 50 层可进）"; hidden.Checked = false;
            hidden.SetBounds(28, 220, 470, 26); hidden.ForeColor = Ink;

            go.Text = "从第 30 层开局"; go.SetBounds(28, 258, 464, 44);
            go.FlatStyle = FlatStyle.Flat; go.FlatAppearance.BorderColor = Gold; go.BackColor = Gold; go.ForeColor = Bg;
            go.Font = new Font("Microsoft YaHei UI", 12f, FontStyle.Bold);
            go.Click += (s, e) => Start();
            floor.ValueChanged += (s, e) => go.Text = "从第 " + floor.Value + " 层开局";

            hint.SetBounds(28, 310, 470, 54); hint.ForeColor = Muted;
            hint.Text = "开局会放弃正在进行的一局，并重新载入塔档。\r\n载入完成后在游戏里按 Insert（手柄 L3+R3）→「下一层」，进入所选的层。";

            log.SetBounds(28, 368, 464, 86); log.Multiline = true; log.ReadOnly = true; log.ScrollBars = ScrollBars.Vertical;
            log.BackColor = Panel; log.ForeColor = Muted; log.BorderStyle = BorderStyle.None;

            Controls.AddRange(new Control[] { title, sub, status, current, lf, floor, catchup, hidden, go, hint, log });
            poll.Tick += (s, e) => { if (!Connected()) Connect(); else Send("{\"op\":\"tower\",\"cmd\":\"state\",\"client\":\"floor-editor\"}"); };
            Shown += (s, e) => { Connect(); poll.Start(); };
            FormClosed += (s, e) => { poll.Stop(); Disconnect(); };
        }

        bool Connected() { lock (pipeLock) return pipe != null && pipe.IsConnected; }

        void Connect()
        {
            Disconnect();
            try
            {
                var p = new NamedPipeClientStream(".", "PBZLab", PipeDirection.InOut, PipeOptions.Asynchronous);
                p.Connect(800);
                lock (pipeLock) { pipe = p; writer = new StreamWriter(p, new UTF8Encoding(false)) { AutoFlush = true, NewLine = "\n" }; }
                reader = new Thread(() => Read(p)) { IsBackground = true };
                reader.Start();
                SetStatus("已连接游戏里的影塔运行库", Ink);
                Send("{\"op\":\"hello\",\"client\":\"floor-editor\"}");
                Send("{\"op\":\"tower\",\"cmd\":\"state\",\"client\":\"floor-editor\"}");
            }
            catch (Exception)
            {
                SetStatus("没连上：请先启动游戏并进入存档（需要已安装影塔 Mod）", Red);
            }
        }

        void Disconnect()
        {
            lock (pipeLock)
            {
                try { if (pipe != null) pipe.Dispose(); } catch (Exception) { }
                pipe = null; writer = null;
            }
        }

        void Send(string json)
        {
            lock (pipeLock)
            {
                try { if (writer != null) writer.WriteLine(json); }
                catch (Exception) { try { pipe.Dispose(); } catch (Exception) { } pipe = null; writer = null; }
            }
        }

        void Read(NamedPipeClientStream p)
        {
            try
            {
                using (var r = new StreamReader(p, new UTF8Encoding(false)))
                {
                    string line;
                    while ((line = r.ReadLine()) != null) OnLine(line);
                }
            }
            catch (Exception) { }
            BeginInvokeSafe(() => SetStatus("与游戏断开，正在重连…", Muted));
        }

        static string Field(string line, string key)
        {
            var m = Regex.Match(line, "\"" + key + "\":(\"(?<s>(?:[^\"\\\\]|\\\\.)*)\"|(?<n>[^,}\\]]+))");
            if (!m.Success) return null;
            return m.Groups["s"].Success ? Regex.Unescape(m.Groups["s"].Value) : m.Groups["n"].Value.Trim();
        }

        void OnLine(string line)
        {
            if (Field(line, "op") != "tower") return;
            var ev = Field(line, "event");
            if (ev == "state")
            {
                var f = Field(line, "floor"); var ph = Field(line, "phase");
                var code = Field(line, "code"); var msg = Field(line, "msg");
                BeginInvokeSafe(() =>
                {
                    current.Text = "当前：" + PhaseText(ph) + (string.IsNullOrEmpty(f) ? "" : "，第 " + f + " 层");
                    if (!string.IsNullOrEmpty(code) && !string.IsNullOrEmpty(msg) && code != "info.state" && pendingLog)
                    {
                        pendingLog = code.StartsWith("info.enter") || code == "info.started" ? false : pendingLog;
                        Log((code.StartsWith("err.") ? "失败：" : "") + msg);
                    }
                });
            }
        }
        bool pendingLog;

        static string PhaseText(string ph)
        {
            switch (ph)
            {
                case "idle": return "未开局";
                case "ready": return "准备中";
                case "fighting": return "战斗中";
                case "reward": return "领奖中";
                case "won": return "已通关";
                case "lost": return "已失败";
                default: return ph ?? "未知";
            }
        }

        void Start()
        {
            if (!Connected()) { Connect(); if (!Connected()) return; }
            int n = (int)floor.Value;
            var sb = new StringBuilder();
            sb.Append("{\"op\":\"tower\",\"cmd\":\"start\",\"floors\":50,\"editor\":\"floor\",\"client\":\"floor-editor\",\"takeover\":true");
            sb.Append(",\"from\":").Append(n);
            if (catchup.Checked) sb.Append(",\"catchup\":\"1\"");
            if (hidden.Checked) sb.Append(",\"timed\":2,\"potion\":2");
            sb.Append("}");
            pendingLog = true;
            Send(sb.ToString());
            Log("已发送：从第 " + n + " 层开局" + (catchup.Checked ? "（补发点数）" : "") + (hidden.Checked ? "（开放第 51 层）" : "") + "。等游戏重新载入塔档…");
        }

        void Log(string s) { log.AppendText("[" + DateTime.Now.ToString("HH:mm:ss") + "] " + s + "\r\n"); }
        void SetStatus(string s, Color c) { BeginInvokeSafe(() => { status.Text = s; status.ForeColor = c; }); }
        void BeginInvokeSafe(Action a)
        {
            if (IsDisposed) return;
            try { if (InvokeRequired) BeginInvoke(a); else a(); } catch (Exception) { }
        }
    }
}
