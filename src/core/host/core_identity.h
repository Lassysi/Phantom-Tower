// Read the exact loaded core's file once; never confuse a new UI with an old
// resident DLL. This runs on the IPC thread, never in a gameplay callback.
#pragma once
#include <windows.h>
#include <bcrypt.h>
#include <array>
#include <cstdint>
#include <format>
#include <string>

namespace pbz {
inline const std::string& LoadedCoreSha256() {
    static const std::string hash = []() -> std::string {
        HMODULE module = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCWSTR>(&LoadedCoreSha256), &module)) return {};
        std::array<wchar_t,32768> path{};
        const auto length = GetModuleFileNameW(module,path.data(),static_cast<DWORD>(path.size()));
        if (!length || length>=path.size()) return {};
        HANDLE file = CreateFileW(path.data(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);
        if (file==INVALID_HANDLE_VALUE) return {};
        BCRYPT_ALG_HANDLE algorithm=nullptr;
        BCRYPT_HASH_HANDLE state=nullptr;
        bool ok = BCryptOpenAlgorithmProvider(&algorithm,BCRYPT_SHA256_ALGORITHM,nullptr,0)==0 &&
            BCryptCreateHash(algorithm,&state,nullptr,0,nullptr,0,0)==0;
        std::array<uint8_t,65536> buffer{};
        while(ok) {
            DWORD got=0;
            if(!ReadFile(file,buffer.data(),static_cast<DWORD>(buffer.size()),&got,nullptr)){ok=false;break;}
            if(!got)break;
            ok=BCryptHashData(state,buffer.data(),got,0)==0;
        }
        std::array<uint8_t,32> digest{};
        if(ok)ok=BCryptFinishHash(state,digest.data(),static_cast<ULONG>(digest.size()),0)==0;
        if(state)BCryptDestroyHash(state);
        if(algorithm)BCryptCloseAlgorithmProvider(algorithm,0);
        CloseHandle(file);
        std::string result;
        if(ok)for(auto byte:digest)result+=std::format("{:02x}",byte);
        return result;
    }();
    return hash;
}
} // namespace pbz
