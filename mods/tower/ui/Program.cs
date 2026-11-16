using System.Text.Json;

namespace PBZ.Tower.UI;

public static class Program
{
    [STAThread]
    public static int Main(string[] args)
    {
        bool mock = args.Contains("--mock"), check = args.Contains("--self-check");
        string? Value(string name) { int i = Array.IndexOf(args, name); return i >= 0 && i + 1 < args.Length ? args[i + 1] : null; }
        try
        {
            var files = PackageFiles.Open(mock, Value("--test-root"));
            if (check)
            {
                Console.WriteLine(JsonSerializer.Serialize(new { ok = true, abi = Wire.Abi, loader = "0.2.0", engine = files.Release?.EngineVersion,
                    embedded = files.Embedded, files = files.Release?.Files.Length ?? 0, core = files.Release?.CoreSha256, module = files.Release?.ModuleSha256 })); return 0;
            }
            ApplicationConfiguration.Initialize();
            Application.Run(new LoaderForm(files, mock, mock ? Value("--scenario") ?? "connected" : "connected",
                mock && int.TryParse(Value("--quit-after-ms"), out var ms) ? ms : 0));
            return 0;
        }
        catch (Exception e)
        {
            string code = e is TowerError t ? t.Code : e.GetType().Name;
            if (check) Console.WriteLine(JsonSerializer.Serialize(new { ok = false, error = code }));
            else MessageBox.Show("Shadow Tower could not start. / 影塔启动失败。\n" + code, "Shadow Tower", MessageBoxButtons.OK, MessageBoxIcon.Error);
            return 1;
        }
    }
}
