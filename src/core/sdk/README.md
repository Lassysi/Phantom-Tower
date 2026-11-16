# 运行库与功能模块

`PBZCore.dll` 是**运行库**：注入、反射、游戏线程、钩子账本、ProcessEvent、属性仲裁、怪物生成、管道，加上修改器现有的全部功能（默认都关着）。
独立 Mod（影塔等）做成**功能模块** DLL，由运行库加载，共用同一份运行库。

一个游戏进程只有一份运行库（命名互斥体保证）：只装修改器、只装影塔、两个一起开都一样，只有一个属性仲裁、一套钩子。
修改器的功能代码留在运行库里，所以影塔可以直接把它们做成"神通"道具（`FindFeature("player.auto_deflect_blue")`），不需要修改器在场。

## 加载

管道 `\\.\pipe\PBZLab`：

| 请求 | 回执 |
|---|---|
| `{"op":"module","cmd":"load","path":"E:\\…\\PBZTower.dll"}` | `{"op":"module","cmd":"load","ok":true,"id":"tower","abi":3}`；失败 `ok:false,"error":"…"` |
| `{"op":"module","cmd":"list"}` | `{"op":"module","cmd":"list","ok":true,"abi":3,"modules":[{"id":"…","path":"…"}]}` |
| `{"op":"hello"}` | 多了 `"module_abi":3`，前端用来判断运行库能不能加载自己的模块 |

前端（影塔的启动器）流程：连管道 → 连不上就注入自己包里的 `PBZCore.dll` → `hello` 看 `module_abi` → `module load`。
运行库把模块复制到 `%LOCALAPPDATA%\PBZLab\modules\` 再加载（原文件不锁，可以重新编译）。同名模块一个进程只加载一次，模块永不释放。

## 写模块

```cpp
#include "../../sdk/pbz_module.h"   // 再按需 include services/*.h、host/log.h、ipc/pipe.h
namespace pbz::module {
const char* Id() { return "tower"; }
bool Start(std::string* why) {      // 管道线程：登记功能和管道 op
    AddFeature(std::make_unique<MyFeature>());
    ipc::RegisterOp("tower", [](const std::string& line) { … });
    return true;
}
void Stop() {}                      // 运行库停用时，游戏线程，在关闭全部功能之前
}
```

编译：`src\core\sdk\build_module.bat <名字> <源码目录> [输出目录]`（默认输出 `build\core\modules\`）。示例见 `example/hello_module.cpp`，`tools/core/selftest.py` 会编译并加载它。

模块里的 IFeature 和核心里的写法完全一样：游戏线程 Enable/Disable/Tick，Disable 必须还原；运行库停用时统一关闭。

## 约束

- 运行库和模块用同一套编译器、同样的 `/MT /EHa /std:c++20` 编译（接口直接传 `std::string`、`std::function`）。加载时比对接口版本、编译器主版本、迭代器调试级别，不一致就拒绝。
- 两边各带静态 CRT 也能互相释放内存：UCRT 的 malloc 走进程堆。
- 异常不要抛出模块边界。
- 函数表（`sdk::Api`）或共享类型（`IFeature`、`attr::Target`、`enemies::Request`）的布局一变，`sdk::kAbi` 就 +1。

## 行为变化（运行库这边）

- `set` / `action` 的功能查找改到游戏线程做：模块的功能在游戏线程加入功能表，管道线程不再读功能表。回执不变，只是 `unknown_feature` 也要等一帧。
- 运行库停用时先调用各模块的 `Stop`，再清理生成的怪物、关功能、拆钩子。

## 共存（ABI 3）

- **功能持有者**：`HoldFeature(f, holder, value)` / `ReleaseFeature(f, holder)`，有任何持有者就保持开启，最后一个放手才关闭并还原。修改器界面的 `set` 用持有者 `"user"`，影塔用 `"tower"`。
- **全部还原**：只还原 `"user"` 或无持有者的功能；清怪时保留影塔的（`request_id` 以 `tower-` 开头）。
- **卸载保护**：有模块加载时，普通 `unload`（修改器关闭）被拒绝，回 `{"op":"unload","ok":false,"reason":"modules_loaded"}`；开发热换发 `{"op":"unload","force":true}` 或用备用事件。

## 还没做

- `shared/protocol.md` 补 `module` op 与 `unload` 的 `force` / 拒绝回执（shared 目录改动要和 Codex 确认）。
