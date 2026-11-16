#include "modules.h"

#include <windows.h>

#include <format>
#include <mutex>
#include <vector>

#include "../ipc/pipe.h"
#include "../sdk/pbz_module.h"
#include "../services/diag.h"
#include "../services/native_exec.h"
#include "../services/gamethread.h"
#include "../services/hooks.h"
#include "log.h"

namespace pbz::modules {
namespace {

struct Loaded {
    std::string id, path;
    PbzModuleStopFn stop = nullptr;
};
std::mutex g_lock;
std::vector<Loaded> g_loaded;
bool g_stopped = false;

const sdk::Api& Table() {
    static const sdk::Api api = [] {
        sdk::Api a = sdk::Stamp();
        a.ueNum = &ue::Num;
        a.ueObj = &ue::Obj;
        a.ueObjBatch = &ue::ObjBatch;
        a.ueName = &ue::Name;
        a.ueObjName = &ue::ObjName;
        a.ueClassOf = &ue::ClassOf;
        a.ueClassName = &ue::ClassName;
        a.ueIsA = &ue::IsA;
        a.ueFindClass = &ue::FindClass;
        a.uePropOffset = &ue::PropOffset;
        a.ueForEach = &ue::ForEach;
        a.ueIsDefault = &ue::IsDefault;
        a.ueRead = &ue::Read;
        a.ueWrite = &ue::Write;
        a.gtReady = &gamethread::Ready;
        a.gtFrames = &gamethread::Frames;
        a.gtPost = [](std::function<void()> fn) { gamethread::Post(std::move(fn), "模块队列"); };
        a.hkCreate = &hooks::Create;
        a.hkEnable = &hooks::Enable;
        a.absPrepare = &abshook::Prepare;
        a.absApply = &abshook::Apply;
        a.peReady = &pe::Ready;
        a.peFindFunction = &pe::FindFunction;
        a.peFunctionsWithPrefix = &pe::FunctionsWithPrefix;
        a.peCall = &pe::Call;
        a.peIntercept = &pe::Intercept;
        a.peRemoveIntercept = &pe::RemoveIntercept;
        a.peObserve = &pe::Observe;
        a.peUnobserve = &pe::Unobserve;
        a.atTargets = &attr::Targets;
        a.atAlive = &attr::Alive;
        a.atAttrOffset = &attr::AttrOffset;
        a.atActorOffset = &attr::ActorOffset;
        a.atWorldOffset = &attr::WorldOffset;
        a.atPlayerOffset = &attr::PlayerOffset;
        a.atAttrCurrent = &attr::AttrCurrent;
        a.atAttrBase = &attr::AttrBase;
        a.atWant = &attr::Want;
        a.atHold = &attr::Hold;
        a.logWrite = [](const std::string& s) { log::Write(s); };
        a.addFeature = [](IFeature* f) { AddFeature(std::unique_ptr<IFeature>(f)); };
        a.findFeature = &FindFeature;
        a.ipcSend = &ipc::Send;
        a.ipcRegisterOp = &ipc::RegisterOp;
        a.ipcField = &ipc::Field;
        a.ipcUnescape = &ipc::Unescape;
        a.ipcEsc = &ipc::Esc;
        a.enSubmit = &enemies::Submit;
        a.enOwnedActors = &enemies::OwnedActors;
        a.enOwnedBosses = &enemies::OwnedBosses;
        a.enOwnedFactions = &enemies::OwnedFactions;
        a.enClearTagged = &enemies::ClearTagged;
        a.invList = &inv::List;
        a.invApply = &inv::Apply;
        a.holdFeature = &HoldFeature;
        a.releaseFeature = &ReleaseFeature;
        a.mvBell = &BellCommand;
        a.mvPoint = &PointTeleport;
        a.mvBusy = &TeleportBusy;
        a.mvBellLevel = &BellLevel;
        a.mvWhere = &PlayerWhere;
        a.ueLive = &ue::Live;
        a.diagSection = &diag::Section;
        a.diagEvent = [](const char* tag, const void* o, const void* f) { diag::Event(tag, o, f); };
        a.diagWhere = &diag::SwapWhere;
        a.nxHook = &nx::Hook;
        a.nxUnhook = &nx::Unhook;
        return a;
    }();
    return api;
}

std::wstring Wide(const std::string& s) {
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::string Fail(const std::string& cmd, const std::string& why) {
    log::Info("模块 {} 失败：{}", cmd, why);
    return std::format(R"({{"op":"module","cmd":"{}","ok":false,"error":"{}"}})", ipc::Esc(cmd), ipc::Esc(why));
}

// 复制到 %LOCALAPPDATA%\PBZLab\modules\<名字>-<时间>-<序号>.dll：原文件不被锁；热换运行库后能载入新副本
std::wstring CopyForLoad(const std::wstring& src, std::string* why) {
    wchar_t base[MAX_PATH];
    if (!GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH)) { *why = "找不到 LOCALAPPDATA"; return {}; }
    std::wstring dir = std::wstring(base) + L"\\PBZLab";
    CreateDirectoryW(dir.c_str(), nullptr);
    dir += L"\\modules";
    CreateDirectoryW(dir.c_str(), nullptr);
    std::wstring stem = src.substr(src.find_last_of(L"\\/") + 1);
    if (const auto dot = stem.find_last_of(L'.'); dot != std::wstring::npos) stem.resize(dot);
    static int seq = 0;
    SYSTEMTIME t;
    GetLocalTime(&t);
    const std::wstring dst = std::format(L"{}\\{}-{:02}{:02}{:02}-{}-{}.dll", dir, stem, t.wHour, t.wMinute, t.wSecond, GetCurrentProcessId(), ++seq);
    if (!CopyFileW(src.c_str(), dst.c_str(), FALSE)) { *why = std::format("复制模块失败（{}）", GetLastError()); return {}; }
    return dst;
}

// inPlace：发布版自动加载，直接从安装目录载入（不复制、不在 %LOCALAPPDATA% 留副本）
std::string Load(const std::string& path, bool inPlace = false) {
    if (path.empty()) return Fail("load", "缺少 path");
    const std::wstring src = Wide(path);
    if (GetFileAttributesW(src.c_str()) == INVALID_FILE_ATTRIBUTES) return Fail("load", "文件不存在");
    std::lock_guard lk(g_lock);
    if (g_stopped) return Fail("load", "运行库正在停用");
    std::string why;
    const std::wstring copy = inPlace ? src : CopyForLoad(src, &why);
    if (copy.empty()) return Fail("load", why);
    HMODULE m = LoadLibraryW(copy.c_str());
    if (!m) return Fail("load", std::format("LoadLibrary 失败（{}）", GetLastError()));
    auto abi = reinterpret_cast<PbzModuleAbiFn>(GetProcAddress(m, "PbzModuleAbi"));
    auto idFn = reinterpret_cast<PbzModuleIdFn>(GetProcAddress(m, "PbzModuleId"));
    auto start = reinterpret_cast<PbzModuleStartFn>(GetProcAddress(m, "PbzModuleStart"));
    auto stop = reinterpret_cast<PbzModuleStopFn>(GetProcAddress(m, "PbzModuleStop"));
    // 还没启动的模块可以安全释放；启动之后（哪怕失败）就永不释放，它可能已经登记了回调
    if (!abi || !idFn || !start || !stop) { FreeLibrary(m); return Fail("load", "不是 PBZ 模块（缺少导出）"); }
    if (abi() != sdk::kAbi) {
        const auto v = abi();
        FreeLibrary(m);
        return Fail("load", std::format("接口版本不一致（运行库 {}，模块 {}）", sdk::kAbi, v));
    }
    const std::string id = idFn() ? idFn() : "";
    if (id.empty()) { FreeLibrary(m); return Fail("load", "模块没有标识"); }
    for (auto& l : g_loaded)
        if (l.id == id) { FreeLibrary(m); return Fail("load", "模块 " + id + " 已加载"); }
    char buf[512] = {};
    bool ok = false;
    try { ok = start(&Table(), buf, sizeof buf); } catch (...) { strcpy_s(buf, "模块启动异常"); }
    if (!ok) return Fail("load", std::format("模块 {} 启动失败：{}", id, buf));
    g_loaded.push_back({id, path, stop});
    log::Info("模块 {} 已加载：{}", id, path);
    return std::format(R"({{"op":"module","cmd":"load","ok":true,"id":"{}","abi":{}}})", ipc::Esc(id), sdk::kAbi);
}

std::string List() {
    std::lock_guard lk(g_lock);
    std::string s = std::format(R"({{"op":"module","cmd":"list","ok":true,"abi":{},"modules":[)", sdk::kAbi);
    for (size_t i = 0; i < g_loaded.size(); ++i)
        s += std::format(R"({}{{"id":"{}","path":"{}"}})", i ? "," : "", ipc::Esc(g_loaded[i].id), ipc::Esc(g_loaded[i].path));
    return s + "]}";
}

}  // namespace

std::string Command(const std::string& cmd, const std::string& path) {
#ifdef PBZ_MOD_ONLY
    if (cmd == "load") return Fail(cmd, "发布版只自动加载随包模块");
#endif
    if (cmd == "load") return Load(path);
    if (cmd == "list") return List();
    return Fail(cmd, "未知命令");
}

void AutoLoad(const std::wstring& dir) {
    const std::wstring path = dir + L"\\PBZTower.dll";
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) { log::Info("自动加载：{} 下没有 PBZTower.dll", std::string(dir.begin(), dir.end())); return; }
    int n = WideCharToMultiByte(CP_UTF8, 0, path.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string utf8(n > 0 ? n - 1 : 0, '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, path.c_str(), -1, utf8.data(), n, nullptr, nullptr);
    log::Info("自动加载：{}", Load(utf8, true));
}

size_t Count() {
    std::lock_guard lk(g_lock);
    return g_stopped ? 0 : g_loaded.size();
}

void StopAll() {
    std::vector<Loaded> loaded;
    {
        std::lock_guard lk(g_lock);
        g_stopped = true;
        loaded = g_loaded;
    }
    for (auto& l : loaded) {
        try { l.stop(); } catch (...) {}
        log::Info("模块 {} 已停止", l.id);
    }
}

}  // namespace pbz::modules
