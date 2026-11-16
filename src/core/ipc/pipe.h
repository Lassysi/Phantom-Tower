#pragma once
#include <functional>
#include <optional>
#include <string>

namespace pbz::ipc {

void Start(std::function<void()> onUnload);
void Stop();
void Send(const std::string& json);
// 纯 Mod 版（影塔安装包）运行库：PBZv2 修改器连上并发 trainer_unlock 后放开修改器功能；修改器断开后撤销并重新上锁。
// 非纯 Mod 版恒为 true
bool TrainerUnlocked();
// 运行库之外的 op（模块登记）：管道线程回调，参数是整行 JSON。同名 op 后登记的覆盖先登记的
void RegisterOp(const char* op, std::function<void(const std::string&)> fn);
// 简易 JSON 取值 / 反转义 / 转义（协议只用扁平对象）
std::optional<std::string> Field(const std::string& line, const char* key);
std::string Unescape(const std::string& s);
std::string Esc(const std::string& s);

}  // namespace pbz::ipc
