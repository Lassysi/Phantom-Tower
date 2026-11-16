// 命名管道服务端 \\.\pipe\PBZLab（shared/protocol.md v0）。支持多个连接（修改器 + 开发工具），状态与日志广播给所有连接。
// 每个连接的管道句柄只在它自己的线程上用重叠 I/O 读写；其他线程（游戏线程、日志）只往发送队列里放消息，
// 绝不直接 WriteFile —— 同步句柄上跨线程读写会互相阻塞，曾导致管道卡死。
#include "pipe.h"

#include "../services/diag.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <deque>
#include <format>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#include <memory>

#include "../features/feature.h"
#include "../features/world/enemy_spawn.h"
#include "../host/boot.h"
#include "../host/core_identity.h"
#include "../host/modules.h"
#include "../sdk/pbz_module.h"
#include "../host/log.h"
#include "../services/gamethread.h"
#include "../services/inventory.h"
#include "../services/process_event.h"

namespace pbz { void RefreshUnavailableFeatures(); bool ResolveFeature(IFeature*); std::string CoreStartupStatus(); }

namespace pbz::ipc {

std::optional<std::string> Field(const std::string& line, const char* key) {
    const auto k = std::format("\"{}\"", key);
    auto p = line.find(k);
    if (p == std::string::npos) return std::nullopt;
    p = line.find(':', p + k.size());
    if (p == std::string::npos) return std::nullopt;
    ++p;
    while (p < line.size() && line[p] == ' ') ++p;
    if (p < line.size() && line[p] == '"') {
        auto e = line.find('"', p + 1);
        return line.substr(p + 1, e - p - 1);
    }
    auto e = line.find_first_of(",}", p);
    return line.substr(p, e - p);
}

// 界面宿主（System.Text.Json）会把中文转成 \uXXXX：还原成 UTF-8（只处理 BMP 字符与 \" \\）
std::string Unescape(const std::string& s) {
    std::string o;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '\\' || i + 1 >= s.size()) { o += s[i]; continue; }
        const char c = s[++i];
        if (c == 'u' && i + 4 < s.size()) {
            const unsigned cp = std::stoul(s.substr(i + 1, 4), nullptr, 16);
            i += 4;
            if (cp < 0x80) o += static_cast<char>(cp);
            else if (cp < 0x800) { o += static_cast<char>(0xC0 | cp >> 6); o += static_cast<char>(0x80 | (cp & 0x3F)); }
            else { o += static_cast<char>(0xE0 | cp >> 12); o += static_cast<char>(0x80 | (cp >> 6 & 0x3F)); o += static_cast<char>(0x80 | (cp & 0x3F)); }
        } else o += c;
    }
    return o;
}

std::string Esc(const std::string& s) {
    std::string o;
    for (char c : s) {
        if (c == '"' || c == '\\') o += '\\';
        if (c == '\n') { o += "\\n"; continue; }
        if (c == '\r') continue;
        o += c;
    }
    return o;
}

