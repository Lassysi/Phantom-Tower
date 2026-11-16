// 按安装器版本查找《影之刃零》的游戏程序目录。
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Text.RegularExpressions;
using Microsoft.Win32;

namespace PBZTowerSetup
{
    static class GameLocator
    {
        public const string AppId = "4115450";
        const string WeGameIdSuffix = "(2002630)";

#if WEGAME_SETUP
        public const string PlatformName = "WeGame";
        public static string Find() => FindWeGame();
        public static string Normalize(string path) => NormalizeWeGame(path);
#else
        public const string PlatformName = "Steam";
        public static string Find() => FindSteam();
        public static string Normalize(string path) => NormalizeSteam(path);
#endif

        static string FindSteam()
        {
            foreach (var lib in SteamLibraries())
            {
                var manifest = Path.Combine(lib, "steamapps", "appmanifest_" + AppId + ".acf");
                if (!File.Exists(manifest)) continue;
                var m = Regex.Match(File.ReadAllText(manifest), "\"installdir\"\\s+\"([^\"]+)\"");
                if (!m.Success) continue;
                var dir = NormalizeSteam(Path.Combine(lib, "steamapps", "common", m.Groups[1].Value));
                if (dir != null) return dir;
            }
            // 兜底：Steam 卸载项 → 正在运行的游戏 → 各盘常见 Steam 库位置。都找不到才让玩家手动选
            foreach (var key in new[] { @"HKEY_LOCAL_MACHINE\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\Steam App " + AppId,
                                        @"HKEY_LOCAL_MACHINE\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall\Steam App " + AppId })
            {
                var dir = NormalizeSteam(Safe(() => Registry.GetValue(key, "InstallLocation", null) as string));
                if (dir != null) return dir;
            }
            foreach (var p in Process.GetProcessesByName("PBZ"))
            {
                var dir = NormalizeSteam(Safe(() => p.MainModule.FileName));
                if (dir != null) return dir;
            }
            foreach (var drive in DriveInfo.GetDrives())
            {
                if (drive.DriveType != DriveType.Fixed || !Safe(() => drive.IsReady)) continue;
                foreach (var lib in new[] { "SteamLibrary", "Steam", @"Program Files (x86)\Steam", @"Program Files\Steam", @"Games\Steam", @"Games\SteamLibrary" })
                {
                    var dir = NormalizeSteam(Path.Combine(drive.RootDirectory.FullName, lib, "steamapps", "common", "PBZ"));
                    if (dir != null) return dir;
                }
            }
            return null;
        }

        static T Safe<T>(Func<T> f) { try { return f(); } catch { return default(T); } }

        // 用户手动选的路径：可以是游戏根目录、platform\steam 目录，或直接选 PBZ.exe
        static string NormalizeSteam(string path)
        {
            if (string.IsNullOrWhiteSpace(path)) return null;
            if (File.Exists(path)) path = Path.GetDirectoryName(path);
            if (!Directory.Exists(path)) return null;
            foreach (var candidate in new[] { path, Path.Combine(path, "platform", "steam"), Path.Combine(path, "..", "platform", "steam") })
            {
                var full = Path.GetFullPath(candidate);
                if (File.Exists(Path.Combine(full, "PBZ.exe")) && File.Exists(Path.Combine(full, "steam_api64.dll"))
                    && full.TrimEnd('\\').EndsWith(@"platform\steam", StringComparison.OrdinalIgnoreCase))
                    return full;
            }
            return null;
        }

        static string FindWeGame()
        {
            foreach (var process in Process.GetProcessesByName("PBZ"))
            {
                using (process)
                {
                    var dir = NormalizeWeGame(Safe(() => process.MainModule.FileName));
                    if (dir != null) return dir;
                }
            }
            foreach (var drive in DriveInfo.GetDrives())
            {
                if (drive.DriveType != DriveType.Fixed || !Safe(() => drive.IsReady)) continue;
                var apps = Path.Combine(drive.RootDirectory.FullName, "WeGameApps", "rail_apps");
                var dir = FindWeGameInLibrary(apps);
                if (dir != null) return dir;
            }
            return null;
        }

        internal static string FindWeGameInLibrary(string apps)
        {
            return Safe(() =>
            {
                if (!Directory.Exists(apps)) return null;
                foreach (var entry in Directory.EnumerateDirectories(apps))
                {
                    var dir = NormalizeWeGame(entry);
                    if (dir != null) return dir;
                }
                return null;
            });
        }

        // 验证游戏编号、平台目录和 PBZ.exe。可从游戏根目录、platform 或实际程序定位。
        static string NormalizeWeGame(string path)
        {
            if (string.IsNullOrWhiteSpace(path)) return null;
            return Safe(() =>
            {
                if (File.Exists(path))
                {
                    if (!Path.GetFileName(path).Equals("PBZ.exe", StringComparison.OrdinalIgnoreCase)) return null;
                    path = Path.GetDirectoryName(path);
                    if (!new DirectoryInfo(path).Name.Equals("wegame", StringComparison.OrdinalIgnoreCase)) return null;
                }
                if (!Directory.Exists(path)) return null;
                var game = new DirectoryInfo(path);
                if (game.Name.Equals("wegame", StringComparison.OrdinalIgnoreCase))
                {
                    game = game.Parent;
                    if (game == null || !game.Name.Equals("platform", StringComparison.OrdinalIgnoreCase)) return null;
                }
                if (game.Name.Equals("platform", StringComparison.OrdinalIgnoreCase)) game = game.Parent;
                if (game == null || !game.Name.EndsWith(WeGameIdSuffix, StringComparison.OrdinalIgnoreCase)) return null;
                var target = Path.Combine(game.FullName, "platform", "wegame");
                return File.Exists(Path.Combine(target, "PBZ.exe")) ? target : null;
            });
        }

        static IEnumerable<string> SteamLibraries()
        {
            var roots = new List<string>();
            foreach (var key in new[] { @"HKEY_CURRENT_USER\Software\Valve\Steam", @"HKEY_LOCAL_MACHINE\SOFTWARE\WOW6432Node\Valve\Steam", @"HKEY_LOCAL_MACHINE\SOFTWARE\Valve\Steam" })
            {
                var v = Registry.GetValue(key, key.StartsWith("HKEY_CURRENT_USER") ? "SteamPath" : "InstallPath", null) as string;
                if (!string.IsNullOrEmpty(v)) roots.Add(v.Replace('/', '\\'));
            }
            var seen = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
            foreach (var root in roots)
            {
                if (seen.Add(root)) yield return root;
                var vdf = Path.Combine(root, "steamapps", "libraryfolders.vdf");
                if (!File.Exists(vdf)) continue;
                foreach (Match m in Regex.Matches(File.ReadAllText(vdf), "\"path\"\\s+\"([^\"]+)\""))
                {
                    var lib = m.Groups[1].Value.Replace(@"\\", @"\");
                    if (seen.Add(lib)) yield return lib;
                }
            }
        }
    }
}
