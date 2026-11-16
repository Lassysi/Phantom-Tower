// 核心实例标识（boot id）：每次注入随机生成，热换核心后一定不同。
// 用于让客户端区分"新核心从头计数的编号"和旧核心发出的编号（如物品编辑 session），避免数字相同被误认。
#pragma once
#include <cstdint>
#include <format>
#include <random>
#include <string>

#include <windows.h>

namespace pbz {

inline const std::string& BootId() {
    static const std::string id = [] {
        std::random_device rd;
        const uint64_t a = (uint64_t(rd()) << 32) ^ rd(), b = __rdtsc() ^ GetTickCount64() ^ (uint64_t(GetCurrentProcessId()) << 40) ^
                                                      reinterpret_cast<uint64_t>(&BootId);
        return std::format("{:016x}", a ^ (b * 0x9E3779B97F4A7C15ull));
    }();
    return id;
}

}  // namespace pbz
