// Pure map-switch preload is not permission to play or finish a cinematic.
// The admitted CFG fragments are audited cooked code and verified against the
// live script bytes, including every relocated object/field/FName operand.
#pragma once
#include <algorithm>
#include <cstring>
#include <span>
#include <string>
#include <vector>
#include "preload_contract.generated.h"

namespace towerpreload {
template<class Resolve>
bool Match(const contract::Segment& s,std::span<const uint8_t> bytes,Resolve&& resolve) {
    if(bytes.size()!=s.bytes.size())return false;
    int ref=0;
    for(size_t i=0;i<bytes.size();) {
        if(ref<s.refN && s.refs[ref].at==static_cast<int>(i)) {
            if(i+8>bytes.size() || (ref+1<s.refN && s.refs[ref+1].at<static_cast<int>(i+8)))return false;
            uint64_t value=0;std::memcpy(&value,bytes.data()+i,8);
            if(!resolve(s.refs[ref],value))return false;
            ++ref;i+=8;
        } else {
            if(ref<s.refN && s.refs[ref].at<static_cast<int>(i))return false;
            if(bytes[i]!=static_cast<uint8_t>(s.bytes[i]))return false;
            ++i;
        }
    }
    return ref==s.refN;
}
inline bool GraphEntry(int entry) {
    return std::find(std::begin(contract::graphEntries),std::end(contract::graphEntries),entry)!=std::end(contract::graphEntries);
}
}
