// 新输入门（1–32 层第 4 层：奖励框弹出 158 ms 就被选走，击杀时连按的攻击键穿透到了对话框）。纯逻辑，离线测试。
// 规则：对话框打开后，确认类输入（键盘 / 鼠标 / 手柄）先要"安静"一段时间——打开至少 kMinOpenMs，且最近 kQuietMs 内没有按下——
// 才算武装；武装之后的点击才算选择，武装前的点击吞掉（对话框留着）。武装是粘的：玩家正常按下选择那一下不会把门关回去。
// 打开前就按住的键：一直按着就一直不安静，松开后过 kQuietMs 才武装。连击同理。
#pragma once
#include <cstdint>

namespace tower::inputgate {

struct Gate {
    static constexpr unsigned long long kMinOpenMs = 400, kQuietMs = 300;
    unsigned long long openedAt = 0, lastDown = 0;
    bool open = false, armed = false;

    void Open(unsigned long long now, uint32_t down) {
        open = true; armed = false; openedAt = now;
        lastDown = down ? now : 0;
    }
    void Close() { open = false; armed = false; }
    // 每帧采样（游戏线程）；down = 当前按住的确认类输入位图
    void Sample(unsigned long long now, uint32_t down) {
        if (!open || armed) return;
        // 先看"这次采样之前"是否已经安静：安静之后的这一下按键就是新输入（武装后它松开时的点击照样算）
        if (now - openedAt >= kMinOpenMs && (lastDown == 0 || now - lastDown >= kQuietMs)) { armed = true; return; }
        if (down) lastDown = now;
    }
    // 点击到达：先按当前输入采样一次，再看门是否已武装
    bool Accept(unsigned long long now, uint32_t down) {
        if (!open) return true;
        Sample(now, down);
        return armed;
    }
};

}  // namespace tower::inputgate