namespace {

std::atomic<bool> g_stop{false};
HANDLE g_loopDone = nullptr;  // 接入线程退出时置位（线程都 detach：全局 std::thread 在进程退出时会触发 std::terminate）
std::function<void()> g_onUnload;
std::mutex g_opLock;
std::unordered_map<std::string, std::function<void(const std::string&)>> g_ops;  // 模块登记的 op

std::string StateJson(IFeature* f, bool ok, const std::string& err = {}, const std::string& id = {}) {
    if (!ok) return std::format(R"({{"op":"state","id":"{}","ok":false,"error":"{}"}})", f ? std::string(f->Id()) : Esc(id), Esc(err));
    const auto v = f->IsToggle() ? std::string(f->enabled ? "true" : "false") : std::format("{}", f->value);
    return std::format(R"({{"op":"state","id":"{}","value":{},"ok":true}})", f->Id(), v);
}

void SendFeatureList(bool pending) {
    std::string s = R"({"op":"list","features":[)";
    bool first = true;
    for (auto& f : Features()) {
        if (IsModuleFeature(f.get())) continue;  // 影塔等模块的内部开关不给修改器看
        // Before the first Tick, read only immutable metadata. Game-thread
        // installation can race this transport thread; do not read mutable state.
        const auto value = pending ? (f->IsToggle() ? "false" : "1") :
            f->IsToggle() ? (f->enabled ? "true" : "false") : std::format("{}", f->value);
        s += std::format(R"({}{{"id":"{}","available":{},"value":{},"reason":"{}"}})", first ? "" : ",", f->Id(),
            !pending && f->available ? "true" : "false", value, pending ? "进入游戏后可用" : Esc(f->reason));
        first = false;
    }
    Send(s + "]}");
}
bool HasGameFrame() { return gamethread::Ready() && gamethread::Frames() > 0; }

#ifdef PBZ_MOD_ONLY
std::atomic<int> g_trainerClients{0};   // 已解锁的修改器连接数
thread_local bool t_trainer = false;    // 本连接（每个连接一个线程）已解锁
constexpr bool kModOnly = true;
#else
constexpr bool kModOnly = false;
#endif

void Handle(const std::string& line) {
    const auto op = Field(line, "op").value_or("");
#ifdef PBZ_MOD_ONLY
    // 影塔安装包的运行库是纯 Mod：默认不接受修改器开关、刷怪、物品编辑、传送、研究探针等请求（无敌 / 伤害倍率等一律不可开）。
    // 用户 2026-11-06：PBZv2 修改器要和影塔共存——一个游戏进程只能有一份运行库（第二份注入会自己退出），
    // 修改器发现已有影塔运行库时不再注入，改为连上来发 trainer_unlock 解锁；这个连接断开（修改器关闭）就撤销修改器开的功能、重新上锁
    if (op == "trainer_unlock") {
        if (!t_trainer) { t_trainer = true; ++g_trainerClients; log::Info("修改器已连接并解锁（影塔运行库与修改器共用）"); }
        Send(R"({"op":"trainer_unlock","ok":true})");
        return;
    }
    static const char* const kTrainerOps[] = {"list", "set", "restore_all", "trace", "watch", "action", "bell", "enemy",
                                              "mapprobe", "pos", "items", "item", "mark"};
    if (g_trainerClients.load() <= 0)
        for (const char* t : kTrainerOps)
            if (op == t) { Send(std::format(R"({{"op":"{}","ok":false,"error":"mod_only"}})", Esc(op))); return; }
#endif
    if (op == "hello") {
        Send(std::format(R"({{"op":"hello","core":"PBZCore","ver":"0.1.0","startup_rev":1,"mod_only":{},"trainer":{},"core_sha256":"{}","module_abi":{},"boot":"{}","game_build":"26367565","engine":"UE5.7","gamethread":{},"frames":{},"cost_us":{},"peak_us":{},"stage":"{}"}})",
                         kModOnly ? "true" : "false", TrainerUnlocked() ? "true" : "false", LoadedCoreSha256(),sdk::kAbi, BootId(), gamethread::Ready() ? "true" : "false", gamethread::Frames(), gamethread::CostUs(), gamethread::PeakUs(), Esc(CoreStartupStatus())));
    } else if (op == "list") {
        if (!HasGameFrame()) SendFeatureList(true);
        else gamethread::Post([] { RefreshUnavailableFeatures(); SendFeatureList(false); });
    } else if (op == "set") {
        const auto id = Field(line, "id").value_or("");
        const auto raw = Field(line, "value").value_or("");
        if (!HasGameFrame()) { Send(StateJson(nullptr, false, "gamethread_unavailable", id)); return; }
        // 查找在游戏线程做：模块的功能在游戏线程加入功能表，管道线程不读功能表
        gamethread::Post([id, raw] {
            auto* f = FindFeature(id);
            if (!f) { Send(StateJson(nullptr, false, "unknown_feature:" + id, id)); return; }
            if (IsModuleFeature(f)) { Send(StateJson(nullptr, false, "module_owned:" + id, id)); return; }  // 模块的功能由模块自己控制
            double v = 1.0;
            if (!f->IsToggle()) {
                try { v = std::stod(raw); } catch (...) { Send(StateJson(f, false, "bad_value")); return; }
                if (!(v >= 0 && v <= 1000)) { Send(StateJson(f, false, "out_of_range")); return; }
            }
            const bool wantEnable = f->IsToggle() ? (raw == "true" || raw == "1") : v != 1;
            if (wantEnable && !f->enabled) {
                if (!ResolveFeature(f)) { Send(StateJson(f, false, f->reason)); return; }
            }
            // 修改器界面是持有者 "user"：关掉时若影塔等模块还在用（如神通），保持开启，等它们放手
            if (wantEnable) HoldFeature(f, "user", f->IsToggle() ? 0 : v);
            else ReleaseFeature(f, "user");
            log::Info("set {} = {}{}", f->Id(), raw, f->enabled && !wantEnable ? "（模块仍在使用，保持开启）" : "");
            Send(StateJson(f, true));
        });
    } else if (op == "restore_all") {
        if (!HasGameFrame()) return; // No feature has executed before the first game frame.
        // 只还原修改器自己（持有者 "user" 或没有持有者）开的；影塔等模块持有的功能不动
        gamethread::Post([] {
            for (auto& f : Features())
                if (f->enabled && !IsModuleFeature(f.get())) {
                    ReleaseFeature(f.get(), "user");
                    if (f->enabled && FeatureHolders(f.get()).empty()) { f->Disable(); if (!f->IsToggle()) f->SetValue(1.0); }
                    Send(StateJson(f.get(), true));
                }
            // 生成的怪物一并清理，但保留影塔的（request_id 以 "tower-" 开头）
            const size_t left = enemies::ClearExcept("tower-");
            Send(std::format(R"({{"op":"enemy","cmd":"clear","request_id":"restore_all","ok":{},"remaining":{}}})", left ? "false" : "true", left));
            log::Info("全部还原（保留模块持有的功能与影塔生成物）");
        });
    } else if (op == "trace") {  // 开发用：函数调用记录器
        const bool on = Field(line, "value").value_or("") == "true";
        gamethread::Post([on] { pe::Trace(on); });
        Send(std::format(R"({{"op":"state","id":"sys.trace","value":{},"ok":true}})", on ? "true" : "false"));
    } else if (op == "watch") {  // 研究用：{"op":"watch","class":"PerfectDodge","func":"K2_CanActivateAbility","mode":1}
        const auto cls = Field(line, "class").value_or("");
        const auto fn = Field(line, "func").value_or("");
        const int mode = std::atoi(Field(line, "mode").value_or("1").c_str());
        gamethread::Post([cls, fn, mode] { pe::Watch(cls, fn, mode); });
    } else if (op == "action") {  // 动作类功能：{"op":"action","id":"combat.kill_all"}
        const auto id = Field(line, "id").value_or("");
        if (!HasGameFrame()) { Send(StateJson(nullptr, false, "gamethread_unavailable", id)); return; }
        const int slot = std::atoi(Field(line, "slot").value_or("0").c_str());
        gamethread::Post([id, slot] {
            auto* f = FindFeature(id);
            if (!f) { Send(StateJson(nullptr, false, "unknown_feature:" + id, id)); return; }
            if (IsModuleFeature(f)) { Send(StateJson(nullptr, false, "module_owned:" + id, id)); return; }  // 模块的功能由模块自己控制
            if (!f->available) ResolveFeature(f);
            if (!f->available) { Send(StateJson(f, false, f->reason)); return; }
            std::string msg;
            const bool ok = f->Run(&msg, slot);
            log::Info("action {}：{}", f->Id(), msg);
            if (!ok) { Send(StateJson(f, false, msg)); return; }
            Send(std::format(R"({{"op":"event","type":"action","data":{{"id":"{}","msg":"{}"}}}})", f->Id(), Esc(msg)));
        });
    } else if (op == "bell") {  // 传送点：{"op":"bell","cmd":"list|tp","id":"Bell_XG_001","x":…,"y":…,"z":…,"yaw":…}
        const auto cmd = Field(line, "cmd").value_or("list"), id = Field(line, "id").value_or("");
        auto num = [&](const char* k) { try { return std::stod(Field(line, k).value_or("0")); } catch (...) { return 0.0; } };
        const double loc[3] = {num("x"), num("y"), num("z")}, yaw = num("yaw");
        if (!HasGameFrame()) { Send(R"({"op":"bell","ok":false,"msg":"gamethread_unavailable","active":[]})"); return; }
        gamethread::Post([cmd, id, loc0 = loc[0], loc1 = loc[1], loc2 = loc[2], yaw] {
            const double l[3] = {loc0, loc1, loc2};
            Send(BellCommand(cmd, id, l, yaw));
        });
    } else if (op == "enemy") {  // 怪物 / Boss 生成：{"op":"enemy","cmd":"status|spawn|clear","request_id":"…","path":"/Game/…_C","count":1,"distance_cm":600}
        enemies::Request r;
        r.command = Field(line, "cmd").value_or("");
        r.requestId = Field(line, "request_id").value_or("");
        r.path = Unescape(Field(line, "path").value_or(""));
        r.faction = Field(line, "faction").value_or("");
        // count 只接受纯整数（不截断小数），distance_cm 必须是有限数
        const auto count = Field(line, "count").value_or("1");
        const bool countOk = !count.empty() && count.size() < 4 && std::all_of(count.begin(), count.end(), [](char c) { return c >= '0' && c <= '9'; });
        double dist = 600;
        bool distOk = true;
        if (auto d = Field(line, "distance_cm")) { try { size_t used = 0; dist = std::stod(*d, &used); distOk = used == d->size() && std::isfinite(dist); } catch (...) { distOk = false; } }
        if (!countOk || !distOk) {
            Send(std::format(R"({{"op":"enemy","cmd":"{}","request_id":"{}","ok":false,"message":"bad_value"}})", Esc(r.command), Esc(r.requestId)));
            return;
        }
        r.count = std::atoi(count.c_str());
        r.distanceCm = dist;
        log::Info("怪物请求 cmd={} id={} path={} count={} dist={}", r.command, r.requestId, r.path, r.count, r.distanceCm);
        enemies::Submit(std::move(r), [](std::string json) {
            log::Info("怪物回执 {}", json.size() > 400 ? json.substr(0, 400) : json);  // 回执也进日志，便于实机排查
            Send(json);
        });
    } else if (op == "mapprobe") {  // 研究用：地图换算数据
        if (!HasGameFrame()) { Send(R"({"op":"mapprobe","ok":false})"); return; }
        gamethread::Post([] { Send(MapProbe()); });
    } else if (op == "pos") {  // 坐标列表：{"op":"pos","cmd":"list|save|tp|rename|del","id":3,"name":"…"}
        const auto cmd = Field(line, "cmd").value_or("list");
        const int id = std::atoi(Field(line, "id").value_or("0").c_str());
        const auto name = Unescape(Field(line, "name").value_or(""));
        if (!HasGameFrame()) { Send(R"({"op":"pos","ok":false,"msg":"gamethread_unavailable","list":[]})"); return; }
        gamethread::Post([cmd, id, name] { Send(PositionCommand(cmd, id, name)); });
    } else if (op == "items") {  // 物品编辑：当前持有
        if (!HasGameFrame()) { Send(R"({"op":"items","ok":false,"msg":"gamethread_unavailable"})"); return; }
        gamethread::Post([] { Send(inv::List()); });
    } else if (op == "item") {  // 物品编辑：{"op":"item","id":"Weapon_Material_01","kind":"stack","num":99}
        // 数量按原始文本交给 inv::ApplyRequest 完整解析（拒绝小数 / 尾随字符 / 溢出），session 防跨存档误写
        const auto id = Field(line, "id").value_or(""), kind = Field(line, "kind").value_or("");
        const auto number = Field(line, "num").value_or(""), session = Field(line, "session").value_or("");
        if (!HasGameFrame()) { Send(R"({"op":"item","ok":false,"msg":"gamethread_unavailable"})"); return; }
        gamethread::Post([id, kind, number, session] { Send(inv::ApplyRequest(id, kind, number, session)); });
    } else if (op == "diag") {  // 异常诊断：本进程已记录的异常报告（JSON 与 minidump 路径）
        Send(diag::Summary());
    } else if (op == "mark") {
        const auto label = Field(line, "label").value_or("mark");
        gamethread::Post([label] { pe::Mark(label); });
    } else if (op == "module") {  // 功能模块：{"op":"module","cmd":"load|list","path":"E:\…\PBZTower.dll"}
        Send(modules::Command(Field(line, "cmd").value_or("list"), Unescape(Field(line, "path").value_or(""))));
    } else if (op == "unload") {
        // 有功能模块（影塔）在用时，普通卸载请求（修改器关闭）不卸运行库；开发热换带 force:true
        if (modules::Count() > 0 && Field(line, "force").value_or("") != "true") {
            log::Info("卸载请求被保留：有 {} 个模块在用运行库", modules::Count());
            Send(std::format(R"({{"op":"unload","ok":false,"reason":"modules_loaded","modules":{}}})", modules::Count()));
            return;
        }
        if (g_onUnload) g_onUnload();
    } else {
        std::function<void(const std::string&)> fn;
        { std::lock_guard lk(g_opLock); if (auto it = g_ops.find(op); it != g_ops.end()) fn = it->second; }
        if (fn) {
            try { fn(line); } catch (...) { log::Info("模块 op {} 异常", op); }
        }
    }
}

// 每个客户端一个线程、一个发送队列；管道句柄只在自己的线程上做重叠读写。
struct Client {
    HANDLE h = INVALID_HANDLE_VALUE;
    HANDLE wake = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    std::mutex m;
    std::deque<std::string> out;
    std::atomic<bool> done{false};
    ~Client() { CloseHandle(wake); }
};
std::mutex g_cLock;
std::vector<std::shared_ptr<Client>> g_clients;
HANDLE g_stopEv = nullptr;

void Flush(Client& c) {
    std::deque<std::string> out;
    { std::lock_guard lk(c.m); out.swap(c.out); }
    for (auto& s : out) {
        OVERLAPPED wo{};
        wo.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        DWORD w = 0;
        if (!WriteFile(c.h, s.data(), static_cast<DWORD>(s.size()), &w, &wo) && GetLastError() == ERROR_IO_PENDING)
            GetOverlappedResult(c.h, &wo, &w, TRUE);
        CloseHandle(wo.hEvent);
    }
}

// 等重叠读完成；期间被唤醒就先把待发消息写出去。返回 false = 停止或断线
bool Wait(Client& c, OVERLAPPED& ov, DWORD* n) {
    HANDLE hs[3] = {ov.hEvent, c.wake, g_stopEv};
    for (;;) {
        const DWORD r = WaitForMultipleObjects(3, hs, FALSE, INFINITE);
        if (r == WAIT_OBJECT_0) return GetOverlappedResult(c.h, &ov, n, FALSE) != 0;
        if (r == WAIT_OBJECT_0 + 2) { CancelIoEx(c.h, &ov); GetOverlappedResult(c.h, &ov, n, TRUE); return false; }
        ResetEvent(c.wake);
        Flush(c);
    }
}

void ClientLoop(std::shared_ptr<Client> c) {
    OVERLAPPED ov{};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    std::string buf;
    char tmp[4096];
    while (!g_stop) {
        ResetEvent(ov.hEvent);
        DWORD n = 0;
        if (!ReadFile(c->h, tmp, sizeof tmp, &n, &ov) && (GetLastError() != ERROR_IO_PENDING || !Wait(*c, ov, &n))) break;
        if (!n) break;
        buf.append(tmp, n);
        for (size_t p; (p = buf.find('\n')) != std::string::npos;) {
            auto line = buf.substr(0, p);
            buf.erase(0, p + 1);
            if (!line.empty()) Handle(line);
        }
        Flush(*c);
    }
    CloseHandle(ov.hEvent);
#ifdef PBZ_MOD_ONLY
    if (t_trainer) {  // 最后一个修改器连接断开：先撤销修改器开的功能（影塔持有的不动），再重新上锁
        if (g_trainerClients.load() <= 1 && HasGameFrame()) Handle(R"({"op":"restore_all"})");
        if (--g_trainerClients <= 0) { g_trainerClients = 0; log::Info("修改器已断开：撤销修改器开启的功能，运行库回到纯 Mod"); }
    }
    t_trainer = false;
#endif
    DisconnectNamedPipe(c->h);
    CloseHandle(c->h);
    c->done = true;
}

// 管道名：默认 \\.\pipe\PBZLab；环境变量 PBZCORE_PIPE 可改（自检用独立名字，避免和游戏里的核心串线）
std::wstring PipeName() {
    wchar_t v[128];
    const DWORD n = GetEnvironmentVariableW(L"PBZCORE_PIPE", v, 128);
    return std::wstring(LR"(\\.\pipe\)") + (n && n < 128 ? std::wstring(v, n) : L"PBZLab");
}

void Loop() {
    while (!g_stop) {
        HANDLE h = CreateNamedPipeW(PipeName().c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED, PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
                                    PIPE_UNLIMITED_INSTANCES, 1 << 16, 1 << 16, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) { if (WaitForSingleObject(g_stopEv, 1000) == WAIT_OBJECT_0) break; continue; }
        OVERLAPPED ov{};
        ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        bool ok = ConnectNamedPipe(h, &ov) != 0;
        if (!ok) {
            const DWORD e = GetLastError();
            if (e == ERROR_PIPE_CONNECTED) ok = true;
            else if (e == ERROR_IO_PENDING) {
                HANDLE hs[2] = {ov.hEvent, g_stopEv};
                DWORD n;
                if (WaitForMultipleObjects(2, hs, FALSE, INFINITE) == WAIT_OBJECT_0) ok = GetOverlappedResult(h, &ov, &n, FALSE) != 0;
                else { CancelIoEx(h, &ov); GetOverlappedResult(h, &ov, &n, TRUE); }
            }
        }
        CloseHandle(ov.hEvent);
        if (!ok || g_stop) { CloseHandle(h); continue; }
        auto c = std::make_shared<Client>();
        c->h = h;
        {
            std::lock_guard lk(g_cLock);
            std::erase_if(g_clients, [](auto& x) {
                if (!x->done) return false;
                return true;
            });
            g_clients.push_back(c);
        }
        std::thread([c] {
            try { ClientLoop(c); } catch (...) { c->done = true; }  // 线程里的异常不能逃出去（否则 std::terminate 结束游戏）
        }).detach();
        log::Info("面板已连接（{} 个连接）", g_clients.size());
    }
}

}  // namespace

