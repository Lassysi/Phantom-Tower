// 键表：Windows 虚拟键码 ↔ 虚幻引擎键名（游戏存档里的写法）↔ 显示名；以及游戏默认占用（与 src/tower_hotkey.h kReserved 同一份）
using System;
using System.Collections.Generic;
using System.IO;
using System.Text;

namespace PBZTowerHotkey
{
    sealed class KeyInfo
    {
        public int Vk; public string Ue; public string Label;
        public KeyInfo(int vk, string ue, string label) { Vk = vk; Ue = ue; Label = label; }
    }

    static class KeyTable
    {
        public static readonly List<KeyInfo> All = new List<KeyInfo>();
        static readonly Dictionary<int, KeyInfo> ByVk = new Dictionary<int, KeyInfo>();
        static readonly Dictionary<string, KeyInfo> ByUe = new Dictionary<string, KeyInfo>(StringComparer.OrdinalIgnoreCase);

        static void Add(int vk, string ue, string label) { var k = new KeyInfo(vk, ue, label); All.Add(k); ByVk[vk] = k; ByUe[ue] = k; }

        static KeyTable()
        {
            for (char c = 'A'; c <= 'Z'; ++c) Add(c, c.ToString(), c.ToString());
            string[] digits = { "Zero", "One", "Two", "Three", "Four", "Five", "Six", "Seven", "Eight", "Nine" };
            for (int i = 0; i < 10; ++i) { Add('0' + i, digits[i], i.ToString()); Add(0x60 + i, "NumPad" + digits[i], "Num " + i); }
            for (int i = 1; i <= 12; ++i) Add(0x6F + i, "F" + i, "F" + i);
            Add(0x6A, "Multiply", "Num *"); Add(0x6B, "Add", "Num +"); Add(0x6D, "Subtract", "Num -"); Add(0x6E, "Decimal", "Num ."); Add(0x6F, "Divide", "Num /");
            Add(0x2D, "Insert", "Insert"); Add(0x2E, "Delete", "Delete"); Add(0x24, "Home", "Home"); Add(0x23, "End", "End");
            Add(0x21, "PageUp", "Page Up"); Add(0x22, "PageDown", "Page Down");
            Add(0x26, "Up", "↑"); Add(0x28, "Down", "↓"); Add(0x25, "Left", "←"); Add(0x27, "Right", "→");
            Add(0x09, "Tab", "Tab"); Add(0x0D, "Enter", "Enter"); Add(0x1B, "Escape", "Esc"); Add(0x20, "SpaceBar", "Space"); Add(0x08, "BackSpace", "Backspace");
            Add(0x14, "CapsLock", "Caps Lock"); Add(0x13, "Pause", "Pause"); Add(0x91, "ScrollLock", "Scroll Lock"); Add(0x90, "NumLock", "Num Lock");
            Add(0xBA, "Semicolon", ";"); Add(0xBB, "Equals", "="); Add(0xBC, "Comma", ","); Add(0xBD, "Hyphen", "-"); Add(0xBE, "Period", ".");
            Add(0xBF, "Slash", "/"); Add(0xC0, "Tilde", "`"); Add(0xDB, "LeftBracket", "["); Add(0xDC, "Backslash", "\\"); Add(0xDD, "RightBracket", "]"); Add(0xDE, "Apostrophe", "'");
            Add(0xA0, "LeftShift", "Left Shift"); Add(0xA1, "RightShift", "Right Shift"); Add(0xA2, "LeftControl", "Left Ctrl"); Add(0xA3, "RightControl", "Right Ctrl");
            Add(0xA4, "LeftAlt", "Left Alt"); Add(0xA5, "RightAlt", "Right Alt"); Add(0x5B, "LeftCommand", "Win"); Add(0x5C, "RightCommand", "Win"); Add(0x5D, "Apps", "Menu");
            Add(0x10, "Shift", "Shift"); Add(0x11, "Control", "Ctrl"); Add(0x12, "Alt", "Alt"); Add(0x2C, "PrintScreen", "Print Screen");
            Add(0x01, "LeftMouseButton", "鼠标左键 / Left mouse"); Add(0x02, "RightMouseButton", "鼠标右键 / Right mouse"); Add(0x04, "MiddleMouseButton", "鼠标中键 / Middle mouse");
            Add(0x05, "ThumbMouseButton", "鼠标侧键 1 / Mouse 4"); Add(0x06, "ThumbMouseButton2", "鼠标侧键 2 / Mouse 5");
        }

