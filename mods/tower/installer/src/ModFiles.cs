// 安装 / 卸载。只碰自己的文件：<游戏EXE目录>\avrt.dll 和 <游戏EXE目录>\PBZTower\ 下的文件。
// 安装清单 PBZTower\install.txt 记录每个文件的 SHA-256。卸载：PBZTower 文件夹整个删；avrt.dll 只在确认是本 Mod 的
// （清单哈希对得上或带本 Mod 标记）时删，别的 Mod 的同名文件不碰。
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Reflection;
using System.Security.Cryptography;
using System.Text;

namespace PBZTowerSetup
{
    sealed class ModFiles
    {
        public const string Version = "4.1.1";
        const string Marker = "PBZTower-Setup";
        // 安装位置（相对游戏 EXE 目录）→ 内嵌资源名
        static readonly string[][] Payload =
        {
            new[] { "avrt.dll", "payload.avrt.dll" },
            new[] { @"PBZTower\PBZCore.dll", "payload.PBZCore.dll" },
            new[] { @"PBZTower\PBZTower.dll", "payload.PBZTower.dll" },
            new[] { @"PBZTower\README.txt", "payload.guide.txt" },  // 与手动安装包同名（旧版的 使用说明.txt 更新时自动清掉）
            new[] { @"PBZTower\world-template.json", "payload.world.json" },  // 塔档世界模板：统一的地图 / 剧情状态，与玩家进度无关
        };

        readonly string _dir;
        public ModFiles(string gameExeDir) { _dir = gameExeDir; }

        string ModDir { get { return Path.Combine(_dir, "PBZTower"); } }
        string ManifestPath { get { return Path.Combine(ModDir, "install.txt"); } }

        // 3.0.0：旧版本的局文件移到子目录留底（战绩 records / 语言 / 打开键设置等保留）。只在玩家不在塔档里时调用
        public static void ArchiveRun(string folder)
        {
            if (!Directory.Exists(TowerDataDir)) return;
            var dest = Path.Combine(TowerDataDir, folder);
            Directory.CreateDirectory(dest);
            foreach (var leaf in new[] { "run.txt", "growth.txt", "run-log.txt", "hexes.txt", "foresight.txt", "checkpoint-run.txt", "checkpoint-growth.txt", "ledger.txt" })
            {
                var src = Path.Combine(TowerDataDir, leaf);
                if (!File.Exists(src)) continue;
                var to = Path.Combine(dest, leaf);
                if (File.Exists(to)) File.Delete(to);
                File.Move(src, to);
            }
        }

        public static string TowerDataDir
        {
            get { return Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "PBZTower"); }
        }

        // 有清单，或者留着本 Mod 的代理与模块（手动安装 / 旧版卸载残留）都算已装，卸载按钮可用
        public bool Installed
        {
            get
            {
                var avrt = Path.Combine(_dir, "avrt.dll");
                return File.Exists(avrt) && (File.Exists(ManifestPath) || (IsOurProxy(avrt) && File.Exists(Path.Combine(ModDir, "PBZTower.dll"))));
            }
        }

        // 本 Mod 的 avrt.dll 代理（任何版本）都带着载入 \PBZTower\PBZCore.dll 的路径字符串；别的 Mod 的 avrt.dll 没有
        static bool IsOurProxy(string path)
        {
            try
            {
                var info = new FileInfo(path);
                if (!info.Exists || info.Length > 4 * 1024 * 1024) return false;
                var data = File.ReadAllBytes(path);
                var marker = Encoding.Unicode.GetBytes(@"\PBZTower");
                for (int i = 0; i + marker.Length <= data.Length; ++i)
                {
                    int k = 0;
                    while (k < marker.Length && data[i + k] == marker[k]) ++k;
                    if (k == marker.Length) return true;
                }
            }
            catch { }
            return false;
        }

        public string InstalledVersion
        {
            get
            {
                if (!File.Exists(ManifestPath)) return null;
                foreach (var line in File.ReadAllLines(ManifestPath, Encoding.UTF8))
                    if (line.StartsWith("version=")) return line.Substring(8);
                return null;
            }
        }

