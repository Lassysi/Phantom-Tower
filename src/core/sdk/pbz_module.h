// 模块 SDK：运行库（PBZCore.dll）与功能模块（影塔等独立 Mod）之间的接口。
// 模块不链接运行库：运行库加载模块时把这张函数表交给它，sdk/module_shim.cpp 用这张表实现 services/ 里的同名函数，
// 所以模块代码照常 include services/*.h、features/feature.h，写法和核心里的功能完全一样。
// 一个游戏进程只有一份运行库：修改器和影塔各自把自己的模块交给同一份运行库，共用一个属性仲裁、一套钩子账本。
//
// 约束（违反会在加载时被拒绝，或出现难查的崩溃）：
//  - 运行库和模块用同一套编译器（E:\VSBuildTools）、同样的 /MT /EHa /std:c++20 编译：接口直接传 std::string / std::function。
//    两边各带静态 CRT 也能互相释放内存：UCRT 的 malloc 走进程堆（GetProcessHeap）。
//  - 模块永不卸载（与运行库相同）。模块的功能、拦截、钩子都登记在运行库里，运行库停用时统一还原。
//  - 异常不要抛出模块边界。
#pragma once
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "../features/feature.h"
#include "../features/world/enemy_spawn.h"
#include "../services/abshook.h"
#include "../services/attributes.h"
#include "../services/inventory.h"
#include "../services/process_event.h"
#include "../services/native_exec.h"
#include "../services/ue.h"

