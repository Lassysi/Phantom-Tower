// Included in cam.cpp's anonymous namespace. FOV belongs to the final camera
// manager, not the CineCamera lens that ATPC interpolates every frame.
#include <atomic>
#include <cstdint>
#include <cstring>

// services/hooks.h is included by cam.cpp (and the isolated FOV test) before
// this header, outside any namespace. Including it here would nest pbz::pbz
// inside the anonymous namespace.

namespace fov_hook {
using QueryFn = float(__fastcall*)(void*);

// Final-output override for one manager. The feature Tick runs after
// UGameEngine::Tick, so a LockedFOV write alone can miss the frame's camera
// update. GetFOVAngle (manager vtable 0x800) is queried during that update;
// the detour returns the user value and also writes it into the final
// CameraCachePrivate.POV.FOV, which is what the renderer reads.
struct State {
    std::atomic<bool> apply{false};
    std::atomic<float> value{0};
    std::atomic<void*> manager{nullptr};
    std::atomic<bool> hit{false};
    QueryFn original = nullptr;
    void* target = nullptr;
    int cacheFovOffset = -1;
};
inline State state;

inline float __fastcall Query(void* self) {
    if (state.apply.load(std::memory_order_relaxed) && self &&
        self == state.manager.load(std::memory_order_relaxed) && state.original) {
        const float value = state.value.load(std::memory_order_relaxed);
        if (state.cacheFovOffset >= 0)
            ue::Write(static_cast<uint8_t*>(self) + state.cacheFovOffset, &value, 4);
        if (!state.hit.exchange(true)) log::Info("cam.fov 最终 POV 覆盖命中：{:g}", value);
        return value;
    }
    return state.original ? state.original(self) : 0.0f;
}

// Build 26367565 PBZPlayerCameraManager::GetFOVAngle (39 bytes):
//   return LockedFOV > 0 ? LockedFOV : CameraCachePrivate.POV.FOV;
// The 0x2cc field and the 0x798 POV accessor are both part of the fingerprint.
inline constexpr std::uint8_t kQueryCode[] = {
    0x48, 0x83, 0xEC, 0x28,                          // sub rsp, 0x28
    0xF3, 0x0F, 0x10, 0x81, 0xCC, 0x02, 0x00, 0x00,  // movss xmm0,[rcx+0x2cc]
    0x0F, 0x57, 0xC9,                                // xorps xmm1,xmm1
    0x0F, 0x2F, 0xC1,                                // comiss xmm0,xmm1
    0x77, 0x0E,                                      // ja +0x0e
    0x48, 0x8B, 0x01,                                // mov rax,[rcx]
    0xFF, 0x90, 0x98, 0x07, 0x00, 0x00,              // call [rax+0x798]
    0xF3, 0x0F, 0x10, 0x40, 0x30,                    // movss xmm0,[rax+0x30]
    0x48, 0x83, 0xC4, 0x28,                          // add rsp, 0x28
    0xC3,                                            // ret
};
}  // namespace fov_hook

