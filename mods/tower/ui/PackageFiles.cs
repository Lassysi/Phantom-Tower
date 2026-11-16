using System.IO;
using System.IO.Compression;
using System.Reflection;
using System.Security.Cryptography;
using System.Text.Json;

namespace PBZ.Tower.UI;

public sealed record PayloadEntry(string Path, long Length, string Sha256);
public sealed record ReleaseManifest(int Schema, string Version, string Source, int Abi, int Protocol, string GameBuild,
    string GameExeSha256, string CoreSha256, string ModuleSha256, string EngineVersion, PayloadEntry[] Files);

public sealed class PackageFiles
{
    public static readonly JsonSerializerOptions Json = new() { PropertyNameCaseInsensitive = true, PropertyNamingPolicy = JsonNamingPolicy.CamelCase, WriteIndented = true };
    public string Assets { get; private set; } = AppContext.BaseDirectory;
    public string UserRoot { get; private set; } = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "PBZTower");
    public ReleaseManifest? Release { get; private set; }
    public bool Embedded { get; private set; }
    public static string Hash(string path) { using var f = File.OpenRead(path); return Convert.ToHexStringLower(SHA256.HashData(f)); }
    public static void RejectLinks(string path)
    {
        for (string? p = Path.GetFullPath(path); p is not null; p = Path.GetDirectoryName(p))
            if ((Directory.Exists(p) || File.Exists(p)) && (File.GetAttributes(p) & FileAttributes.ReparsePoint) != 0)
                throw new TowerError("ui.unsafe_path");
    }
    public static string EntryPath(string root, string entry)
    {
        if (string.IsNullOrWhiteSpace(entry) || entry.Contains(':') || entry.Contains('\\') || entry.Split('/').Any(x => x is "" or "." or "..")) throw new TowerError("ui.unsafe_path");
        var result = Path.GetFullPath(Path.Combine(root, entry));
        if (!result.StartsWith(Path.TrimEndingDirectorySeparator(Path.GetFullPath(root)) + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase)) throw new TowerError("ui.unsafe_path");
        RejectLinks(result); return result;
    }
    public static void ValidateManifest(ReleaseManifest manifest)
    {
        if (manifest.Schema != 1 || manifest.Abi != Wire.Abi || manifest.Protocol != Wire.Version || manifest.Files.Length == 0 ||
            manifest.EngineVersion.Length == 0 || manifest.GameBuild.Length == 0 || manifest.GameExeSha256.Length != 64) throw new TowerError("ui.package_invalid");
        var names = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        foreach (var entry in manifest.Files)
        {
            _ = EntryPath(Path.Combine(Path.GetTempPath(), "PBZTower-path-validation"), entry.Path);
            if (!names.Add(entry.Path) || entry.Length < 0 || entry.Sha256.Length != 64 || !entry.Sha256.All(Uri.IsHexDigit)) throw new TowerError("ui.package_invalid");
        }
        if (!manifest.Files.Any(x => x.Path == "core/PBZCore.dll" && x.Sha256.Equals(manifest.CoreSha256, StringComparison.OrdinalIgnoreCase)) ||
            !manifest.Files.Any(x => x.Path == "modules/PBZTower.dll" && x.Sha256.Equals(manifest.ModuleSha256, StringComparison.OrdinalIgnoreCase))) throw new TowerError("ui.package_invalid");
    }
    public static bool Verify(string root, ReleaseManifest manifest)
    {
        foreach (var entry in manifest.Files)
        {
            var path = EntryPath(root, entry.Path);
            if (!File.Exists(path) || new FileInfo(path).Length != entry.Length || !Hash(path).Equals(entry.Sha256, StringComparison.OrdinalIgnoreCase)) return false;
        }
        return true;
    }
    public static PackageFiles Open(bool mock = false, string? testRoot = null)
    {
        if (testRoot is not null && !mock) throw new TowerError("ui.test_mode_required");
        var result = new PackageFiles();
        if (testRoot is not null) result.UserRoot = Path.GetFullPath(testRoot);
        if (mock && testRoot is null) result.UserRoot = Path.Combine(result.UserRoot, "preview");
        using var payload = Assembly.GetExecutingAssembly().GetManifestResourceStream("PBZTower.Payload.zip");
        if (payload is not null)
        {
            result.Embedded = true;
            var digest = Convert.ToHexStringLower(SHA256.HashData(payload)); payload.Position = 0;
            var packages = Path.Combine(result.UserRoot, "packages");
            RejectLinks(packages); Directory.CreateDirectory(packages);
            result.Assets = Path.Combine(packages, digest[..24]);
            using var mutex = new Mutex(false, "Local\\PBZTower.Payload." + digest[..24]);
            try { if (!mutex.WaitOne(TimeSpan.FromSeconds(50))) throw new TowerError("ui.extract_busy"); }
            catch (AbandonedMutexException) { }
            try
            {
                using var zip = new ZipArchive(payload, ZipArchiveMode.Read);
                var jsonEntry = zip.GetEntry("release-manifest.json") ?? throw new TowerError("ui.package_invalid");
                using var source = jsonEntry.Open();
                result.Release = JsonSerializer.Deserialize<ReleaseManifest>(source, Json) ?? throw new TowerError("ui.package_invalid");
                ValidateManifest(result.Release);
                if (!Verify(result.Assets, result.Release))
                {
                    var stage = Path.Combine(packages, ".pending-" + Guid.NewGuid().ToString("N")); Directory.CreateDirectory(stage);
                    // Incomplete staging is retained on failure; never recursively delete a computed user path.
                    foreach (var entry in result.Release.Files)
                    {
                        var path = EntryPath(stage, entry.Path);
                        Directory.CreateDirectory(Path.GetDirectoryName(path)!);
                        using var input = (zip.GetEntry(entry.Path) ?? throw new TowerError("ui.package_invalid")).Open();
                        using var output = new FileStream(path, FileMode.CreateNew, FileAccess.Write); input.CopyTo(output);
                    }
                    if (!Verify(stage, result.Release)) throw new TowerError("ui.package_invalid");
                    File.WriteAllText(Path.Combine(stage, "release-manifest.json"), JsonSerializer.Serialize(result.Release, Json));
                    if (Directory.Exists(result.Assets)) Directory.Move(result.Assets, result.Assets + ".damaged-" + Guid.NewGuid().ToString("N"));
                    Directory.Move(stage, result.Assets);
                }
            }
            finally { mutex.ReleaseMutex(); }
        }
        else if (File.Exists(Path.Combine(result.Assets, "release-manifest.json")))
        {
            result.Release = JsonSerializer.Deserialize<ReleaseManifest>(File.ReadAllText(Path.Combine(result.Assets, "release-manifest.json")), Json) ?? throw new TowerError("ui.package_invalid");
            ValidateManifest(result.Release);
            if (!Verify(result.Assets, result.Release)) throw new TowerError("ui.package_invalid");
        }
        RejectLinks(result.UserRoot); Directory.CreateDirectory(result.UserRoot);
        return result;
    }
}
