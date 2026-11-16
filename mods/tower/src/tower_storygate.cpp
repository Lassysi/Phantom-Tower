#include "tower_storygate.h"
#include <algorithm>
#include <cstring>
#include <unordered_set>
#include <unordered_map>
#include <vector>
#include <string_view>
#include "../../../src/core/sdk/pbz_module.h"
#include "../../../src/core/host/log.h"
#include "tower_perf.h"
#include "tower_travel_api.h"
#include "tower_vm_gate.h"
#include "terminal_flows.generated.h"
#include "tower_preload_runtime.h"

namespace towerstorygate {
namespace {
namespace ue=pbz::ue;
namespace pe=pbz::pe;
using ue::UObject;
bool active=false;
const tower::replay::Contract* replay=nullptr;
UObject* nodeClass=nullptr;
UObject* inputFn=nullptr;
UObject* graphFn=nullptr;
bool vmInput=false,vmGraph=false,flowRouting=false;
int pinOffset=-1, softOffset=-1, loadedOffset=-1;
int nodeGuidOffset=-1,assetGuidOffset=-1,templateOffset=-1;
UObject* flowAssetClass=nullptr;
std::vector<UObject*> hooks;
std::vector<UObject*> vmHooks;
struct NodeStamp{UObject* cls;UObject* outer;tower::terminal::Guid guid;bool operator==(const NodeStamp&)const=default;};
NodeStamp Stamp(UObject* node){return {ue::ClassOf(node),ue::Get<UObject*>(node+ue::Layout::ObjOuter),nodeGuidOffset>=0?ue::Get<tower::terminal::Guid>(node+nodeGuidOffset):tower::terminal::Guid{}};}
struct NodeSet{
    std::unordered_map<UObject*,NodeStamp> nodes;
    std::pair<UObject*,bool> insert(UObject* node){const auto now=Stamp(node);auto [i,fresh]=nodes.try_emplace(node,now);if(!fresh && i->second!=now){i->second=now;fresh=true;}return {node,fresh};}
    size_t count(UObject* node)const{const auto i=nodes.find(node);return i!=nodes.end() && node && ue::Live(node) && i->second==Stamp(node)?1:0;}
    void clear(){nodes.clear();}
};
NodeSet parked, loggedAllow, loggedBlock;
int entryCursor=0;
std::vector<UObject*> entryMatches;
// ABI6 Core's Name() deliberately truncates UTF-16 names to low bytes. Flow
// inputs are Chinese, so resolve their native FNames once and compare all 64
// bits. Keep Core/ABI unchanged and never infer a pin from a lossy string.
uint64_t playPinName=0, interruptPinName=0;
bool ResolvePinNames() {
    if(playPinName && interruptPinName && playPinName!=interruptPinName)return true;
    auto* cls=towerperf::ClassNamed("KismetStringLibrary");
    auto* fn=cls?pe::FindFunction(cls,"Conv_StringToName"):nullptr;
    const int input=fn?ue::PropOffset(fn,"InString"):-1;
    const int result=fn?ue::PropOffset(fn,"ReturnValue"):-1;
    if(input<0 || input+16>64 || result<0 || result+8>64 || !(input+16<=result || result+8<=input))return false;
    UObject* cdo=nullptr;
    ue::ForEach([&](int,UObject* obj){if(ue::Live(obj) && ue::ClassOf(obj)==cls && ue::IsDefault(obj)){cdo=obj;return false;}return true;});
    if(!cdo)return false;
    const auto make=[&](const wchar_t* text)->uint64_t {
        const auto count=static_cast<int32_t>(std::wstring_view(text).size()+1);
        struct Str {const wchar_t* data;int32_t num,max;};
        const Str value{text,count,count};
        std::array<uint8_t,64> args{};
        std::memcpy(args.data()+input,&value,sizeof value);
        pe::Call(cdo,fn,args.data()); // game thread; only FString->FName, no allocated output
        return ue::Get<uint64_t>(args.data()+result);
    };
    const auto play=make(L"播放"),interrupt=make(L"中断");
    if(!play || !interrupt || play==interrupt || (play>>32) || (interrupt>>32))return false;
    playPinName=play;interruptPinName=interrupt;
    pbz::log::Info("影塔原生入场：中文输入FName已核验 播放={} 中断={}（完整FName比较）",playPinName,interruptPinName);
    return true;
}
bool Travelling();
bool LayoutReady(){return flowRouting && flowAssetClass && ue::Live(flowAssetClass) && nodeGuidOffset>=0 && assetGuidOffset>=0 && templateOffset>=0;}
bool Terminal(UObject* node) {
    if(!node || nodeGuidOffset<0 || assetGuidOffset<0 || templateOffset<0)return false;
    auto* asset=ue::Get<UObject*>(node+ue::Layout::ObjOuter);
    if(!asset || !ue::Live(asset) || !flowAssetClass || !ue::IsA(asset,flowAssetClass))return false;
    auto* source=ue::Get<UObject*>(asset+templateOffset);
    if(source && ue::Live(source))asset=source;
    const auto nodeGuid=ue::Get<tower::terminal::Guid>(node+nodeGuidOffset);
    const auto assetGuid=ue::Get<tower::terminal::Guid>(asset+assetGuidOffset);
    const auto* rule=tower::terminal::Find(assetGuid,nodeGuid);
    if(!rule)return false;
    parked.insert(node);
    if(loggedBlock.insert(node).second)pbz::log::Info("影塔战后流程：冻结首节点 flow={} node={} class={}（位移/台词/CG/QTE/任务/发奖前阻断）",rule->flow,ue::ObjName(node),ue::ClassName(node));
    return true;
}
towervm::Action Flow(UObject* node,UObject* fn,void*) {
    if(!active || Travelling() || !node || !ue::Live(node) || !fn || ue::PropOffset(fn,"ReturnValue")>=0)return towervm::Action::Pass;
    return Terminal(node)?towervm::Action::Block:towervm::Action::Pass;
}

bool Travelling() { return towertravel::Busy() || pbz::TeleportBusy(); }
std::string Sequence(UObject* node) {
    // Build26266538/UE5.7: FSoftObjectPtr stores the FName asset at +0x10.
    // Read-only pinned samples confirm e.g. unloaded QuanChiDeath node -> Seq031.
    // The actor's property offset is reflected; no object/native addresses are hardcoded.
    const auto name=softOffset>=0 ? ue::Name(ue::Get<uint32_t>(node+softOffset+0x10)) : std::string();
    if (!name.empty() && name!="None") return name;
    auto* loaded=loadedOffset>=0 ? ue::Get<UObject*>(node+loadedOffset) : nullptr;
    return loaded && ue::Live(loaded) ? ue::ObjName(loaded) : std::string();
}
void RemoveHooks() {
    for (auto* fn:hooks) pe::RemoveIntercept(fn);
    for (auto* fn:vmHooks) towervm::Remove(fn);
    hooks.clear();vmHooks.clear();nodeClass=nullptr;inputFn=graphFn=nullptr;vmInput=vmGraph=false;pinOffset=softOffset=loadedOffset=-1;
}
bool Input(UObject* node, void* params) {
    if (!active || Travelling() || !node || !ue::Live(node) || !params || pinOffset<0) return false;
    if(!playPinName || !interruptPinName)return true;
    const auto pin=ue::Get<uint64_t>(static_cast<uint8_t*>(params)+pinOffset);
    if(pin==interruptPinName && parked.count(node))return true; // never-started frozen nodes cannot emit native completion through cleanup
    if (pin!=playPinName) return false; // Keep the native interrupt/cleanup path.
    const auto seq=Sequence(node);
    if (tower::replay::AllowSequence(replay,seq)) {
        if (loggedAllow.insert(node).second)
            pbz::log::Info("影塔剧情入口：放行 seq={} node={} class={} pin={}（本层遭遇契约）",seq,ue::ObjName(node),ue::ClassName(node),pin);
        return false;
    }
    parked.insert(node);
    if (loggedBlock.insert(node).second)
        pbz::log::Info("影塔剧情入口：冻结 seq={} node={} class={} pin={}（未进入蓝图播放链；不补完成回调）",seq.empty()?"未知，按未声明处理":seq,ue::ObjName(node),ue::ClassName(node),pin);
    return true;
}
towervm::Action Script(UObject* node,UObject* fn,void* params) {
    using towervm::Action;
    if(!active || Travelling() || !node || !ue::Live(node) || !fn || ue::PropOffset(fn,"ReturnValue")>=0)return Action::Pass;
    auto* cls=ue::ClassOf(node);
    bool sceneNode=nodeClass && ue::IsA(node,nodeClass);
    for(auto* parent=cls;!sceneNode && parent;parent=ue::Get<UObject*>(parent+ue::Layout::StructSuper)){
        const auto n=ue::ObjName(parent);
        if(n=="PlayLevelSequence_C" || n=="PlayLevelSequence_Simple_C" || n=="PlayLevelSequence_ReUse_C" || n=="PlayLevelSequence_ReUseNew_C" || n=="PlayLevelSequence_ReUseBell_C" || n=="PlayLevelSequenceEx_C"){sceneNode=true;break;}
    }
    if(!sceneNode)return Action::Pass;
    // Map-switch prewarm can finish after TeleportBusy has cleared. Its own
    // async/latent callbacks must release native loading, even for an unrelated
    // parked scene. This grants only verified pure preload CFG entries; camera,
    // play, actor spawn and Flow completion remain behind the encounter gate.
    auto* preloadBase=nodeClass?nodeClass:towerperf::ClassNamed("PlayLevelSequence_C");
    if(towerpreload::Allows(preloadBase,fn,params))return Action::Pass;
    if(Terminal(node))return Action::Block;
    const auto method=ue::ObjName(fn);
    // Construction/editor/preload bookkeeping is not a request to play. Blocking
    // it would leave dynamic nodes uninitialized before their first input.
    if(method=="K2_InitializeInstance" || method=="K2_DeinitializeInstance" || method=="K2_OnPostEditChange" || method=="K2_PreloadContent" || method=="K2_FlushContent")return Action::Pass;
    if(method=="K2_Cleanup")return parked.count(node)?Action::Block:Action::Cleanup;
    const int pinField=fn==inputFn?pinOffset:ue::PropOffset(fn,"PinName");
    if((fn==inputFn || method=="K2_ExecuteInput") && params && pinField>=0){
        if(!playPinName || !interruptPinName)return Action::Block;
        const auto pin=ue::Get<uint64_t>(static_cast<uint8_t*>(params)+pinField);
        if(pin==interruptPinName)return parked.count(node)?Action::Block:Action::Cleanup; // frozen nodes never started; no native completion/cleanup chain to run
        if(pin!=playPinName)return Action::Pass;
    }
    // Class fallback protects its very first VM call before the normal tick has
    // discovered a just-loaded node class; it doesn't install hooks recursively.
    const int sceneField=cls==nodeClass?softOffset:ue::PropOffset(cls,"LevelSequence");
    const auto seq=sceneField>=0?ue::Name(ue::Get<uint32_t>(node+sceneField+0x10)):std::string();
    if(!parked.count(node) && tower::replay::AllowSequence(replay,seq)) {
        if(loggedAllow.insert(node).second)pbz::log::Info("影塔剧情入口：字节码执行前放行 seq={} node={} function={}（遭遇契约）",seq,ue::ObjName(node),ue::ObjName(fn));
        return Action::Pass;
    }
    parked.insert(node);
    if(loggedBlock.insert(node).second)pbz::log::Info("影塔剧情入口：字节码执行前冻结 seq={} node={} function={}（镜头/QTE/加载/完成输出均未执行）",seq.empty()?"未知":seq,ue::ObjName(node),ue::ObjName(fn));
    return Action::Block;
}
void Install() {
    if (!active) return;
    ResolvePinNames();
    if(!flowRouting || !LayoutReady()) {
        auto* base=towerperf::ClassNamed("FlowNode");
        auto* flow=towerperf::ClassNamed("FlowAsset");
        flowAssetClass=flow;
        nodeGuidOffset=base?ue::PropOffset(base,"NodeGuid"):-1;
        assetGuidOffset=flow?ue::PropOffset(flow,"AssetGuid"):-1;
        templateOffset=flow?ue::PropOffset(flow,"TemplateAsset"):-1;
        auto* seed=base?pe::FindFunction(base,"K2_ExecuteInput"):nullptr;
        std::string why;
        if(seed && towervm::Add(seed,[](UObject*,UObject*,void*){return towervm::Action::Pass;},&why)) {
            towervm::Class("FlowNode",Flow);
            for(const char* name:{"PlayLevelSequence_C","PlayLevelSequence_Simple_C","PlayLevelSequence_ReUse_C","PlayLevelSequence_ReUseNew_C","PlayLevelSequence_ReUseBell_C","PlayLevelSequenceEx_C"})towervm::Class(name,Script);
            flowRouting=true;
        }
    }
    auto* cls=towerperf::ClassNamed("PlayLevelSequence_C");
    if (!cls) return;
    // Prove the preload profile when the scene class is installed, before a
    // user starts another floor. A failed proof is explicit in the log rather
    // than silently discovered only at an asynchronous loading callback.
    towerpreload::runtime::Ready(cls);
    if (cls==nodeClass && inputFn && ue::Live(inputFn) && vmInput && vmGraph && towervm::Ready() && LayoutReady() && pinOffset>=0 && softOffset>=0) return;
    RemoveHooks();
    auto* fn=pe::FindFunction(cls,"K2_ExecuteInput");
    const int pin=fn ? ue::PropOffset(fn,"PinName") : -1;
    const int soft=ue::PropOffset(cls,"LevelSequence");
    if (!fn || pin<0 || soft<0) return;
    nodeClass=cls;inputFn=fn;pinOffset=pin;softOffset=soft;
    graphFn=pe::FindFunction(cls,"ExecuteUbergraph_PlayLevelSequence");
    loadedOffset=ue::PropOffset(cls,"LoadedLevelSequence");
    pe::Intercept(fn,Input);hooks.push_back(fn);
    // The live R3 sample proved ProcessEvent had no callbacks for actual CGs.
    // Guard the interpreter boundary for all void node script functions, including
    // local ubergraph/async callbacks, before they execute any Blueprint instruction.
    for(auto* script:pe::FunctionsWithPrefix(cls,"")) {
        if(ue::PropOffset(script,"ReturnValue")>=0)continue;
        std::string why;
        if(towervm::Add(script,Script,&why)){
            vmHooks.push_back(script);vmInput|=script==inputFn;vmGraph|=script==graphFn;
        } else if(script==inputFn || script==graphFn)pbz::log::Info("影塔剧情入口：真实蓝图执行门不可用 {}（{}）",ue::ObjName(script),why);
    }
    // If another path queued a callback for a parked node, it must not advance
    // into quest/loot/autosave. No native-exec slots or VM argument parsing needed.
    for (const char* name:{"OnLevelSequenceFinished","OnFinished","FinishLevelSequence"})
        if (auto* callback=pe::FindFunction(cls,name)) {
            pe::Intercept(callback,[](UObject* node,void*) { return active && parked.count(node)>0; });
            hooks.push_back(callback);
        }
    pbz::log::Info("影塔剧情入口：反射门已挂，真实蓝图门 input={} graph={}（PinName={} LevelSequence={}；实机触发以字节码执行日志为准）",vmInput,vmGraph,pinOffset,softOffset);
}
}
void Session(bool inside) {
    if (!inside) {
        active=false;
        RemoveHooks();towervm::End();
        active=false;replay=nullptr;parked.clear();loggedAllow.clear();loggedBlock.clear();entryCursor=0;entryMatches.clear();
        towerpreload::Reset();
        flowAssetClass=nullptr;flowRouting=false;nodeGuidOffset=assetGuidOffset=templateOffset=-1;
        return;
    }
    active=true;Install();
}
void SetReplay(const tower::replay::Contract* c) { if(replay!=c){entryCursor=0;entryMatches.clear();}replay=c; }
bool Protected(std::string* why){
    Install();
    if(active && towervm::Ready() && LayoutReady() && playPinName && interruptPinName)return true;
    if(why)*why="剧情真实执行门或精确Flow身份尚未就绪，暂不开战（不使用播放后跳停代替）";
    return false;
}
bool Ready(std::string* why) {
    Install();
    if (active && inputFn && ue::Live(inputFn) && vmInput && vmGraph && towervm::Ready() && LayoutReady() && playPinName && interruptPinName && pinOffset>=0 && softOffset>=0) return true;
    if (why) *why="原生剧情真实执行入口尚未就绪（蓝图解释门 / 输入 / Ubergraph / Flow身份布局）；未开遭遇，不发奖";
    return false;
}
bool HadCinematic(){return !loggedAllow.nodes.empty();}
Entry Start(const tower::replay::Contract& c,std::string* why){
    if(c.entryAsset==tower::terminal::Guid{})return Entry::Started;
    if(!Ready(why))return Entry::Waiting;
    auto eligible=[&](UObject* node){
        if(!node || !ue::Live(node) || ue::IsDefault(node))return false;
        bool scene=ue::IsA(node,nodeClass);  // 28 层大师兄入场是 PlayLevelSequenceEx_C，不一定派生自 PlayLevelSequence_C
        for(auto* p=ue::ClassOf(node);!scene && p;p=ue::Get<UObject*>(p+ue::Layout::StructSuper))scene=ue::ObjName(p)=="PlayLevelSequenceEx_C";
        if(!scene)return false;
        if(ue::Get<tower::terminal::Guid>(node+nodeGuidOffset)!=c.entryNode)return false;
        auto* asset=ue::Get<UObject*>(node+ue::Layout::ObjOuter);
        if(!asset || !ue::Live(asset) || !ue::IsA(asset,flowAssetClass))return false;
        auto* source=ue::Get<UObject*>(asset+templateOffset);
        if(!source || !ue::Live(source) || ue::Get<tower::terminal::Guid>(source+assetGuidOffset)!=c.entryAsset)return false;
        auto* isActive=pe::FindFunction(flowAssetClass,"IsActive");
        const int ret=isActive?ue::PropOffset(isActive,"ReturnValue"):-1;
        if(!isActive || ret<0 || ret>=16)return false;
        std::array<uint8_t,16> args{};pe::Call(asset,isActive,args.data());
        return args[ret]!=0;
    };
    static UObject* batch[4096];
    const int total=ue::Num(),count=ue::ObjBatch(entryCursor,4096,batch);
    for(int i=0;i<count;++i){
        auto* node=batch[i];
        if(eligible(node))entryMatches.push_back(node);
    }
    entryCursor+=4096;
    if(entryCursor<total)return Entry::Waiting;
    entryCursor=0;
    std::vector<UObject*> matches;matches.swap(entryMatches);
    std::erase_if(matches,[&](UObject* node){return !eligible(node);}); // loading/GC can change the active instance during a multi-tick scan
    if(matches.empty()){if(why)*why="未找到唯一已激活的原生入场Flow实例";return Entry::Waiting;}
    if(matches.size()!=1){if(why)*why="多个原生入场Flow实例同时激活，未重复触发，请保留现场取证";return Entry::Failed;}
    auto* node=matches[0];
    const int hasPlayed=ue::PropOffset(ue::ClassOf(node),"HasPlay");
    if(hasPlayed>=0 && ue::Get<uint8_t>(node+hasPlayed))return Entry::Started;
    const int pins=ue::PropOffset(ue::ClassOf(node),"InputPins");
    auto* pinClass=towerperf::ClassNamed("FlowPin");
    const int pinName=pinClass?ue::PropOffset(pinClass,"PinName"):-1;
    const int stride=pinClass?ue::Get<int32_t>(pinClass+ue::Layout::StructSize):0;
    struct Arr{uint8_t* data;int32_t num,max;};
    const auto list=pins>=0?ue::Get<Arr>(node+pins):Arr{};
    if(!list.data || list.num<1 || list.num>16 || stride<8 || stride>256 || pinName<0 || pinName+8>stride){if(why)*why="原生入场节点Pin布局未验证";return Entry::Failed;}
    uint64_t play=0;
    for(int i=0;i<list.num;++i){const auto name=ue::Get<uint64_t>(list.data+i*stride+pinName);if(name==playPinName){if(play){if(why)*why="原生入场节点有重复播放输入，未启动";return Entry::Failed;}play=name;}}
    auto* fn=pe::FindFunction(ue::ClassOf(node),"K2_ExecuteInput");
    const int off=fn?ue::PropOffset(fn,"PinName"):-1;
    if(!play || !fn || off<0 || off+8>64){if(why)*why="原生入场节点播放输入未就绪";return Entry::Failed;}
    std::array<uint8_t,64> args{};std::memcpy(args.data()+off,&play,8);
    pbz::log::Info("影塔原生入场：执行唯一已激活实例 node={} class={} contract={}（原生节点准备/绑定/演出）",ue::ObjName(node),ue::ClassName(node),c.id);
    pe::Call(node,fn,args.data());
    return Entry::Started;
}
bool Cinematic(){
    if(!active || !replay)return false;
    auto* playerClass=towerperf::ClassNamed("MovieSceneSequencePlayer");
    const int status=playerClass?ue::PropOffset(playerClass,"Status"):-1;
    if(status<0)return false;
    for(const auto& [node,stamp]:loggedAllow.nodes){
        if(!loggedAllow.count(node))continue;
        const int off=ue::PropOffset(ue::ClassOf(node),"SequencePlayer");
        auto* player=off>=0?ue::Get<UObject*>(node+off):nullptr;
        if(!player || !ue::Live(player) || !ue::IsA(player,playerClass))continue;
        const uint8_t state=ue::Get<uint8_t>(player+status);
        if(state==1 || state==2)return true; // playing or QTE-paused
    }
    return false;
}
} // namespace towerstorygate
