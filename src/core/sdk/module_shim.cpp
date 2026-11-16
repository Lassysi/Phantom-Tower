// 模块垫片：编进每个模块（不进运行库）。用运行库交来的函数表实现 services/ 里的同名函数，并提供模块导出。
// 只转发模块该用的部分；初始化、卸载、每帧仲裁这类运行库自己的流程不在这里，模块用到会直接链接失败。
#include <windows.h>

#include <cstring>

#include "../host/log.h"
#include "../ipc/pipe.h"
#include "../services/diag.h"
#include "../services/native_exec.h"
#include "../services/gamethread.h"
#include "../services/hooks.h"
#include "pbz_module.h"

namespace {
const pbz::sdk::Api* g_api = nullptr;
}

extern "C" __declspec(dllexport) uint32_t PbzModuleAbi() { return pbz::sdk::kAbi; }
extern "C" __declspec(dllexport) const char* PbzModuleId() { return pbz::module::Id(); }

extern "C" __declspec(dllexport) bool PbzModuleStart(const pbz::sdk::Api* api, char* why, int whyLen) {
    std::string reason;
    bool ok = false;
    if (!api) reason = "没有函数表";
    else if (pbz::sdk::Compatible(*api, &reason)) {
        g_api = api;
        try { ok = pbz::module::Start(&reason); } catch (...) { reason = "模块启动异常"; ok = false; }
    }
    if (!ok && why && whyLen > 0) strncpy_s(why, whyLen, reason.c_str(), _TRUNCATE);
    return ok;
}

extern "C" __declspec(dllexport) void PbzModuleStop() {
    if (!g_api) return;
    try { pbz::module::Stop(); } catch (...) {}
}

namespace pbz {
void AddFeature(std::unique_ptr<IFeature> f) { g_api->addFeature(f.release()); }
IFeature* FindFeature(const std::string& id) { return g_api->findFeature(id); }
bool HoldFeature(IFeature* f, const std::string& holder, double value) { return g_api->holdFeature(f, holder, value); }
void ReleaseFeature(IFeature* f, const std::string& holder) { g_api->releaseFeature(f, holder); }
std::string BellCommand(const std::string& cmd, const std::string& id, const double loc[3], double yaw) { return g_api->mvBell(cmd, id, loc, yaw); }
bool PointTeleport(double x, double y, double z, double yaw, std::string* msg) { return g_api->mvPoint(x, y, z, yaw, msg); }
bool TeleportBusy() { return g_api->mvBusy(); }
std::string BellLevel(const std::string& id) { return g_api->mvBellLevel(id); }
bool PlayerWhere(double out[4], std::string* level) { return g_api->mvWhere(out, level); }
}  // namespace pbz

namespace pbz::log {
void Write(const std::string& s) { g_api->logWrite(s); }
}

namespace pbz::ipc {
void Send(const std::string& json) { g_api->ipcSend(json); }
void RegisterOp(const char* op, std::function<void(const std::string&)> fn) { g_api->ipcRegisterOp(op, std::move(fn)); }
std::optional<std::string> Field(const std::string& line, const char* key) { return g_api->ipcField(line, key); }
std::string Unescape(const std::string& s) { return g_api->ipcUnescape(s); }
std::string Esc(const std::string& s) { return g_api->ipcEsc(s); }
}  // namespace pbz::ipc

namespace pbz::ue {
int Num() { return g_api->ueNum(); }
UObject* Obj(int i) { return g_api->ueObj(i); }
int ObjBatch(int start, int count, UObject** out) { return g_api->ueObjBatch(start, count, out); }
std::string Name(uint32_t i) { return g_api->ueName(i); }
std::string ObjName(UObject* o) { return g_api->ueObjName(o); }
UObject* ClassOf(UObject* o) { return g_api->ueClassOf(o); }
std::string ClassName(UObject* o) { return g_api->ueClassName(o); }
bool IsA(UObject* o, UObject* cls) { return g_api->ueIsA(o, cls); }
UObject* FindClass(std::string_view name) { return g_api->ueFindClass(name); }
int PropOffset(UObject* s, std::string_view prop) { return g_api->uePropOffset(s, prop); }
void ForEach(const std::function<bool(int, UObject*)>& fn) { g_api->ueForEach(fn); }
bool IsDefault(UObject* o) { return g_api->ueIsDefault(o); }
bool Read(const void* p, void* out, size_t n) { return g_api->ueRead(p, out, n); }
bool Live(UObject* o) { return g_api->ueLive(o); }
bool Write(void* p, const void* in, size_t n) { return g_api->ueWrite(p, in, n); }
}  // namespace pbz::ue