void Send(const std::string& json) {
    std::lock_guard lk(g_cLock);
    for (auto& c : g_clients) {
        if (c->done) continue;
        {
            std::lock_guard q(c->m);
            if (c->out.size() > 2000) continue;
            c->out.push_back(json + "\n");
        }
        SetEvent(c->wake);
    }
}

void RegisterOp(const char* op, std::function<void(const std::string&)> fn) {
    std::lock_guard lk(g_opLock);
    g_ops[op] = std::move(fn);
}

void Start(std::function<void()> onUnload) {
    g_onUnload = std::move(onUnload);
    g_stopEv = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    log::g_sink = [](const std::string& line) { Send(std::format(R"({{"op":"log","level":"info","msg":"{}"}})", Esc(line))); };
    g_loopDone = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    std::thread([] {
        try { Loop(); } catch (...) { log::Write("管道线程异常退出"); }
        SetEvent(g_loopDone);
    }).detach();
}

void Stop() {
    log::g_sink = nullptr;
    { std::lock_guard lk(g_opLock); g_ops.clear(); }
    g_stop = true;
    if (g_stopEv) SetEvent(g_stopEv);
    if (g_loopDone) WaitForSingleObject(g_loopDone, 3000);
    std::vector<std::shared_ptr<Client>> cs;
    { std::lock_guard lk(g_cLock); cs.swap(g_clients); }
    for (auto& c : cs)
        for (int i = 0; i < 150 && !c->done; ++i) Sleep(20);
}

bool TrainerUnlocked() {
#ifdef PBZ_MOD_ONLY
    return g_trainerClients.load() > 0;
#else
    return true;
#endif
}

}  // namespace pbz::ipc
