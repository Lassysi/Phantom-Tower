// 影塔·百战 打开键设置（用户 2026-11-08：有玩家键盘没有 Insert；界面只留必要内容）。
// 只写 %LOCALAPPDATA%\PBZTower\hotkey.txt；不碰游戏目录、不碰存档。影塔模块 2 秒内读到，游戏不用重启。
// 游戏要用的键（默认键位 + 玩家在游戏里改过的键位）不让设；Insert 始终可用。手柄玩家用 L3 + R3，不受影响。
using System;
using System.Collections.Generic;
using System.Drawing;
using System.IO;
using System.Linq;
using System.Text;
using System.Windows.Forms;
using PBZTowerSetup;

namespace PBZTowerHotkey
{
    sealed class HotkeyForm : Form
    {
        const string Prompt = "点这里再按键 / Click, then press a key";
        readonly Label _current = new Label(), _box = new Label(), _verdict = new Label();
        readonly InkButton _save = new InkButton(), _reset = new InkButton();
        readonly HashSet<string> _rebound;
        int _picked;

        static string ConfigPath
        {
            get { return Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "PBZTower", "hotkey.txt"); }
        }

        public HotkeyForm(string game = null)
        {
            Text = "菜单打开键 / Menu Key";
            FormBorderStyle = FormBorderStyle.FixedSingle; MaximizeBox = false; MinimizeBox = false; StartPosition = FormStartPosition.CenterScreen;
            ClientSize = new Size(400, 214); BackColor = Theme.Ink; ForeColor = Theme.Paper; KeyPreview = true;
            try { Icon = Icon.ExtractAssociatedIcon(Application.ExecutablePath); } catch { }

            var dir = game ?? GameLocator.Find();
            var sav = dir == null ? null : Path.GetFullPath(Path.Combine(dir, "..", "..", "ProjectPBZ", "Saved", "EnhancedInputUserSettings.sav"));
            _rebound = KeyTable.Rebound(sav);

            _current.SetBounds(24, 18, 352, 22); _current.Font = Theme.Text(10f); _current.ForeColor = Theme.Paper; _current.BackColor = Color.Transparent;

            _box.SetBounds(24, 48, 352, 58); _box.TextAlign = ContentAlignment.MiddleCenter; _box.Font = Theme.Text(10.5f);
            _box.BackColor = Theme.Panel; _box.ForeColor = Theme.Muted; _box.BorderStyle = BorderStyle.FixedSingle; _box.Cursor = Cursors.Hand;
            _box.Text = Prompt;
            _box.MouseDown += (s, e) =>
            {
                if (e.Button == MouseButtons.XButton1) Pick(0x05);
                else if (e.Button == MouseButtons.XButton2) Pick(0x06);
                else _box.Focus();
            };

            _verdict.SetBounds(24, 114, 352, 22); _verdict.Font = Theme.Text(9.5f); _verdict.BackColor = Color.Transparent; _verdict.ForeColor = Theme.Muted; _verdict.AutoEllipsis = true;

            _save.Text = "保存 / Save"; _save.Filled = true; _save.Font = Theme.Text(10f, FontStyle.Bold); _save.SetBounds(24, 150, 168, 38); _save.Enabled = false; _save.Click += (s, e) => Save();
            _reset.Text = "恢复默认 / Default"; _reset.Font = Theme.Text(10f, FontStyle.Bold); _reset.SetBounds(208, 150, 168, 38); _reset.Accent = Theme.Muted; _reset.Click += (s, e) => Reset();

            Controls.AddRange(new Control[] { _current, _box, _verdict, _save, _reset });
            ShowCurrent();
        }

        void ShowCurrent()
        {
            int vk = 0; string name = null;
            try
            {
                if (File.Exists(ConfigPath))
                    foreach (var line in File.ReadAllLines(ConfigPath))
                    {
                        if (line.StartsWith("vk=")) int.TryParse(line.Substring(3), out vk);
                        else if (line.StartsWith("name=")) name = line.Substring(5);
                    }
            }
            catch { }
            _current.Text = "当前 / Current：Insert" + (vk > 0 ? "  +  " + (name ?? KeyTable.Label(vk)) : "");
        }

        protected override bool ProcessCmdKey(ref Message msg, Keys keyData)
        {
            const int WM_KEYDOWN = 0x100, WM_SYSKEYDOWN = 0x104;
            if (msg.Msg == WM_KEYDOWN || msg.Msg == WM_SYSKEYDOWN)
            {
                int vk = (int)(keyData & Keys.KeyCode);
                // 区分左右修饰键（Shift / Ctrl / Alt 一律占用，但提示写准确）
                if (vk == 0x10) vk = (Control.ModifierKeys & Keys.Shift) != 0 && (GetKeyStateSafe(0xA1)) ? 0xA1 : 0xA0;
                else if (vk == 0x11) vk = GetKeyStateSafe(0xA3) ? 0xA3 : 0xA2;
                else if (vk == 0x12) vk = GetKeyStateSafe(0xA5) ? 0xA5 : 0xA4;
                Pick(vk);
                return true;  // 不让 Tab / 回车 / Alt 去切换焦点或按按钮
            }
            return base.ProcessCmdKey(ref msg, keyData);
        }

        [System.Runtime.InteropServices.DllImport("user32.dll")] static extern short GetKeyState(int vk);
        static bool GetKeyStateSafe(int vk) { try { return (GetKeyState(vk) & 0x8000) != 0; } catch { return false; } }

        void Show(string text, Color color) { _verdict.ForeColor = color; _verdict.Text = text; }

        void Pick(int vk)
        {
            var key = KeyTable.Of(vk);
            _box.Text = KeyTable.Label(vk);
            string why;
            if (key == null) why = "不支持这个键 / Not supported";
            else if (vk == 0x2D) why = "Insert 是默认键 / Already the default";
            else if (KeyTable.Reserved.TryGetValue(vk, out why)) why = "冲突：" + why.Split('/')[0].Trim() + " / Conflict: " + why.Split('/').Last().Trim();
            else if (_rebound.Contains(key.Ue)) why = "冲突：游戏键位已使用 / Conflict: used in game";
            else why = null;
            if (why != null)
            {
                _picked = 0; _save.Enabled = false; _box.ForeColor = Theme.Crimson;
                Show(why, Theme.Crimson);
            }
            else
            {
                _picked = vk; _save.Enabled = true; _box.ForeColor = Theme.Gold;
                Show("可以使用 / Available", Theme.Cyan);
            }
        }

        void Save()
        {
            if (_picked == 0) return;
            try
            {
                Directory.CreateDirectory(Path.GetDirectoryName(ConfigPath));
                File.WriteAllText(ConfigPath, "vk=" + _picked + "\r\nname=" + KeyTable.Label(_picked) + "\r\n", new UTF8Encoding(false));
                Show("已保存 / Saved", Theme.Cyan);
                _save.Enabled = false;
                ShowCurrent();
            }
            catch (Exception e) { Show("保存失败 / Save failed：" + e.Message, Theme.Crimson); }
        }

        void Reset()
        {
            try { if (File.Exists(ConfigPath)) File.Delete(ConfigPath); } catch { }
            _picked = 0; _save.Enabled = false; _box.ForeColor = Theme.Muted;
            _box.Text = Prompt;
            Show("已恢复默认 / Reset", Theme.Cyan);
            ShowCurrent();
        }
    }

    static class Program
    {
        [STAThread]
        static void Main()
        {
            Application.EnableVisualStyles();
            Application.SetCompatibleTextRenderingDefault(false);
            Application.Run(new HotkeyForm());
        }
    }
}
