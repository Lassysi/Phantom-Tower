using System.IO.Pipes;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.Json.Nodes;

namespace PBZ.Tower.UI;

// Dependencies are replaceable only in compiled tests, never through release CLI switches.
public sealed record LinkServices(string PipeName, Func<GameTarget?> Discover, Func<int, bool> RuntimePresent,
    Action<GameTarget, string> Inject, Func<GameTarget, string?> Validate, Func<NamedPipeClientStream, GameTarget, bool> OwnsPipe);

public sealed class CoreLink : IAsyncDisposable
{
    readonly PackageFiles files;
    readonly LinkServices services;
    readonly CancellationTokenSource stop = new();
    readonly SemaphoreSlim writer = new(1, 1);
    readonly HashSet<(int, long)> attempts = [];
    readonly HashSet<GameTarget> validated = [];
    NamedPipeClientStream? pipe;
    CancellationTokenSource? cycle;
    Task? loop;
    long lastHello;
    int disposed;
    string lastStatus = "";
    public bool Ready { get; private set; }
    public string RuntimeVersion { get; private set; } = "";
    public event Action<LinkStatus>? Status;
    public CoreLink(PackageFiles files, LinkServices? testServices = null)
    {
        this.files = files;
        services = testServices ?? new("PBZLab", GamePlatform.Discover, GamePlatform.RuntimePresent, GamePlatform.Inject,
            t => files.Release is { } r && !PackageFiles.Hash(t.Executable).Equals(r.GameExeSha256, StringComparison.OrdinalIgnoreCase) ? "game_version" : null,
            (p, t) => GetNamedPipeServerProcessId(p.SafePipeHandle, out var owner) && owner == (uint)t.Pid);
    }
    public void Start() => loop ??= Task.Run(Loop);
    void CancelCycle() { try { cycle?.Cancel(); } catch (ObjectDisposedException) { } }
    public void Reconnect() { CancelCycle(); pipe?.Dispose(); }
    void Report(string code, bool ready = false, string detail = "")
    {
        Ready = ready;
        if (code != lastStatus || detail.Length > 0) { lastStatus = code; Status?.Invoke(new(code, ready, detail)); }
    }
    async Task Loop()
    {
        while (!stop.IsCancellationRequested)
        {
            using var turn = CancellationTokenSource.CreateLinkedTokenSource(stop.Token); cycle = turn;
            try { await Connect(turn.Token); }
            catch (OperationCanceledException) when (turn.IsCancellationRequested) { }
            catch (Exception e) { Report(e switch { TowerError t => t.Code, System.ComponentModel.Win32Exception { NativeErrorCode: 5 } => "access_denied", _ => "connection_failed" }, detail: e.Message); }
            finally { Ready = false; pipe?.Dispose(); pipe = null; if (cycle == turn) cycle = null; }
            try { await Task.Delay(1500, stop.Token); } catch (OperationCanceledException) { break; }
        }
    }
    async Task Connect(CancellationToken ct)
    {
        var target = services.Discover();
        if (target is null) { Report("wait_game"); return; }
        if (validated.Add(target)) { if (services.Validate(target) is { } error) { validated.Remove(target); throw new TowerError(error); } }
        var connection = new NamedPipeClientStream(".", services.PipeName, PipeDirection.InOut, PipeOptions.Asynchronous);
        try { await connection.ConnectAsync(400, ct); }
        catch (Exception e) when (e is IOException or TimeoutException)
        {
            connection.Dispose();
            if (services.RuntimePresent(target.Pid)) { Report("wait_core"); return; }
            var identity = (target.Pid, target.Started);
            if (attempts.Contains(identity)) { Report("inject_once"); return; }
            if (files.Release is null) throw new TowerError("package_missing");
            var core = Path.Combine(files.Assets, "core", "PBZCore.dll");
            if (!PackageFiles.Hash(core).Equals(files.Release.CoreSha256, StringComparison.OrdinalIgnoreCase)) throw new TowerError("package_invalid");
            attempts.Add(identity); Report("loading_core");
            await Task.Run(() => services.Inject(target, core), ct);
            Report("wait_core"); return;
        }
        if (!services.OwnsPipe(connection, target)) { connection.Dispose(); throw new TowerError("wrong_pipe"); }
        pipe = connection; lastHello = Environment.TickCount64;
        var handshake = new LoaderProtocol(Path.Combine(files.Assets, "modules", "PBZTower.dll"));
        Report("wait_core");
        using var cancellation = ct.Register(connection.Dispose);
        using var heartbeatStop = CancellationTokenSource.CreateLinkedTokenSource(ct);
        var heartbeat = Heartbeat(heartbeatStop.Token);
        try
        {
            var input = new JsonLines(connection);
            while (await input.ReadAsync(ct) is { } m)
            {
                if (m.Text("op") == "hello") lastHello = Environment.TickCount64;
                foreach (var request in handshake.Accept(m))
                {
                    if (request.Text("cmd") == "load")
                    {
                        var path = request.Text("path");
                        if (files.Release is null || !File.Exists(path) || !PackageFiles.Hash(path).Equals(files.Release.ModuleSha256, StringComparison.OrdinalIgnoreCase)) throw new TowerError("package_missing");
                        Report("loading_module");
                    }
                    await Write(connection, request, ct);
                }
                RuntimeVersion = handshake.RuntimeVersion;
                if (handshake.Ready) Report("connected", true);
                else if (Ready) Report("wait_core");
            }
            Report("disconnected");
        }
        finally
        {
            heartbeatStop.Cancel();
            try { await heartbeat; } catch (Exception e) when (e is IOException or ObjectDisposedException or OperationCanceledException) { }
        }
    }
    async Task Heartbeat(CancellationToken ct)
    {
        while (!ct.IsCancellationRequested)
        {
            if (Environment.TickCount64 - lastHello > 10000) { Report("response_timeout"); CancelCycle(); return; }
            var current = pipe ?? throw new TowerError("disconnected");
            await Write(current, new() { ["op"] = "hello", ["client"] = "PBZTower.Loader", ["ver"] = "0.2.0" }, ct);
            await Task.Delay(2000, ct);
        }
    }
    async Task Write(NamedPipeClientStream current, JsonObject message, CancellationToken ct)
    {
        await writer.WaitAsync(ct);
        try
        {
            if (pipe != current || !current.IsConnected) throw new TowerError("disconnected");
            await current.WriteAsync(Encoding.UTF8.GetBytes(message.ToJsonString() + "\n"), ct);
            await current.FlushAsync(ct);
        }
        finally { writer.Release(); }
    }
    public async ValueTask DisposeAsync()
    {
        if (Interlocked.Exchange(ref disposed, 1) != 0) return;
        stop.Cancel(); CancelCycle(); pipe?.Dispose();
        if (loop is not null) try { await loop; } catch (OperationCanceledException) { }
        stop.Dispose(); writer.Dispose();
    }
    [DllImport("kernel32.dll", SetLastError = true)] static extern bool GetNamedPipeServerProcessId(Microsoft.Win32.SafeHandles.SafePipeHandle pipe, out uint pid);
}
