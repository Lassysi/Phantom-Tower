#include <cstdio>
#include <cstdlib>
#include "../src/tower_growth_run.h"
#include "../src/tower_floorplay.h"
#include "../src/tower_mobs.h"
static void Check(bool ok, const char* what) { if (!ok) { std::printf("FAIL %s\n", what); std::exit(1); } }
static bool Same(const tower::rewards::Offer& a, const tower::rewards::Offer& b) { return a.kind==b.kind && a.index==b.index && a.amount==b.amount && a.quality==b.quality; }
int main() {
    namespace rw=tower::rewards;
    for (uint64_t seed : {1ull, 2204130ull, 0xFFFFFFFFFFFFFFFEull}) for (int difficulty : {0,1}) {
        tower::growthrun::Runner a,b;
        a.profile=b.profile=tower::growth::Profile::Kit(); a.catalog=b.catalog=rw::Contract(); a.difficulty=b.difficulty=difficulty;
        std::array<int,tower::kItemCount> items{};
        for (int f=1; f<=50; ++f) {
            auto bossA=tower::floorplay::ScheduledFloor(seed,f,true), bossB=tower::floorplay::ScheduledFloor(seed,f,true);
            Check(bossA.bosses[0]==bossB.bosses[0] && bossA.replay==bossB.replay,"same planned encounter");
            if (f>=2) for (int w=0;w<tower::mobs::WaveCount(f);++w) {
                auto plan=tower::mobs::Plan(f,w,tower::mobs::WaveCount(f)); int zone=(f-1)/10+1;
                Check(tower::mobs::Pick(seed,f,w,zone,plan,[](int){return false;})==tower::mobs::Pick(seed,f,w,zone,plan,[](int){return false;}),"same wave without runtime failures");
            }
            a.DrawRewards(f,50,seed,items); b.DrawRewards(f,50,seed,items);
            for(int i=0;i<3;++i) Check(Same(a.offers[i],b.offers[i]),"same reward options");
            Check(a.refresh==b.refresh,"same refresh availability");
            auto selected=a.offers[0];
            if(selected.kind==rw::Kind::Attr) { a.attrs.Add(selected.index,selected.amount); b.attrs.Add(selected.index,selected.amount); }
            a.lastGroup.assign(a.offers.begin(),a.offers.end()); b.lastGroup.assign(b.offers.begin(),b.offers.end());
            a.OpenShop(f,50,seed,items); b.OpenShop(f,50,seed,items);
            Check(a.shop.shelf.size()==b.shop.shelf.size(),"same shop size");
            for(size_t i=0;i<a.shop.shelf.size();++i) Check(Same(a.shop.shelf[i].offer,b.shop.shelf[i].offer)&&a.shop.shelf[i].price==b.shop.shelf[i].price,"same stock and prices");
            if(f==25) {
                tower::growthrun::Runner resumed; Check(tower::growthrun::Runner::Load(a.Save(),&resumed),"resume parses saved reward state");
                resumed.catalog=rw::Contract(); resumed.difficulty=difficulty; a=std::move(resumed);
            }
        }
    }
    std::puts("TOWER_SEED_REPRO_OK");
}
