// 示例模块（自检用）：登记一个管道 op 和一个开关功能，演示模块怎么用运行库的服务。
#include <format>

#include "../../host/log.h"
#include "../../ipc/pipe.h"
#include "../../services/gamethread.h"
#include "../pbz_module.h"

namespace {

class Ping : public pbz::IFeature {
public:
    const char* Id() const override { return "hello.ping"; }
    bool Resolve(std::string*) override { return true; }
    bool IsToggle() const override { return true; }
    void Enable() override { enabled = true; pbz::log::Info("hello.ping 开启"); }
    void Disable() override { enabled = false; pbz::log::Info("hello.ping 关闭"); }
    void Tick(float) override {}
};

}  // namespace

namespace pbz::module {

const char* Id() { return "hello"; }

bool Start(std::string*) {
    AddFeature(std::make_unique<Ping>());
    ipc::RegisterOp("hello_mod", [](const std::string& line) {
        const auto who = ipc::Unescape(ipc::Field(line, "who").value_or(""));
        ipc::Send(std::format(R"({{"op":"hello_mod","ok":true,"who":"{}","gamethread":{}}})", ipc::Esc(who), gamethread::Ready() ? "true" : "false"));
    });
    log::Info("hello 模块启动");
    return true;
}

void Stop() { log::Info("hello 模块停止"); }

}  // namespace pbz::module
