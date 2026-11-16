using System.IO.Pipes;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;
using PBZ.Tower.UI;

static class Tests
{
    static readonly List<string> Checks = [];
    static string root = "";
    static void Check(bool valid, string name) { if (!valid) throw new Exception(name); Checks.Add(name); }
    static void Throws(Action action, string code, string name)
    { try { action(); throw new Exception("not rejected: " + name); } catch (TowerError e) { Check(e.Code.EndsWith(code), name); } }
    static JsonObject Hello(int abi = 6, bool ready = true, string boot = "fixture") => new() { ["op"] = "hello", ["core"] = "PBZCore", ["ver"] = "test", ["module_abi"] = abi, ["boot"] = boot, ["gamethread"] = ready, ["frames"] = ready ? 5 : 0 };
    static JsonObject Modules(bool exists = true) => new() { ["op"] = "module", ["cmd"] = "list", ["abi"] = 6, ["ok"] = true,
        ["modules"] = exists ? new JsonArray(new JsonObject { ["id"] = "tower" }) : new JsonArray() };
    static void Handshake()
    {
        var p = new LoaderProtocol("fixture-module.dll");
        Throws(() => p.Accept(Hello(2)).ToArray(), "abi_mismatch", "reject ABI 2 before module requests");
        Throws(() => p.Accept(Hello(3)).ToArray(), "abi_mismatch", "reject previous ABI 3 before module requests");
        Throws(() => p.Accept(Hello(4)).ToArray(), "abi_mismatch", "reject previous ABI 4 before module requests");
        Throws(() => p.Accept(Hello(5)).ToArray(), "abi_mismatch", "reject previous ABI 5 before module requests");
        p = new("fixture-module.dll");
        Check(!p.Accept(Hello(ready: false)).Any() && !p.Ready, "pipe connection alone is not readiness");
        var list = p.Accept(Hello()).Single();
        Check(list.Text("op") == "module" && list.Text("cmd") == "list", "ready hello requests module list");
        Check(!p.Accept(Hello()).Any(), "repeated hello does not repeat module list");
        Check(!p.Accept(Modules()).Any() && p.Ready, "existing tower reused without load");
        Throws(() => p.Accept(Hello(boot: "other")).ToArray(), "core_changed", "boot change invalidates handshake");
        p = new("fixture-module.dll"); p.Accept(Hello()).ToArray();
        var load = p.Accept(Modules(false)).Single();
        Check(load.Text("path") == "fixture-module.dll" && load.Text("cmd") == "load", "missing tower loads the declared module path");
        Check(!p.Accept(Modules(false)).Any(), "duplicate module-list response does not reload");
        p.Accept(new() { ["op"] = "module", ["cmd"] = "load", ["abi"] = 6, ["id"] = "tower", ["ok"] = true }).ToArray();
        Check(p.Ready, "module ack completes readiness");
        p.Accept(Hello(ready: false)).ToArray(); Check(!p.Ready, "loss of game-thread readiness disables ready state");
        p = new("module"); p.Accept(Hello()).ToArray(); p.Accept(Modules(false)).ToArray();
        Throws(() => p.Accept(new() { ["op"] = "module", ["cmd"] = "load", ["ok"] = false }).ToArray(), "module_failed", "failed module load cannot report ready");
    }
    static async Task Lines()
    {
        using var ms = new MemoryStream(Encoding.UTF8.GetBytes("{\"op\":\"hello\",\"name\":\"影塔\"}\r\n{\"x\":2}\n"));
        var reader = new JsonLines(ms);
        Check((await reader.ReadAsync(default))!.Text("name") == "影塔", "UTF-8 and CRLF survive framing");
        Check((await reader.ReadAsync(default))!.Number("x") == 2 && await reader.ReadAsync(default) is null, "multiple messages and clean EOF");
        using var truncated = new MemoryStream(Encoding.UTF8.GetBytes("{\"x\":1}"));
        try { await new JsonLines(truncated).ReadAsync(default); throw new Exception("truncated accepted"); } catch (TowerError e) { Check(e.Code == "truncated_message", "truncated message rejected"); }
        using var large = new MemoryStream(Encoding.UTF8.GetBytes(new string('x', 1024 * 1024 + 2)));
        try { await new JsonLines(large).ReadAsync(default); throw new Exception("oversize accepted"); } catch (TowerError e) { Check(e.Code == "message_too_large", "message allocation is bounded"); }
    }
    static PackageFiles PackageFixture()
    {
        var assets = AppContext.BaseDirectory;
        Directory.CreateDirectory(Path.Combine(assets, "core")); Directory.CreateDirectory(Path.Combine(assets, "modules"));
        File.WriteAllBytes(Path.Combine(assets, "core/PBZCore.dll"), [1, 2, 3]);
        File.WriteAllBytes(Path.Combine(assets, "modules/PBZTower.dll"), [4, 5, 6]);
        var entries = new[] { new PayloadEntry("core/PBZCore.dll", 3, PackageFiles.Hash(Path.Combine(assets, "core/PBZCore.dll"))), new PayloadEntry("modules/PBZTower.dll", 3, PackageFiles.Hash(Path.Combine(assets, "modules/PBZTower.dll"))) };
        var manifest = new ReleaseManifest(1, "test", "test", 6, 2, "test", new string('0', 64), entries[0].Sha256, entries[1].Sha256, "fixture-abi6", entries);
        File.WriteAllText(Path.Combine(assets, "release-manifest.json"), JsonSerializer.Serialize(manifest, PackageFiles.Json));
        var files = PackageFiles.Open(true, Path.Combine(root, "user"));
        Check(files.Release?.Abi == 6 && PackageFiles.Verify(assets, manifest), "folder payload verified before use");
        Throws(() => PackageFiles.EntryPath(root, "../escape"), "unsafe_path", "path traversal rejected");
        Throws(() => PackageFiles.EntryPath(root, "C:/escape"), "unsafe_path", "absolute and alternate-stream paths rejected");
        Throws(() => PackageFiles.ValidateManifest(manifest with { Abi = 3 }), "package_invalid", "manifest ABI drift rejected");
        Throws(() => PackageFiles.ValidateManifest(manifest with { Abi = 4 }), "package_invalid", "previous ABI 4 payload rejected");
        Throws(() => PackageFiles.ValidateManifest(manifest with { Abi = 5 }), "package_invalid", "previous ABI 5 payload rejected");
        Throws(() => PackageFiles.ValidateManifest(manifest with { Files = [entries[0], entries[0]] }), "package_invalid", "duplicate payload paths rejected");
        File.AppendAllText(Path.Combine(assets, "core/PBZCore.dll"), "changed");
        Check(!PackageFiles.Verify(assets, manifest), "tampered payload fails verification");
        File.WriteAllBytes(Path.Combine(assets, "core/PBZCore.dll"), [1, 2, 3]);
        Check(LoaderForm.Copy.Values.All(x => x.Zh.Length > 0 && x.En.Length > 0), "every loader message has both languages");
        return files;
    }
    static async Task Pipe(PackageFiles files, bool missing, bool wrongOwner = false)
    {
        var name = "PBZTower-Test-" + Guid.NewGuid().ToString("N");
        using var cancel = new CancellationTokenSource(TimeSpan.FromSeconds(8));
        await using var server = new NamedPipeServerStream(name, PipeDirection.InOut, 1, PipeTransmissionMode.Byte, PipeOptions.Asynchronous, 64, 64);
        var seen = new List<string>(); var result = new TaskCompletionSource<LinkStatus>(TaskCreationOptions.RunContinuationsAsynchronously);
        int injected = 0;
        var target = new GameTarget(Environment.ProcessId, 1, "fixture");
        await using var link = new CoreLink(files, new(name, () => target, _ => false, (_, _) => injected++, _ => null,
            (p, t) => !wrongOwner && GetNamedPipeServerProcessId(p.SafePipeHandle, out var pid) && pid == (uint)t.Pid));
        link.Status += s => { if (s.Ready || s.Code == "wrong_pipe") result.TrySetResult(s); };
        var serving = Task.Run(async () =>
        {
            try
            {
                await server.WaitForConnectionAsync(cancel.Token);
                var input = new JsonLines(server);
                using var output = new StreamWriter(server, new UTF8Encoding(false), 64, true) { AutoFlush = true, NewLine = "\n" };
                while (await input.ReadAsync(cancel.Token) is { } m)
                {
                    seen.Add(m.Text("op") + "/" + m.Text("cmd"));
                    var response = m.Text("op") == "hello" ? Hello() : m.Text("cmd") == "list" ? Modules(!missing) : new JsonObject { ["op"] = "module", ["cmd"] = "load", ["ok"] = true, ["id"] = "tower", ["abi"] = 6 };
                    await output.WriteLineAsync(response.ToJsonString());
                }
            }
            catch (Exception e) when (e is IOException or OperationCanceledException) { }
        });
        link.Start();
        var status = await result.Task.WaitAsync(cancel.Token);
        Check(wrongOwner ? status.Code == "wrong_pipe" : status.Ready, wrongOwner ? "real pipe owner mismatch rejected" : missing ? "real duplex pipe loads missing module" : "real duplex pipe reuses existing runtime/module");
        await link.DisposeAsync(); cancel.Cancel(); await serving;
        Check(injected == 0, "connected transport never injects another runtime");
        Check(seen.All(x => x is "hello/" or "module/list" or "module/load"), "connect/disconnect never sends gameplay, restore or unload commands");
        if (!wrongOwner) Check(seen.Count(x => x == "module/load") == (missing ? 1 : 0), "module loaded at most once per handshake");
    }
    static async Task InjectionOnce(PackageFiles files)
    {
        int calls = 0;
        var target = new GameTarget(12345, 1, "fixture");
        var once = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        await using var link = new CoreLink(files, new("PBZTower-Absent-" + Guid.NewGuid().ToString("N"), () => target, _ => false, (_, _) => Interlocked.Increment(ref calls), _ => null, (_, _) => true));
        link.Status += s => { if (s.Code == "inject_once") once.TrySetResult(); };
        link.Start(); await once.Task.WaitAsync(TimeSpan.FromSeconds(7));
        Check(calls == 1, "absent pipe injects once, never on every retry");
        link.Reconnect(); await Task.Delay(100);
        await link.DisposeAsync();
        Check(calls == 1, "manual reconnect does not reinject same process instance");
    }
    static void Layout(PackageFiles files)
    {
        Exception? failure = null;
        var thread = new Thread(() =>
        {
            try
            {
                Application.SetHighDpiMode(HighDpiMode.PerMonitorV2); Application.EnableVisualStyles();
                foreach (var language in new[] { "zh-CN", "en" })
                foreach (var dpi in new[] { 96, 120, 144, 192 })
                {
                    using var form = new LoaderForm(files, true);
                    form.StartPosition = FormStartPosition.Manual; form.Location = new(-20000, -20000);
                    form.ShowInTaskbar = false; form.Opacity = 0; form.Show();
                    Control[] All(Control c) => c.Controls.Cast<Control>().SelectMany(x => new[] { x }.Concat(All(x))).ToArray();
                    var controls = All(form);
                    ((ComboBox)controls.Single(x => x.Name == "Language")).SelectedIndex = language == "zh-CN" ? 0 : 1;
                    form.PerformAutoScale();
                    form.Scale(new SizeF((float)dpi / form.DeviceDpi, (float)dpi / form.DeviceDpi));
                    form.PerformLayout();
                    form.Size = form.PreferredSize; form.PerformLayout();
                    foreach (var c in controls.OfType<Label>())
                    {
                        var preferred = c.GetPreferredSize(new(c.Width, 0));
                        Check(c.Height >= preferred.Height, $"{language}/{dpi}: {c.Name} text fits vertically");
                    }
                    var logo = (PictureBox)controls.Single(x => x.Name == "FoxLogo");
                    Check(logo.Image is not null && logo.SizeMode == PictureBoxSizeMode.Zoom, $"{language}/{dpi}: original fox is contained");
                    var surface = controls.Single(x => x.Name == "Root");
                    using var bitmap = new Bitmap(surface.Width, surface.Height);
                    surface.DrawToBitmap(bitmap, new Rectangle(Point.Empty, bitmap.Size));
                    Check(surface.Width > 300 && controls.Single(x => x.Name == "Load").Visible, $"{language}/{dpi}: actual controls rendered");
                    bitmap.Save(Path.Combine(root, $"loader-{language}-{dpi}.png"), System.Drawing.Imaging.ImageFormat.Png);
                    form.Size = new(form.Width + 180, form.Height + 100); form.PerformLayout();
                    Check(controls.Single(x => x.Name == "Title").Right <= controls.Single(x => x.Name == "Title").Parent!.Width,
                        $"{language}/{dpi}: resized header stays inside its column");
                }
            }
            catch (Exception e) { failure = e; }
        });
        thread.SetApartmentState(ApartmentState.STA); thread.Start(); thread.Join();
        if (failure is not null) throw failure;
    }
    [DllImport("kernel32.dll", SetLastError = true)] static extern bool GetNamedPipeServerProcessId(Microsoft.Win32.SafeHandles.SafePipeHandle pipe, out uint pid);
    static void ProcessIdentity()
    {
        using var self = System.Diagnostics.Process.GetCurrentProcess();
        var actual = GamePlatform.Identity(Environment.ProcessId);
        Check(actual.Pid == self.Id && actual.Started == self.StartTime.ToUniversalTime().Ticks, "limited-query identity reads the actual process creation time");
        Check(string.Equals(actual.Executable, Environment.ProcessPath, StringComparison.OrdinalIgnoreCase), "limited-query identity reads the actual executable without module enumeration");
        Check(GamePlatform.Matches(actual, actual with { Executable = actual.Executable.ToUpperInvariant() }), "Windows path casing does not change identity");
        Check(!GamePlatform.Matches(actual, actual with { Pid = actual.Pid + 1 }), "different PID cannot be injected");
        Check(!GamePlatform.Matches(actual, actual with { Started = actual.Started + 1 }), "reused PID creation time cannot be injected");
        Check(!GamePlatform.Matches(actual, actual with { Executable = actual.Executable + ".other" }), "different executable cannot be injected");
        self.Refresh(); int before = self.HandleCount;
        for (int i = 0; i < 100; ++i) _ = GamePlatform.Identity(self.Id);
        self.Refresh(); Check(self.HandleCount <= before + 2, "limited-query handles are disposed after repeated identity reads");
    }
    public static async Task<int> Main(string[] args)
    {
        try
        {
            root = Path.GetFullPath(args.Single()); Directory.CreateDirectory(root);
            ProcessIdentity();
            Handshake(); await Lines(); var files = PackageFixture();
            await Pipe(files, false); await Pipe(files, true); await Pipe(files, false, true); await InjectionOnce(files); Layout(files);
            File.WriteAllText(Path.Combine(root, "loader-tests.json"), JsonSerializer.Serialize(new { ok = true, checks = Checks, gameAccess = false }, new JsonSerializerOptions { WriteIndented = true }));
            Console.WriteLine($"TOWER_LOADER_OK {Checks.Count} checks (isolated pipes; no game)"); return 0;
        }
        catch (Exception e) { Console.Error.WriteLine(e); return 1; }
    }
}
