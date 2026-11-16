// 影塔界面里与游戏无关的小逻辑（离线可测）：
// - Lazy：按需取对象（如文本库默认对象），失效就重新找，找不到返回空——调用方据此明确退出，不拿空接收者调函数。
//   2026-10-31 实机：首次打开钟菜单时还没弹过对话框，文本库默认对象只在弹对话框时初始化，钟菜单加按钮先用到它 → 空接收者访问冲突。
// - Once：同一个钟菜单实例在一次激活里会触发两个回调（BP_OnActivated 与 Activate），只排一次加按钮。
#pragma once
#include <algorithm>
#include <vector>

namespace tower::ui {

template <class T> struct Lazy {
    T* v = nullptr;
    template <class Find, class Alive> T* Get(Find&& find, Alive&& alive) {
        if (v && !alive(v)) v = nullptr;
        if (!v) v = find();
        return v;
    }
};

struct Once {
    std::vector<const void*> pending;
    bool Add(const void* p) {  // true：第一次排队
        if (std::find(pending.begin(), pending.end(), p) != pending.end()) return false;
        pending.push_back(p);
        return true;
    }
    void Done(const void* p) { std::erase(pending, p); }
};

}  // namespace tower::ui
