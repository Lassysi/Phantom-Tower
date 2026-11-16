using System.Diagnostics;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using Microsoft.Win32.SafeHandles;

namespace PBZ.Tower.UI;

public static class GamePlatform
{
    static readonly HashSet<string> Names = new(StringComparer.OrdinalIgnoreCase)
    { "PBZ", "ProjectPBZ", "PBZ-Win64-Shipping", "ProjectPBZ-Win64-Shipping", "PhantomBladeZero", "PhantomBladeZero-Win64-Shipping" };
    public static GameTarget? Discover()
    {
        var targets = new List<GameTarget>(); bool denied = false;
        foreach (var process in Process.GetProcesses())
        {
            using (process)
            {
                try
                {
                    if (process.Id == Environment.ProcessId || !Names.Contains(process.ProcessName)) continue;
                    var target = Identity(process.Id);
                    var path = target.Executable;
                    if (FileVersionInfo.GetVersionInfo(path).ProductName is "PBZLab" or "PBZTower" || !IsGameImage(path)) continue;
                    _ = GameRoot(path);
                    targets.Add(target);
                }
                catch (System.ComponentModel.Win32Exception e) { if (e.NativeErrorCode == 5) denied = true; }
                catch (Exception e) when (e is IOException or InvalidOperationException or UnauthorizedAccessException or TowerError) { }
            }
        }
        if (targets.Count > 1) throw new TowerError("ui.multiple_games");
        if (targets.Count == 0 && denied) throw new TowerError("ui.access_denied");
        return targets.SingleOrDefault();
    }
    public static bool IsGameImage(string path)
    {
        using var file = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete);
        if (file.Length <= 16 * 1024 * 1024) return false;
        using var reader = new BinaryReader(file);
        if (reader.ReadUInt16() != 0x5a4d) return false;
        file.Position = 0x3c; int pe = reader.ReadInt32();
        if (pe < 0x40 || pe > file.Length - 6) return false;
        file.Position = pe; return reader.ReadUInt32() == 0x4550 && reader.ReadUInt16() == 0x8664;
    }
    public static string GameRoot(string image)
    {
        for (var d = Directory.GetParent(Path.GetFullPath(image)); d is not null; d = d.Parent)
            if (Directory.Exists(Path.Combine(d.FullName, "ProjectPBZ", "Content"))) return d.FullName;
        throw new TowerError("ui.save_not_found");
    }
    public static Process Open(GameTarget target)
    {
        var p = Process.GetProcessById(target.Pid);
        try { if (Matches(target, Identity(target.Pid))) return p; }
        catch { p.Dispose(); throw; }
        p.Dispose(); throw new TowerError("ui.game_changed");
    }
    public static GameTarget Identity(int pid)
    {
        using var h = OpenProcessForQuery(0x1000, false, pid);
        if (h.IsInvalid) throw new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error());
        return Identity(h, pid);
    }
    static GameTarget Identity(SafeProcessHandle h, int pid)
    {
        var path = new StringBuilder(32768); int length = path.Capacity;
        if (!QueryFullProcessImageName(h, 0, path, ref length) || !GetProcessTimes(h, out var created, out _, out _, out _))
            throw new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error());
        return new(pid, DateTime.FromFileTimeUtc(((long)created.High << 32) | created.Low).Ticks, path.ToString());
    }
    public static bool Matches(GameTarget expected, GameTarget actual) => expected.Pid == actual.Pid && expected.Started == actual.Started &&
        string.Equals(expected.Executable, actual.Executable, StringComparison.OrdinalIgnoreCase);
    public static bool RuntimePresent(int pid)
    {
        var handle = OpenMutex(0x00100000, false, "PBZCore.Instance." + pid);
        if (handle != IntPtr.Zero) { CloseHandle(handle); return true; }
        if (Marshal.GetLastWin32Error() == 2) return false;
        throw new TowerError("ui.access_denied");
    }
    public static void Inject(GameTarget target, string path)
    {
        using var process = Open(target);
        var localLoad = GetProcAddress(GetModuleHandle("kernel32.dll"), "LoadLibraryW");
        if (localLoad == IntPtr.Zero || VirtualQuery(localLoad, out var info, (nuint)Marshal.SizeOf<MemoryInfo>()) == 0) throw new TowerError("ui.inject_failed");
        var buffer = new StringBuilder(1024); GetModuleFileName(info.AllocationBase, buffer, buffer.Capacity);
        var host = Path.GetFileName(buffer.ToString());
        var remote = process.Modules.Cast<ProcessModule>().FirstOrDefault(x => x.ModuleName.Equals(host, StringComparison.OrdinalIgnoreCase));
        if (remote is null) throw new TowerError("ui.inject_failed");
        var start = remote.BaseAddress + (int)(localLoad.ToInt64() - info.AllocationBase.ToInt64());
        using var processHandle = OpenProcessForQuery(0x143A, false, target.Pid);
        if (processHandle.IsInvalid) throw new TowerError(Marshal.GetLastWin32Error() == 5 ? "ui.access_denied" : "ui.inject_failed");
        if (!Matches(target, Identity(processHandle, target.Pid))) throw new TowerError("ui.game_changed");
        var h = processHandle.DangerousGetHandle();
        IntPtr memory = IntPtr.Zero, thread = IntPtr.Zero; bool mayFree = true;
        try
        {
            var bytes = Encoding.Unicode.GetBytes(Path.GetFullPath(path) + "\0");
            memory = VirtualAllocEx(h, IntPtr.Zero, (nuint)bytes.Length, 0x3000, 0x04);
            if (memory == IntPtr.Zero || !WriteProcessMemory(h, memory, bytes, (nuint)bytes.Length, out var written) || written != (nuint)bytes.Length) throw new TowerError("ui.inject_failed");
            thread = CreateRemoteThread(h, IntPtr.Zero, 0, start, memory, 0, IntPtr.Zero);
            if (thread == IntPtr.Zero) throw new TowerError("ui.inject_failed");
            mayFree = WaitForSingleObject(thread, 10000) == 0;
            if (!mayFree) throw new TowerError("ui.inject_timeout");
        }
        finally
        {
            if (thread != IntPtr.Zero) CloseHandle(thread);
            // A timed-out loader may still read its argument; keep it until process exit.
            if (memory != IntPtr.Zero && mayFree) VirtualFreeEx(h, memory, 0, 0x8000);
        }
    }
    [StructLayout(LayoutKind.Sequential)] struct MemoryInfo { public IntPtr BaseAddress, AllocationBase; public uint AllocationProtect; public ushort PartitionId; public nuint RegionSize; public uint State, Protect, Type; }
    [StructLayout(LayoutKind.Sequential)] struct FileTime { public uint Low, High; }
    [DllImport("kernel32.dll", EntryPoint = "OpenProcess", SetLastError = true)] static extern SafeProcessHandle OpenProcessForQuery(uint access, bool inherit, int pid);
    [DllImport("kernel32.dll", EntryPoint = "QueryFullProcessImageNameW", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern bool QueryFullProcessImageName(SafeProcessHandle handle, int flags, StringBuilder path, ref int size);
    [DllImport("kernel32.dll", SetLastError = true)] static extern bool GetProcessTimes(SafeProcessHandle handle, out FileTime created, out FileTime exited, out FileTime kernel, out FileTime user);
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)] static extern IntPtr OpenMutex(uint access, bool inherit, string name);
    [DllImport("kernel32.dll", SetLastError = true)] static extern IntPtr VirtualAllocEx(IntPtr h, IntPtr a, nuint size, uint type, uint protect);
    [DllImport("kernel32.dll")] static extern bool VirtualFreeEx(IntPtr h, IntPtr a, nuint size, uint type);
    [DllImport("kernel32.dll", SetLastError = true)] static extern bool WriteProcessMemory(IntPtr h, IntPtr a, byte[] bytes, nuint size, out nuint written);
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode)] static extern IntPtr GetModuleHandle(string name);
    [DllImport("kernel32.dll", CharSet = CharSet.Ansi)] static extern IntPtr GetProcAddress(IntPtr module, string name);
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode)] static extern uint GetModuleFileName(IntPtr module, StringBuilder buffer, int size);
    [DllImport("kernel32.dll")] static extern nuint VirtualQuery(IntPtr address, out MemoryInfo info, nuint size);
    [DllImport("kernel32.dll", SetLastError = true)] static extern IntPtr CreateRemoteThread(IntPtr h, IntPtr attr, uint size, IntPtr start, IntPtr arg, uint flags, IntPtr id);
    [DllImport("kernel32.dll")] static extern uint WaitForSingleObject(IntPtr h, uint ms);
    [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr h);
}
