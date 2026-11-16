using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;

namespace PBZ.Tower.UI;

public static class Wire
{
    public const int Abi = 6, Version = 2;
    public static string Text(this JsonNode? node, string key, string fallback = "") => node?[key] is JsonValue v && v.TryGetValue<string>(out var s) ? s : fallback;
    public static long Number(this JsonNode? node, string key, long fallback = 0)
    {
        if (node?[key] is not JsonValue v) return fallback;
        if (v.TryGetValue<long>(out var n)) return n;
        return v.TryGetValue<int>(out var small) ? small : fallback;
    }
    public static bool Flag(this JsonNode? node, string key) => node?[key] is JsonValue v && v.TryGetValue<bool>(out var b) && b;
}
public sealed class TowerError(string code, Exception? inner = null) : Exception(code, inner)
{
    public string Code { get; } = code;
}
public sealed record GameTarget(int Pid, long Started, string Executable);
public sealed record LinkStatus(string Code, bool Ready = false, string Detail = "");
public sealed class JsonLines(Stream stream)
{
    readonly StreamReader reader = new(stream, new UTF8Encoding(false, true), false, 4096, true);
    public async Task<JsonObject?> ReadAsync(CancellationToken ct)
    {
        var line = new StringBuilder(); var one = new char[1];
        while (await reader.ReadAsync(one.AsMemory(), ct) != 0)
        {
            if (one[0] == '\n')
            {
                try { return JsonNode.Parse(line.ToString()) as JsonObject ?? throw new TowerError("bad_message"); }
                catch (JsonException e) { throw new TowerError("bad_message", e); }
            }
            if (line.Length >= 1024 * 1024) throw new TowerError("message_too_large");
            if (one[0] != '\r') line.Append(one[0]);
        }
        if (line.Length > 0) throw new TowerError("truncated_message");
        return null;
    }
}

// Loader-only handshake. No tower state, menu, input, reward, restore or unload commands.
public sealed class LoaderProtocol(string modulePath)
{
    bool asked, loading;
    public bool Ready { get; private set; }
    public string Boot { get; private set; } = "";
    public string RuntimeVersion { get; private set; } = "";
    public IEnumerable<JsonObject> Accept(JsonObject m)
    {
        if (m.Text("op") == "hello")
        {
            if (m.Number("module_abi") != Wire.Abi) throw new TowerError("abi_mismatch");
            if (Boot.Length > 0 && Boot != m.Text("boot")) throw new TowerError("core_changed");
            Boot = m.Text("boot"); RuntimeVersion = m.Text("ver");
            if (!m.Flag("gamethread") || m.Number("frames") < 1 || Boot.Length == 0) { Ready = false; asked = false; return []; }
            if (!asked) { asked = true; return [new() { ["op"] = "module", ["cmd"] = "list" }]; }
        }
        else if (m.Text("op") == "module" && asked && !Ready)
        {
            if (m.Text("cmd") == "list")
            {
                if (!m.Flag("ok")) throw new TowerError("module_failed");
                if (m.Number("abi") != Wire.Abi) throw new TowerError("abi_mismatch");
                if (m["modules"] is not JsonArray modules) throw new TowerError("bad_message");
                if (modules.Any(x => x.Text("id") == "tower")) { Ready = true; return []; }
                if (!loading) { loading = true; return [new() { ["op"] = "module", ["cmd"] = "load", ["path"] = modulePath }]; }
            }
            else if (m.Text("cmd") == "load" && loading)
            {
                if (!m.Flag("ok")) throw new TowerError("module_failed");
                if (m.Number("abi") != Wire.Abi) throw new TowerError("abi_mismatch");
                if (m.Text("id") == "tower") Ready = true;
            }
        }
        return [];
    }
}