        public static KeyInfo Of(int vk) { KeyInfo k; return ByVk.TryGetValue(vk, out k) ? k : null; }
        public static KeyInfo OfUe(string ue) { KeyInfo k; return ByUe.TryGetValue(ue, out k) ? k : null; }
        public static string Label(int vk) { var k = Of(vk); return k != null ? k.Label : "0x" + vk.ToString("X2"); }

        // 游戏默认占用（IMC_* 资源导出，2026-11-08）+ 系统 / 覆盖层 + 影塔自己用的键。与 src/tower_hotkey.h kReserved 一致
        public static readonly Dictionary<int, string> Reserved = new Dictionary<int, string>
        {
            { 'W', "移动 / Move" }, { 'A', "移动 / Move" }, { 'S', "移动 / Move" }, { 'D', "移动 / Move" },
            { 0x26, "移动 / Move" }, { 0x28, "移动 / Move" }, { 0x25, "移动 / Move" }, { 0x27, "移动 / Move" },
            { 'C', "下蹲 / Crouch" }, { 'E', "处决、交互 / Execute, interact" }, { 'F', "重击 / Heavy attack" }, { 'Q', "战技 / Combat art" },
            { 'R', "下方道具（药）/ Bottom item" }, { 'T', "上方道具 / Top item" }, { 'V', "感知 / Sense" }, { 'M', "地图 / Map" },
            { 'I', "系统菜单 / System menu" }, { 'O', "游戏调试键 / Game debug key" }, { 'P', "拍照、跳过教学 / Photo, skip tutorial" },
            { '1', "切换兵器 / Switch weapon" }, { '2', "切换影之武装 / Switch shadow arm" },
            { '3', "游戏调试键 / Game debug key" }, { '4', "游戏调试键 / Game debug key" }, { '5', "游戏调试键 / Game debug key" },
            { '6', "游戏调试键 / Game debug key" }, { '7', "游戏调试键 / Game debug key" }, { '8', "游戏调试键 / Game debug key" }, { 0x63, "游戏调试键 / Game debug key" },
            { 0x09, "地图 / Map" }, { 0x1B, "系统菜单 / System menu" }, { 0x20, "跳跃 / Jump" }, { 0x0D, "确认 / Confirm" }, { 0x08, "返回 / Back" },
            { 0x10, "冲刺、闪避 / Sprint, dodge" }, { 0xA0, "冲刺、闪避 / Sprint, dodge" }, { 0xA1, "修饰键 / Modifier key" },
            { 0x11, "杀气技能 / Sha-chi skill" }, { 0xA2, "杀气技能 / Sha-chi skill" }, { 0xA3, "修饰键 / Modifier key" },
            { 0x12, "慢走 / Walk" }, { 0xA4, "慢走 / Walk" }, { 0xA5, "修饰键 / Modifier key" },
            { 0x5B, "系统键 / System key" }, { 0x5C, "系统键 / System key" }, { 0x5D, "系统键 / System key" },
            { 0x01, "轻击 / Light attack" }, { 0x02, "格挡、架势 / Deflect, stance" }, { 0x04, "锁定目标 / Lock on" },
            { 0x7B, "Steam 截图 / Steam screenshot" }, { 0x2C, "系统截图 / Screenshot" },
            { 'J', "影塔对话框确认 / Tower dialog confirm" }, { 'K', "影塔对话框确认 / Tower dialog confirm" }, { 'L', "影塔对话框确认 / Tower dialog confirm" },
        };

        // 玩家在游戏里改过的键位：EnhancedInputUserSettings.sav 里的键名（虚幻 FString：4 字节长度 + ASCII + \0）
        public static HashSet<string> Rebound(string savPath)
        {
            var found = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
            if (savPath == null || !File.Exists(savPath)) return found;
            byte[] b;
            try { b = File.ReadAllBytes(savPath); } catch { return found; }
            for (int i = 0; i + 5 < b.Length; ++i)
            {
                int len = BitConverter.ToInt32(b, i);
                if (len < 2 || len > 40 || i + 4 + len > b.Length || b[i + 4 + len - 1] != 0) continue;
                bool ascii = true;
                for (int k = 0; k < len - 1 && ascii; ++k) ascii = b[i + 4 + k] >= 0x20 && b[i + 4 + k] < 0x7F;
                if (!ascii) continue;
                var s = Encoding.ASCII.GetString(b, i + 4, len - 1);
                if (OfUe(s) != null) found.Add(s);
            }
            return found;
        }
    }
}