namespace pbz::gamethread {
bool Ready() { return g_api->gtReady(); }
unsigned long long Frames() { return g_api->gtFrames(); }
void Post(std::function<void()> fn) { g_api->gtPost(std::move(fn)); }
}  // namespace pbz::gamethread

namespace pbz::hooks {
bool Create(const char* name, void* target, void* detour, void** original) { return g_api->hkCreate(name, target, detour, original); }
bool Enable(const char* name, bool on) { return g_api->hkEnable(name, on); }
}  // namespace pbz::hooks

namespace pbz::abshook {
bool Prepare(Hook& h, void* target, void* detour, std::string* why) { return g_api->absPrepare(h, target, detour, why); }
bool Apply(Hook& h, bool on) { return g_api->absApply(h, on); }
}  // namespace pbz::abshook

namespace pbz::pe {
bool Ready() { return g_api->peReady(); }
ue::UObject* FindFunction(ue::UObject* cls, std::string_view name) { return g_api->peFindFunction(cls, name); }
std::vector<ue::UObject*> FunctionsWithPrefix(ue::UObject* cls, std::string_view prefix) { return g_api->peFunctionsWithPrefix(cls, prefix); }
void Call(ue::UObject* obj, ue::UObject* func, void* params) { g_api->peCall(obj, func, params); }
void Intercept(ue::UObject* func, Interceptor fn) { g_api->peIntercept(func, std::move(fn)); }
void RemoveIntercept(ue::UObject* func) { g_api->peRemoveIntercept(func); }
int Observe(ue::UObject* func, Observer fn) { return g_api->peObserve(func, std::move(fn)); }
void Unobserve(int token) { g_api->peUnobserve(token); }
}  // namespace pbz::pe

namespace pbz::attr {
const std::vector<Target>& Targets() { return g_api->atTargets(); }
bool Alive(const Target& t) { return g_api->atAlive(t); }
int AttrOffset(const std::string& a) { return g_api->atAttrOffset(a); }
int ActorOffset(const std::string& p) { return g_api->atActorOffset(p); }
int WorldOffset(const std::string& p) { return g_api->atWorldOffset(p); }
int PlayerOffset(const std::string& p) { return g_api->atPlayerOffset(p); }
float* AttrCurrent(const Target& t, int off) { return g_api->atAttrCurrent(t, off); }
float* AttrBase(const Target& t, int off) { return g_api->atAttrBase(t, off); }
void Want(const Target& t, float* field, Kind kind, float m) { g_api->atWant(t, field, kind, m); }
void Hold(const Target& t, float* field, float v) { g_api->atHold(t, field, v); }
}  // namespace pbz::attr

namespace pbz::enemies {
void Submit(Request request, Reply reply) { g_api->enSubmit(std::move(request), std::move(reply)); }
std::vector<ue::UObject*> OwnedActors() { return g_api->enOwnedActors(); }
std::vector<ue::UObject*> OwnedBosses() { return g_api->enOwnedBosses(); }
std::vector<std::pair<ue::UObject*, std::string>> OwnedFactions() { return g_api->enOwnedFactions(); }
size_t ClearTagged(const std::string& prefix) { return g_api->enClearTagged(prefix); }
}  // namespace pbz::enemies

namespace pbz::inv {
std::string List() { return g_api->invList(); }
std::string Apply(const std::string& id, const std::string& kind, long long num, const std::string& session) { return g_api->invApply(id, kind, num, session); }
}  // namespace pbz::inv

namespace pbz::nx {
bool Hook(ue::UObject* func, Handler fn, std::string* why) { return g_api->nxHook(func, std::move(fn), why); }
void Unhook(ue::UObject* func) { g_api->nxUnhook(func); }
}  // namespace pbz::nx

namespace pbz::diag {
const char* SwapWhere(const char* what) { return g_api->diagWhere(what); }
Scope::Scope(const char* what) : prev(g_api->diagWhere(what)) {}
Scope::~Scope() { g_api->diagWhere(prev); }
void Event(const char* tag, const void* obj, const void* func) { g_api->diagEvent(tag, obj, func); }
void Section(const char* key, const std::string& text) { g_api->diagSection(key, text); }
}  // namespace pbz::diag