        public static bool GameRunning()
        {
            foreach (var p in Process.GetProcessesByName("PBZ")) { p.Dispose(); return true; }
            return false;
        }

        // 影塔上次没正常退出（游戏崩溃 / 强关）：真存档没有被改，但建议先进游戏让影塔自己收尾
        public static bool TowerSessionOpen()
        {
            var f = Path.Combine(TowerDataDir, "tower-session.txt");
            if (!File.Exists(f)) return false;
            var s = File.ReadAllText(f).Trim();
            return s.Length > 0 && s != "phase=outside";
        }

        // 同名 avrt.dll 已存在且不是本 Mod 装的：多半是别的 Mod，不覆盖
        public string Conflict()
        {
            var avrt = Path.Combine(_dir, "avrt.dll");
            if (!File.Exists(avrt)) return null;
            var known = ReadManifest();
            string expected;
            if (known.TryGetValue("avrt.dll", out expected) && expected == Hash(File.ReadAllBytes(avrt))) return null;
            if (IsOurProxy(avrt)) return null;  // 本 Mod 的代理（清单丢了 / 手动安装过）：直接覆盖
            return UiLanguage.T("游戏目录里已经有另一个 avrt.dll（可能来自别的 Mod）。为免冲突，安装已停止。\n位置：", "Another avrt.dll is already in the game folder, possibly from another mod. Installation stopped to avoid a conflict.\nLocation: ") + avrt;
        }

        // 安装 / 更新（同一个流程，玩家的爬塔进度在 %LOCALAPPDATA%\PBZTower，不碰）：
        //   1. 新文件全部写到暂存（.new）并读回核对哈希——磁盘满、杀毒拦截在这一步就失败，游戏目录原样不动；
        //   2. 旧文件改名成 .old 备份，暂存文件换上；任何一步出错就把 .old 换回去（回到更新前的版本）；
        //   3. 成功后写清单、删备份，并清掉旧版本留下、新版本不再带的文件（PBZTower 文件夹整个属于本 Mod）。
        public void Install()
        {
            Directory.CreateDirectory(ModDir);
            var staged = new List<string[]>();  // { 目标, 暂存, 哈希 }
            try
            {
                foreach (var entry in Payload)
                {
                    var bytes = Resource(entry[1]);
                    var target = Path.Combine(_dir, entry[0]);
                    var temp = target + ".new";
                    File.WriteAllBytes(temp, bytes);
                    var hash = Hash(bytes);
                    if (Hash(File.ReadAllBytes(temp)) != hash) throw new IOException(UiLanguage.T("写入后校验不一致：", "Verification failed after writing: ") + target);
                    staged.Add(new[] { target, temp, hash });
                }
            }
            catch
            {
                foreach (var s in staged) TryDelete(s[1]);
                foreach (var entry in Payload) TryDelete(Path.Combine(_dir, entry[0]) + ".new");
                throw;
            }

            var swapped = new List<string[]>();  // { 目标, 备份（无旧文件为 null） }
            try
            {
                foreach (var s in staged)
                {
                    string backup = null;
                    if (File.Exists(s[0])) { backup = s[0] + ".old"; TryDelete(backup); File.Move(s[0], backup); }
                    swapped.Add(new[] { s[0], backup });
                    File.Move(s[1], s[0]);
                }
            }
            catch
            {
                for (int i = swapped.Count - 1; i >= 0; --i)  // 倒序还原：回到更新前的样子
                {
                    TryDelete(swapped[i][0]);
                    if (swapped[i][1] != null && File.Exists(swapped[i][1])) File.Move(swapped[i][1], swapped[i][0]);
                }
                foreach (var s in staged) TryDelete(s[1]);
                throw;
            }

            var manifest = new StringBuilder();
            manifest.AppendLine("by=" + Marker);
            manifest.AppendLine("version=" + Version);
            manifest.AppendLine("installed=" + DateTime.Now.ToString("yyyy-MM-dd HH:mm"));
            foreach (var s in staged) manifest.AppendLine("file=" + s[0].Substring(_dir.TrimEnd('\\').Length + 1) + "|" + s[2]);
            File.WriteAllText(ManifestPath, manifest.ToString(), new UTF8Encoding(false));

            foreach (var w in swapped) if (w[1] != null) TryDelete(w[1]);
            var keep = new HashSet<string>(StringComparer.OrdinalIgnoreCase) { ManifestPath };
            foreach (var s in staged) keep.Add(s[0]);
            foreach (var f in Directory.GetFiles(ModDir)) if (!keep.Contains(f)) TryDelete(f);  // 旧版留下的文件
            // 1.3.0：Codex 的手柄测试安装器（PBZTower-v1.1.1-ControllerTest.exe）只换 PBZTower.dll，把原模块备份在这个子目录；完整安装后它已过时
            foreach (var d in new[] { "controller-test-backup" })
            {
                var p = Path.Combine(ModDir, d);
                try { if (Directory.Exists(p)) Directory.Delete(p, true); } catch { }
            }
        }