class FovFeature : public IFeature {
public:
    const char* Id() const override { return "cam.fov"; }
    bool Resolve(std::string* why) override {
        if (!attr::Resolve(why)) return false;
        auto* pawn=ue::FindClass("Pawn");
        pcClass_=ue::FindClass("PBZPlayerController");
        managerClass_=ue::FindClass("PlayerCameraManager");
        if(!pawn||!pcClass_||!managerClass_){*why="玩家镜头类型未加载";return false;}
        controllerOff_=ue::PropOffset(pawn,"Controller");
        pawnOff_=ue::PropOffset(pcClass_,"AcknowledgedPawn");
        managerOff_=ue::PropOffset(pcClass_,"PlayerCameraManager");
        if(controllerOff_<0||pawnOff_<0||managerOff_<0){*why="当前玩家镜头引用缺失";return false;}
        auto* cacheEntry=ue::FindClass("CameraCacheEntry");
        auto* view=ue::FindClass("MinimalViewInfo");
        const int cacheOff=ue::PropOffset(managerClass_,"CameraCachePrivate");
        const int povOff=cacheEntry?ue::PropOffset(cacheEntry,"POV"):-1;
        const int fovOff=view?ue::PropOffset(view,"FOV"):-1;
        if(cacheOff<0||povOff<0||fovOff<0){*why="最终镜头 POV 布局缺失";return false;}
        cacheFovOffset_=cacheOff+povOff+fovOff;
        return fov_.Resolve(pcClass_,"FOV",{{"NewFOV","FloatProperty",4}},why) &&
            target_.Resolve(pcClass_,"GetViewTarget",{{"ReturnValue","ObjectProperty",8,true}},why) &&
            look_.Resolve(pcClass_,"IsLookInputIgnored",{{"ReturnValue","BoolProperty",1,true}},why) &&
            move_.Resolve(pcClass_,"IsMoveInputIgnored",{{"ReturnValue","BoolProperty",1,true}},why) &&
            paused_.Resolve(pcClass_,"IsPaused",{{"ReturnValue","BoolProperty",1,true}},why);
    }
    void SetValue(double v) override {
        if(v==1)Release();
        value=v==1?1:res::Bounded(v,60,120);
        fov_hook::state.value.store(static_cast<float>(value));
        if(value==1)fov_hook::state.apply.store(false);
    }
    void Enable() override {enabled=true;TryHook();}
    void Disable() override {Unhook();Release();enabled=false;}
    void Tick(float) override {
        if(!enabled||value==1)return;
        ue::UObject *pawn=nullptr,*pc=nullptr,*manager=nullptr;
        for(const auto& t:attr::Targets()) {
            if(t.who!=attr::Who::Player||!attr::Alive(t))continue;
            auto* c=ue::Get<ue::UObject*>(t.obj+controllerOff_);
            if(!runtime::Identity(c).Alive()||!ue::IsA(c,pcClass_)||ue::Get<ue::UObject*>(c+pawnOff_)!=t.obj)continue;
            auto* m=ue::Get<ue::UObject*>(c+managerOff_);
            if(!runtime::Identity(m).Alive()||!ue::IsA(m,managerClass_))continue;
            if(pc){Release();return;}
            pawn=t.obj;pc=c;manager=m;
        }
        if(!pc){fov_hook::state.apply.store(false);Release();return;}
        if(pc_.object!=pc||manager_.object!=manager||!pc_.Alive()||!manager_.Alive()) {
            Release();
            int field=-1;
            if(!LockField(pc,manager,field)){available=false;enabled=false;value=1;Unhook();reason="镜头原生入口不匹配，FOV 已停用";log::Info("{}",reason);return;}
            pc_=runtime::Identity(pc);manager_=runtime::Identity(manager);lockOff_=field;
            if(!ReadLock(original_)){Release();return;}
            TryHook();
        } else if(!hooked_) {
            TryHook();
        }
        float current;
        if(!ReadLock(current)){fov_hook::state.apply.store(false);return;}
        // A native camera reset changes the restoration baseline, not the
        // user's enabled setting. Menus/cinematics yield below; normal play
        // reacquires the final manager lock without touching the cine lens.
        if(owned_&&current!=last_) {original_=current;owned_=false;}
        std::vector<uint8_t> args(target_.size);ue::UObject* view=nullptr;
        pe::Call(pc,target_.function,args.data());target_.Read(args.data(),0,view);
        const bool menu=Flag(pc,paused_)||Flag(pc,look_)||Flag(pc,move_)||view!=pawn;
        if(menu){fov_hook::state.apply.store(false);RestoreLock();return;}
        const float desired=static_cast<float>(value);
        // Publish the manager/value before enabling the override; the detour
        // runs from the native camera update, not from this feature Tick.
        fov_hook::state.value.store(desired);
        fov_hook::state.manager.store(manager);
        if(hooked_) {
            if(owned_)RestoreLock();  // drop any value left by a failed hook attempt
            fov_hook::state.apply.store(true);
            return;
        }
        fov_hook::state.apply.store(false);
        if(owned_&&current==desired)return;
        if(!owned_)original_=current;
        Set(desired);last_=desired;owned_=true;
    }
private:
    // Read-only decoding of the observed UE5.7 FOV virtual dispatch. Both
    // implementations must match, and the component offset must agree with
    // reflection. No field address or instruction is patched.
    bool LockField(ue::UObject* pc,ue::UObject* manager,int& off) {
        uint8_t a[23]{},b[9]{};
        auto* pvt=ue::Get<uint8_t*>(pc);auto* pfn=pvt?ue::Get<uint8_t*>(pvt+0xBB0):nullptr;
        if(!pfn||!ue::Read(pfn,a,sizeof a))return false;
        const uint8_t start[]={0x48,0x8B,0x89},tail[]={0x48,0x85,0xC9,0x74,0x0A,0x48,0x8B,0x01};
        // Build 26367565 emits REX.W before the same indirect jump. Decode
        // only these verified encodings; retain reflected owner/size checks.
        const int jump=a[15]==0x48?16:15;
        int field=0,slot=0;memcpy(&field,a+3,4);memcpy(&slot,a+jump+2,4);
        if(memcmp(a,start,3)||memcmp(a+7,tail,sizeof tail)||a[jump]!=0xFF||a[jump+1]!=0xA0||a[jump+6]!=0xC3||field!=managerOff_||slot<0||slot>8192||slot%8)return false;
        auto* mvt=ue::Get<uint8_t*>(manager);auto* setter=mvt?ue::Get<uint8_t*>(mvt+slot):nullptr;
        if(!setter||!ue::Read(setter,b,sizeof b))return false;
        const uint8_t store[]={0xF3,0x0F,0x11,0x89};memcpy(&off,b+4,4);
        const int size=ue::Get<int>(managerClass_+ue::Layout::StructSize,-1);
        if(memcmp(b,store,4)||b[8]!=0xC3||off<40||off>size-4)return false;
        // The query hook is the path that runs inside the camera update. It
        // both returns the user FOV and writes the final CameraCache POV, so
        // require the exact verified entry instead of silently doing nothing.
        auto* query=mvt?ue::Get<uint8_t*>(mvt+0x800):nullptr;
        uint8_t q[sizeof(fov_hook::kQueryCode)]{};
        if(!query||!ue::Read(query,q,sizeof q)||memcmp(q,fov_hook::kQueryCode,sizeof q)) {
            log::Info("cam.fov 原生 FOV 查询入口不匹配，FOV 已停用");
            return false;
        }
        if(cacheFovOffset_<0||cacheFovOffset_>size-4) {
            log::Info("cam.fov 最终镜头 POV 偏移越界，FOV 已停用");
            return false;
        }
        query_=query;
        return true;
    }
    void TryHook() {
        if(hooked_||!query_)return;
        if(!fov_hook::state.original) {
            void* original=nullptr;
            if(!hooks::Create("cam.fov.query",query_,reinterpret_cast<void*>(&fov_hook::Query),&original)||!original) {
                if(!hookLogged_) { hookLogged_=true; log::Info("cam.fov 原生 FOV 查询钩子不可用，保留锁定值回退"); }
                return;
            }
            fov_hook::state.original=reinterpret_cast<fov_hook::QueryFn>(original);
            fov_hook::state.target=query_;
        } else if(fov_hook::state.target!=query_) {
            if(!hookLogged_) { hookLogged_=true; log::Info("cam.fov 镜头查询入口变化，保留锁定值回退"); }
            return;
        }
        if(!hooks::Enable("cam.fov.query",true)) {
            if(!hookLogged_) { hookLogged_=true; log::Info("cam.fov 原生 FOV 查询启用失败，保留锁定值回退"); }
            return;
        }
        fov_hook::state.cacheFovOffset=cacheFovOffset_;
        fov_hook::state.apply.store(false);
        fov_hook::state.manager.store(nullptr);
        fov_hook::state.hit.store(false);
        hooked_=true;
        log::Info("cam.fov 最终 POV 覆盖已启用（仅本地玩家镜头；菜单/过场让出）");
    }
    void Unhook() {
        fov_hook::state.apply.store(false);
        fov_hook::state.manager.store(nullptr);
        fov_hook::state.hit.store(false);
        if(hooked_){hooks::Enable("cam.fov.query",false);hooked_=false;}
    }
    bool Flag(ue::UObject* pc,const runtime::Function& fn) {
        std::vector<uint8_t> args(fn.size);pe::Call(pc,fn.function,args.data());uint8_t out=1;
        return !fn.Read(args.data(),0,out)||out!=0;
    }
    bool ReadLock(float& value) const {return manager_.Alive()&&ue::Read(manager_.object+lockOff_,&value,4)&&std::isfinite(value)&&value>=0&&value<=180;}
    void Set(float fov) {std::vector<uint8_t> args(fov_.size);fov_.Write(args.data(),0,fov);pe::Call(pc_.object,fov_.function,args.data());}
    void RestoreLock() {
        float current;
        if(owned_&&pc_.Alive()&&ue::Get<ue::UObject*>(pc_.object+managerOff_)==manager_.object&&ReadLock(current)&&current==last_)Set(original_);
        owned_=false;
    }
    void Release(){fov_hook::state.apply.store(false);RestoreLock();pc_=runtime::Identity();manager_=runtime::Identity();lockOff_=-1;}
    runtime::Function fov_,target_,look_,move_,paused_;
    ue::UObject *pcClass_=nullptr,*managerClass_=nullptr,*query_=nullptr;
    runtime::Identity pc_,manager_;
    int controllerOff_=-1,pawnOff_=-1,managerOff_=-1,lockOff_=-1,cacheFovOffset_=-1;
    float original_=0,last_=0;
    bool owned_=false,hooked_=false,hookLogged_=false;
};
