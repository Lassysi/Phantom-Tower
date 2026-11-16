// L1：在主模块可执行区段做特征码扫描（"48 8B ?? ..." 格式）。
#pragma once
#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace pbz::scan {

inline bool InCode(const void* p) {
    auto* base = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + reinterpret_cast<IMAGE_DOS_HEADER*>(base)->e_lfanew);
    auto* sec = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec)
        if ((sec->Characteristics & IMAGE_SCN_MEM_EXECUTE) && p >= base + sec->VirtualAddress && p < base + sec->VirtualAddress + sec->Misc.VirtualSize)
            return true;
    return false;
}

inline void* FindCode(std::string_view aob) {
    std::vector<int> pat;
    for (size_t i = 0; i < aob.size();) {
        if (aob[i] == ' ') { ++i; continue; }
        if (aob[i] == '?') { pat.push_back(-1); i += (i + 1 < aob.size() && aob[i + 1] == '?') ? 2 : 1; continue; }
        pat.push_back(std::stoi(std::string(aob.substr(i, 2)), nullptr, 16));
        i += 2;
    }
    auto* base = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + reinterpret_cast<IMAGE_DOS_HEADER*>(base)->e_lfanew);
    auto* sec = IMAGE_FIRST_SECTION(nt);
    for (int s = 0; s < nt->FileHeader.NumberOfSections; ++s, ++sec) {
        if (!(sec->Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        uint8_t* p = base + sec->VirtualAddress;
        const size_t n = sec->Misc.VirtualSize;
        MEMORY_BASIC_INFORMATION mbi;
        for (size_t off = 0; off < n;) {
            if (!VirtualQuery(p + off, &mbi, sizeof mbi)) break;
            const size_t regionEnd = static_cast<uint8_t*>(mbi.BaseAddress) + mbi.RegionSize - p;
            const bool readable = mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS));
            if (readable)
                for (size_t i = off; i + pat.size() <= std::min(regionEnd, n); ++i) {
                    size_t k = 0;
                    while (k < pat.size() && (pat[k] < 0 || p[i + k] == pat[k])) ++k;
                    if (k == pat.size()) return p + i;
                }
            off = regionEnd;
        }
    }
    return nullptr;
}

}  // namespace pbz::scan
