// Execute the actual relocated prologue + SDK install/restore in this test process.
// No game process, hooks or gameplay calls. Counter effects stand in for camera/QTE.
#include <windows.h>
#include <array>
#include <cstdio>
#include "../src/tower_vm_bridge.h"
#include "../../../src/core/services/abshook.cpp"
extern "C" {
#include "../../../src/core/third_party/minhook/src/hde/hde64.c"
}
using Native=void(*)(uint64_t*,void*,void*);
static Native continuation=nullptr;
static bool block=false;
static int intercepted=0;
static void Gate(uint64_t* self,void* frame,void* result){++intercepted;if(!block)continuation(self,frame,result);}
// 3.0.0：模拟 UE4SS 等先挂了同一入口（FF 25 绝对跳转到它的函数，它再调原函数）
static Native foreignOriginal=nullptr;static int foreignCalls=0;
static void Foreign(uint64_t* self,void* frame,void* result){++foreignCalls;foreignOriginal(self,frame,result);}
static Native chained=nullptr;static int chainedCalls=0;
static void Chained(uint64_t* self,void* frame,void* result){++chainedCalls;chained(self,frame,result);}
int main(){
 int checks=0,fail=0;auto check=[&](bool b,const char* why){++checks;if(!b){++fail;std::printf("FAIL %s\n",why);}};
 auto* code=static_cast<uint8_t*>(VirtualAlloc(nullptr,4096,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE));
 if(!code)return 2;
 const uint8_t body[]={
  0x48,0x89,0x5c,0x24,0x20,0x56,0x57,0x41,0x56,0x48,0x83,0xec,0x70,
  0x48,0x8b,0x05,0,0,0,0,0x48,0x33,0xc4,0x48,0x89,0x44,0x24,0x60,0x48,0x8b,0x42,0x20,
  0x0f,0xb6,0x00,0x48,0x01,0x01,0x48,0x83,0xc4,0x70,0x41,0x5e,0x5f,0x5e,0x48,0x8b,0x5c,0x24,0x20,0xc3};
 std::memcpy(code,body,sizeof body);const int32_t disp=256-20;std::memcpy(code+16,&disp,4);
 const uint64_t cookie=0x12345678;std::memcpy(code+256,&cookie,8);
 DWORD old=0;VirtualProtect(code,4096,PAGE_EXECUTE_READ,&old);FlushInstructionCache(GetCurrentProcess(),code,4096);
 uint8_t opcode=5;std::array<uint8_t,256> frame{};auto* bytecode=&opcode;std::memcpy(frame.data()+0x20,&bytecode,8);
 auto call=reinterpret_cast<Native>(code);uint64_t effects=0;call(&effects,frame.data(),nullptr);
 check(effects==5,"baseline native entry executes the counter effect");
 pbz::abshook::Hook h;std::string why;
 check(towervm::bridge::Prepare(h,code,reinterpret_cast<void*>(&Gate),&why),"verified RIP prologue relocates without game RVA");
 continuation=reinterpret_cast<Native>(h.tramp);
 continuation(&effects,frame.data(),nullptr);check(effects==10,"relocated native continuation preserves stack/cookie/arguments and effect");
 check(towervm::bridge::OwnedBytes(h),"uninstalled target equals saved original bytes");
 check(pbz::abshook::Apply(h,true)&&towervm::bridge::OwnedBytes(h),"SDK freezes threads and installs exact owned bytes");
 for(int i=0;i<32;++i)call(&effects,frame.data(),nullptr);
 check(effects==170&&intercepted==32,"permitted native calls reach continuation once each");
 block=true;for(int i=0;i<32;++i)call(&effects,frame.data(),nullptr);
 check(effects==170&&intercepted==64,"blocked calls return before any counter/camera/QTE effect");
 check(pbz::abshook::Apply(h,false)&&towervm::bridge::OwnedBytes(h),"disable restores original bytes");
 call(&effects,frame.data(),nullptr);check(effects==175&&intercepted==64,"disabled entry has original behavior and no intercept calls");
 std::array<uint8_t,64> unsupported{};std::memcpy(unsupported.data(),body,sizeof body);unsupported[12]^=1;
 pbz::abshook::Hook rejected;check(!towervm::bridge::Prepare(rejected,unsupported.data(),reinterpret_cast<void*>(&Gate),&why)&&!rejected.target,"different prologue fails closed without patching");
 std::array<uint8_t,128> wrapper{};
 const uint8_t prefix[]={0x48,0x89,0x5c,0x24,0x08,0x48,0x89,0x6c,0x24,0x10,0x48,0x89,0x74,0x24,0x18,0x57,0x41,0x56,0x41,0x57,0x48,0x83,0xec,0x30,0x4c,0x8b,0x72,0x10};
 const uint8_t site[]={0x4c,0x8b,0xc7,0x48,0x8b,0xd5,0x48,0x8b,0xcb,0xe8};
 std::memcpy(wrapper.data(),prefix,sizeof prefix);std::memcpy(wrapper.data()+0x66,site,sizeof site);
 const int32_t relative=0x400;std::memcpy(wrapper.data()+0x70,&relative,4);
 check(reinterpret_cast<uintptr_t>(towervm::bridge::Interpreter(wrapper.data()))==reinterpret_cast<uintptr_t>(wrapper.data())+0x474,"wrapper's verified relative call resolves interpreter");
 wrapper[0x69]^=1;check(!towervm::bridge::Interpreter(wrapper.data()),"changed call-site ABI refuses resolution");
 wrapper[0x69]^=1;
 // 包装入口被别的 Mod 改成跳转：前 15 字节不再核对，调用点仍要对上
 wrapper[0]=0xff;wrapper[1]=0x25;std::memset(wrapper.data()+2,0,12);
 check(reinterpret_cast<uintptr_t>(towervm::bridge::Interpreter(wrapper.data()))==reinterpret_cast<uintptr_t>(wrapper.data())+0x474,"hooked wrapper still resolves through its intact call site");
 // 叠加挂接：入口是别人的 FF 25 跳转 → 只换跳转槽，原函数 = 别人的入口
 auto* hooked=static_cast<uint8_t*>(VirtualAlloc(nullptr,4096,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE));
 if(!hooked)return 2;
 foreignOriginal=call;
 hooked[0]=0xff;hooked[1]=0x25;std::memset(hooked+2,0,4);void* to=reinterpret_cast<void*>(&Foreign);std::memcpy(hooked+6,&to,8);
 VirtualProtect(hooked,4096,PAGE_EXECUTE_READ,&old);FlushInstructionCache(GetCurrentProcess(),hooked,4096);
 auto viaForeign=reinterpret_cast<Native>(hooked);
 const uint64_t before=effects;viaForeign(&effects,frame.data(),nullptr);
 check(foreignCalls==1&&effects==before+5,"foreign hook alone runs the original");
 pbz::abshook::Hook chain;
 check(towervm::bridge::Prepare(chain,hooked,reinterpret_cast<void*>(&Chained),&why)&&chain.slot==hooked+6&&chain.prev==to,"foreign jump becomes a slot chain");
 chained=reinterpret_cast<Native>(chain.tramp);
 check(pbz::abshook::Apply(chain,true)&&towervm::bridge::OwnedBytes(chain),"chain install swaps only the slot");
 viaForeign(&effects,frame.data(),nullptr);
 check(chainedCalls==1&&foreignCalls==2&&effects==before+10,"chained call runs ours, then the other mod, then the original");
 check(pbz::abshook::Apply(chain,false)&&towervm::bridge::OwnedBytes(chain),"chain removal restores the other mod's address");
 viaForeign(&effects,frame.data(),nullptr);
 check(chainedCalls==1&&foreignCalls==3,"after removal only the other mod remains");
 check(pbz::abshook::LooksHooked(hooked)&&!pbz::abshook::LooksHooked(code),"foreign jump detection");
 // Trampoline intentionally remains resident, matching production in-flight lifetime.
 VirtualFree(code,0,MEM_RELEASE);
 std::printf("TOWER_VM_BRIDGE_%s %d checks (native execution + restore)\n",fail?"FAILED":"OK",checks);return fail?1:0;
}