        static void TryDelete(string path) { try { if (File.Exists(path)) File.Delete(path); } catch { } }

        // 版本比较：1.10.0 > 1.9.2；读不出的按 0
        public static int CompareVersion(string a, string b)
        {
            var x = (a ?? "").Split('.');
            var y = (b ?? "").Split('.');
            for (int i = 0; i < Math.Max(x.Length, y.Length); ++i)
            {
                int p, q;
                int.TryParse(i < x.Length ? x[i] : "0", out p);
                int.TryParse(i < y.Length ? y[i] : "0", out q);
                if (p != q) return p.CompareTo(q);
            }
            return 0;
        }

        // 返回没删掉的文件说明（空 = 全部删干净）
        public List<string> Uninstall(bool removeProgress)
        {
            var kept = new List<string>();
            // avrt.dll 在游戏目录里，可能被别的 Mod 换成它们的：只删本 Mod 的（清单哈希对得上，或带本 Mod 标记）
            var avrt = Path.Combine(_dir, "avrt.dll");
            if (File.Exists(avrt))
            {
                string expected;
                var ours = (ReadManifest().TryGetValue("avrt.dll", out expected) && expected == Hash(File.ReadAllBytes(avrt))) || IsOurProxy(avrt);
                if (ours) File.Delete(avrt);
                else kept.Add(avrt + UiLanguage.T("（不是本 Mod 的文件，可能属于别的 Mod，未删除）", " (not this mod's file; kept)"));
            }
            // PBZTower 文件夹整个属于本 Mod：清单里的、旧版 / 手动安装留下的都删
            if (Directory.Exists(ModDir))
            {
                foreach (var f in Directory.GetFiles(ModDir)) File.Delete(f);
                if (Directory.GetFileSystemEntries(ModDir).Length == 0) Directory.Delete(ModDir);
                else kept.Add(ModDir + UiLanguage.T("（文件夹里还有子文件夹，未删除）", " (contains subfolders; kept)"));
            }
            if (removeProgress && Directory.Exists(TowerDataDir)) Directory.Delete(TowerDataDir, true);
            return kept;
        }

        Dictionary<string, string> ReadManifest()
        {
            var d = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
            if (!File.Exists(ManifestPath)) return d;
            foreach (var line in File.ReadAllLines(ManifestPath, Encoding.UTF8))
            {
                if (!line.StartsWith("file=")) continue;
                var parts = line.Substring(5).Split('|');
                if (parts.Length == 2 && !parts[0].Contains("..")) d[parts[0]] = parts[1];
            }
            return d;
        }

        static byte[] Resource(string name)
        {
            using (var s = Assembly.GetExecutingAssembly().GetManifestResourceStream(name))
            {
                if (s == null) throw new InvalidOperationException(UiLanguage.T("安装包缺少文件：", "Installer resource missing: ") + name);
                var ms = new MemoryStream();
                s.CopyTo(ms);
                return ms.ToArray();
            }
        }

        static string Hash(byte[] data)
        {
            using (var sha = SHA256.Create())
            {
                var sb = new StringBuilder();
                foreach (var b in sha.ComputeHash(data)) sb.Append(b.ToString("x2"));
                return sb.ToString();
            }
        }
    }
}