namespace pbz::sdk {

inline constexpr uint32_t kAbi = 7;  // 7：abshook::Hook 加叠加槽（3.0.0 兼容 UE4SS）  // 6：UFunction 原生执行入口拦截  // 5：对象存活校验、异常诊断现场  // 4：传送（影塔擂台）  // 2：按标签清生成物、背包读写；3：功能持有者（影塔 M1）  // 函数表或共享类型（IFeature、attr::Target、enemies::Request…）布局一变就 +1

struct Api {
    uint32_t abi;
    uint32_t size;  // sizeof(Api)
    uint32_t msc;   // _MSC_VER / 100（编译器主版本）
    uint32_t idl;   // _ITERATOR_DEBUG_LEVEL
    // ue
    int (*ueNum)();
    ue::UObject* (*ueObj)(int);
    int (*ueObjBatch)(int, int, ue::UObject**);
    std::string (*ueName)(uint32_t);
    std::string (*ueObjName)(ue::UObject*);
    ue::UObject* (*ueClassOf)(ue::UObject*);
    std::string (*ueClassName)(ue::UObject*);
    bool (*ueIsA)(ue::UObject*, ue::UObject*);
    ue::UObject* (*ueFindClass)(std::string_view);
    int (*uePropOffset)(ue::UObject*, std::string_view);
    void (*ueForEach)(const std::function<bool(int, ue::UObject*)>&);
    bool (*ueIsDefault)(ue::UObject*);
    bool (*ueRead)(const void*, void*, size_t);
    bool (*ueWrite)(void*, const void*, size_t);
    // gamethread
    bool (*gtReady)();
    unsigned long long (*gtFrames)();
    void (*gtPost)(std::function<void()>);
    // hooks / abshook
    bool (*hkCreate)(const char*, void*, void*, void**);
    bool (*hkEnable)(const char*, bool);
    bool (*absPrepare)(abshook::Hook&, void*, void*, std::string*);
    bool (*absApply)(abshook::Hook&, bool);
    // ProcessEvent
    bool (*peReady)();
    ue::UObject* (*peFindFunction)(ue::UObject*, std::string_view);
    std::vector<ue::UObject*> (*peFunctionsWithPrefix)(ue::UObject*, std::string_view);
    void (*peCall)(ue::UObject*, ue::UObject*, void*);
    void (*peIntercept)(ue::UObject*, pe::Interceptor);
    void (*peRemoveIntercept)(ue::UObject*);
    int (*peObserve)(ue::UObject*, pe::Observer);
    void (*peUnobserve)(int);
    // 属性仲裁
    const std::vector<attr::Target>& (*atTargets)();
    bool (*atAlive)(const attr::Target&);
    int (*atAttrOffset)(const std::string&);
    int (*atActorOffset)(const std::string&);
    int (*atWorldOffset)(const std::string&);
    int (*atPlayerOffset)(const std::string&);
    float* (*atAttrCurrent)(const attr::Target&, int);
    float* (*atAttrBase)(const attr::Target&, int);
    void (*atWant)(const attr::Target&, float*, attr::Kind, float);
    void (*atHold)(const attr::Target&, float*, float);
    // 日志 / 功能登记 / 管道
    void (*logWrite)(const std::string&);
    void (*addFeature)(IFeature*);                  // 运行库接管所有权；游戏线程下一帧加入功能表
    IFeature* (*findFeature)(const std::string&);   // 仅游戏线程
    void (*ipcSend)(const std::string&);
    void (*ipcRegisterOp)(const char*, std::function<void(const std::string&)>);  // 管道 op 交给模块处理（管道线程回调）
    std::optional<std::string> (*ipcField)(const std::string&, const char*);
    std::string (*ipcUnescape)(const std::string&);
    std::string (*ipcEsc)(const std::string&);
    // 怪物生成
    void (*enSubmit)(enemies::Request, enemies::Reply);
    std::vector<ue::UObject*> (*enOwnedActors)();
    std::vector<ue::UObject*> (*enOwnedBosses)();
    std::vector<std::pair<ue::UObject*, std::string>> (*enOwnedFactions)();
    // v2
    size_t (*enClearTagged)(const std::string&);
    std::string (*invList)();
    std::string (*invApply)(const std::string&, const std::string&, long long, const std::string&);
    // v3
    bool (*holdFeature)(IFeature*, const std::string&, double);
    void (*releaseFeature)(IFeature*, const std::string&);
    // v4
    std::string (*mvBell)(const std::string&, const std::string&, const double*, double);
    bool (*mvPoint)(double, double, double, double, std::string*);
    bool (*mvBusy)();
    std::string (*mvBellLevel)(const std::string&);
    bool (*mvWhere)(double*, std::string*);
    // v5
    bool (*ueLive)(ue::UObject*);                                // 对象仍在对象表里且未销毁（旧指针安全）
    void (*diagSection)(const char*, const std::string&);       // 异常现场状态段
    void (*diagEvent)(const char*, const void*, const void*);   // 最近事件（tag 须为静态字符串）
    const char* (*diagWhere)(const char*);                      // 设置本线程"正在做什么"，返回旧值
    // v6
    bool (*nxHook)(ue::UObject*, nx::Handler, std::string*);    // UFunction 原生执行入口拦截（游戏线程）
    void (*nxUnhook)(ue::UObject*);
};

inline Api Stamp() { Api a{}; a.abi = kAbi; a.size = sizeof(Api); a.msc = _MSC_VER / 100; a.idl = _ITERATOR_DEBUG_LEVEL; return a; }

// 两边编译条件一致才允许加载；不一致时 why 写原因
inline bool Compatible(const Api& theirs, std::string* why) {
    const Api mine = Stamp();
    if (theirs.abi != mine.abi) { *why = "接口版本不一致（运行库 " + std::to_string(theirs.abi) + "，模块 " + std::to_string(mine.abi) + "）"; return false; }
    if (theirs.size != mine.size || theirs.msc != mine.msc || theirs.idl != mine.idl) { *why = "编译条件不一致，需要用同一套工具重新编译"; return false; }
    return true;
}

}  // namespace pbz::sdk

// 模块导出（由 sdk/module_shim.cpp 提供，模块作者不用写）
extern "C" {
using PbzModuleAbiFn = uint32_t (*)();
using PbzModuleIdFn = const char* (*)();
using PbzModuleStartFn = bool (*)(const pbz::sdk::Api*, char* why, int whyLen);
using PbzModuleStopFn = void (*)();
}

// 模块作者实现这三个函数
namespace pbz::module {
const char* Id();               // 模块标识，如 "tower"；同一进程同名模块只加载一次
bool Start(std::string* why);   // 管道线程调用：登记功能（AddFeature）和管道 op；失败返回 false 并写原因
void Stop();                    // 运行库停用时在游戏线程调用：清理模块自己的状态（功能会被运行库统一关闭）
}  // namespace pbz::module
