// Included in progress_features.cpp's anonymous namespace.
// 升级无消耗：开启时把升级 / 铸魂 / 修行表里的材料数量和意识尘埃消耗写成 0，关闭时逐项写回。
// 改的是游戏内存里已加载的数据表（DT_WeaponData 等），存档不受影响；游戏每次升级都现查表，所以即时生效。
class UpgradeFree : public IFeature {
    struct ObjectId {
        ue::UObject *object=nullptr,*cls=nullptr,*outer=nullptr;int index=-1;uint64_t name=0;
        explicit ObjectId(ue::UObject* o=nullptr):object(o) {
            if(o){cls=ue::ClassOf(o);outer=ue::Get<ue::UObject*>(o+ue::Layout::ObjOuter);index=ue::Get<int>(o+ue::Layout::ObjIndex,-1);name=ue::Get<uint64_t>(o+ue::Layout::ObjName);}
        }
        bool Alive() const {return object&&index>=0&&ue::Obj(index)==object&&ue::Get<int>(object+ue::Layout::ObjIndex,-1)==index&&ue::ClassOf(object)==cls&&ue::Get<ue::UObject*>(object+ue::Layout::ObjOuter)==outer&&ue::Get<uint64_t>(object+ue::Layout::ObjName)==name;}
    };
    struct Table {ObjectId id;uint8_t* rows=nullptr;int count=-1;};
    struct Check {ObjectId function;int input=-1,result=-1,required=-1;bool map=false;};
public:
    const char* Id() const override { return "res.upgrade_free"; }
    bool IsToggle() const override { return true; }
    bool Resolve(std::string* why) override {
        dtCls_ = ue::FindClass("DataTable");
        if (!dtCls_) { *why = "数据表类未加载"; return false; }
        bagCls_=ue::FindClass("PBZBackPackSubsystem");
        if(!bagCls_ || !pe::Ready() || !PrepareChecks()){*why="升级材料检查接口未就绪";return false;}
        nativeTarget_=upgrade::ResolveEnoughItems(checks_[1].function.object);
        if(!nativeTarget_){*why="原生升级检查指纹不匹配，未启用";return false;}
        return true;
    }
    void Enable() override {
        if (enabled) return;
        saved_.clear();
        savedIndex_.clear();tables_.clear();scanCursor_=0;nextCheck_=0;
        Discover(ue::Num()); // One batched discovery on enable; never repeat a full scan each second.
        if(!nativeCreated_) {
            if(!hooks::Create(kNativeHook,nativeTarget_,reinterpret_cast<void*>(&NativeCheck),reinterpret_cast<void**>(&nativeOriginal_)))throw std::runtime_error("原生升级检查挂接失败");
            nativeCreated_=true;
        }
        nativeThread_=GetCurrentThreadId();activeNative_=this;enabled=true;
        if(!hooks::Enable(kNativeHook,true)){activeNative_=nullptr;enabled=false;throw std::runtime_error("原生升级检查启用失败");}
        for(const auto& check:checks_)pe::Intercept(check.function.object,[this,check](ue::UObject* o,void* args){try{return ZeroCheck(check,o,args);}catch(...){return false;}});
        Apply(); enabled = true;RefreshUpgradeCache();
    }
    void Apply() {
        // 表名 / 行内字段（空 = 整行就是该结构）/ 是否数组 / 元素结构 / 材料表字段 / 尘埃字段
        struct Spot { const char *table, *field; bool array; const char *elem, *mats, *dust; };
        static const Spot spots[] = {
            {"DT_WeaponData", "WeaponUpLevelDatas", true, "WeaponUpLevelData", "Materials", "ConsumeDust"},
            {"DT_WeaponData", "WeaponSnapSoulData", false, "WeaponSnapSoulData", "Materials", "ConsumeDust"},
            {"DT_ShaqiWeaponData", "ShaChiWeaponUpLevelData", true, "ShaChiWeaponUpLevelData", "Materials", "ConsumeDust"},
            {"DT_ShaqiWeaponData", "ShaChiWeaponSnapSoulData", false, "ShaqiWeaponSnapSoulData", "Materials", "ConsumeDust"},
            {"DT_Accessory", "AccessoryLevelDatas", true, "AccessoryLevelData", "NeedItems", "NeedDust"},
            {"DT_MedicineTable", "MedicineLevelData", true, "MedicineLevelData", "ConsumeMaterials", "ConsumeDust"},
            {"DT_AbilityNodesTable", "", false, "AbilityNodeStruct", "NeedMaterials", "ConsumeDust"},
        };
        int tables = 0;
        for (const auto& sp : spots) {
            auto* table = FindTable(sp.table);
            auto* elem = ue::FindClass(sp.elem);
            if (!table || !elem) continue;
            auto* rowStruct = ue::Get<ue::UObject*>(table + 0x28);  // UDataTable::RowStruct
            const int field = *sp.field ? ue::PropOffset(rowStruct, sp.field) : 0;
            const int mats = ue::PropOffset(elem, sp.mats), dust = ue::PropOffset(elem, sp.dust);
            const int stride = ue::Get<int32_t>(elem + ue::Layout::StructSize);
            if (field < 0 || mats < 0 || dust < 0 || stride <= 0) continue;
            activeTable_ = table;
            ++tables;
            // RowMap：TMap<FName, uint8*>，元素 = 键 8 + 行指针 8 + 哈希链 8
            auto* rows = ue::Get<uint8_t*>(table + 0x30);
            for (int r : Slots(table + 0x30,4096)) {
                auto* row = ue::Get<uint8_t*>(rows + 0x18 * r + 8);
                if (!row) continue;
                activeOwner_={rows,rows+0x18*r,row,ue::Get<uint64_t>(rows+0x18*r)};
                if (sp.array) {
                    auto* data = ue::Get<uint8_t*>(row + field);
                    const int n = ue::Get<int32_t>(row + field + 8);
                    const int capacity = ue::Get<int32_t>(row + field + 12);
                    if(n<0 || n>512 || capacity<n || capacity>4096)continue;
                    activeOwner_.arrayHeader=row+field;activeOwner_.arrayData=data;
                    const int gate = ue::PropOffset(elem,"bNeedInJHSY");
                    for (int i = 0; data && i < n; ++i) {
                        auto* record=data+stride*i;
                        Zero(record,mats,dust);
                        if(gate>=0 && gate<stride)Set(record+gate,1);
                    }
                } else {
                    Zero(row + field, mats, dust);
                }
            }
        }
        if(!reported_) { log::Info("升级无消耗：{} 张表，{} 处消耗与升级场景条件清零", tables, saved_.size()); reported_=true; }
    }
    void Disable() override {
        enabled=false;
        activeNative_=nullptr;
        if(nativeCreated_)hooks::Enable(kNativeHook,false);
        for(const auto& check:checks_)pe::RemoveIntercept(check.function.object);
        for (const auto& patch : saved_) {
            if(ue::Obj(patch.index)!=patch.table || ue::Get<int>(patch.table+ue::Layout::ObjIndex,-1)!=patch.index || ue::ClassOf(patch.table)!=patch.cls ||
               ue::Get<uint64_t>(patch.table+ue::Layout::ObjName)!=patch.name ||
               ue::Get<ue::UObject*>(patch.table+ue::Layout::ObjOuter)!=patch.outer)continue;
            if(!patch.owner.Alive(patch.table))continue;
            uint32_t current=0;
            if(ue::Read(patch.p,&current,patch.bytes) && current==0)ue::Write(patch.p,&patch.value,patch.bytes);
        }
        saved_.clear();
        savedIndex_.clear();tables_.clear();
        enabled = false; reported_=false;
        RefreshUpgradeCache();
    }
    void Tick(float) override {
        const auto now=GetTickCount64();
        bool changed=false;
        if(now>=nextCheck_) {
            nextCheck_=now+1000;
            for(auto it=tables_.begin();it!=tables_.end();) {
                if(!it->second.id.Alive()){it=tables_.erase(it);continue;}
                auto* table=it->second.id.object;auto* data=ue::Get<uint8_t*>(table+0x30);const int n=ue::Get<int>(table+0x38,-1);
                if(data!=it->second.rows || n!=it->second.count){it->second.rows=data;it->second.count=n;changed=true;}
                ++it;
            }
        }
        if(tables_.size()<5)changed=Discover(2048)||changed; // Bounded work only while a table is missing.
        if(changed)Apply();
    }

private:
    ue::UObject* FindTable(const char* name) {
        const auto at=tables_.find(name);return at!=tables_.end()&&at->second.id.Alive()?at->second.id.object:nullptr;
    }
    bool Discover(int budget) {
        bool changed=false;const int n=ue::Num();const int end=std::min(n,scanCursor_+budget);
        ue::UObject* batch[1024];
        while(scanCursor_<end){const int take=std::min(1024,end-scanCursor_);const int got=ue::ObjBatch(scanCursor_,take,batch);scanCursor_+=take;
            for(int i=0;i<got;++i){auto* o=batch[i];if(!o||ue::ClassOf(o)!=dtCls_)continue;
                const auto name=ue::ObjName(o);
                if(name!="DT_WeaponData"&&name!="DT_ShaqiWeaponData"&&name!="DT_Accessory"&&name!="DT_MedicineTable"&&name!="DT_AbilityNodesTable")continue;
                auto at=tables_.find(name);if(at!=tables_.end()&&at->second.id.Alive())continue;
                ObjectId id(o);if(!id.Alive())continue;
                tables_[name]={id,ue::Get<uint8_t*>(o+0x30),ue::Get<int>(o+0x38,-1)};changed=true;
            }
        }
        if(scanCursor_>=n)scanCursor_=0;
        return changed;
    }
    static std::vector<int> Slots(uint8_t* map,int limit,bool* valid=nullptr) {
        if(valid)*valid=false;
        std::vector<int> slots;
        const int n=ue::Get<int32_t>(map+8,-1),capacity=ue::Get<int32_t>(map+12,-1);
        const int bits=ue::Get<int32_t>(map+0x28,-1),bitCapacity=ue::Get<int32_t>(map+0x2c,-1),free=ue::Get<int32_t>(map+0x34,-1);
        auto* data=ue::Get<uint8_t*>(map);auto* heap=ue::Get<uint8_t*>(map+0x20);
        if(n<0 || n>limit || capacity<n || capacity>1048576 || bits!=n || bitCapacity<bits || bitCapacity>1048576 || free<0 || free>n || (n&&!data) || (!heap&&bitCapacity>128))return slots;
        auto* flags=heap?heap:map+0x10;
        std::vector<uint32_t> words((n+31)/32);
        if(!words.empty()&&!ue::Read(flags,words.data(),words.size()*4))return slots;
        for(int i=0;i<n;++i)if((words[i/32]>>(i%32))&1)slots.push_back(i);
        if(int(slots.size())!=n-free)slots.clear();
        else if(valid)*valid=true;
        return slots;
    }
    void Set(uint8_t* p,int bytes=4) {
        uint32_t v=0;
        if(!ue::Read(p,&v,bytes) || (bytes==1&&v>1) || (bytes==4&&v>1000000000))return;
        if (v == 0) return;
        const auto previous=savedIndex_.find(p);
        if(previous==savedIndex_.end() || saved_[previous->second].table!=activeTable_ || !saved_[previous->second].owner.Alive(activeTable_)) {
            savedIndex_[p]=saved_.size();
            saved_.push_back({p,v,bytes,activeTable_,ue::Get<int>(activeTable_+ue::Layout::ObjIndex,-1),ue::ClassOf(activeTable_),ue::Get<ue::UObject*>(activeTable_+ue::Layout::ObjOuter),ue::Get<uint64_t>(activeTable_+ue::Layout::ObjName),activeOwner_});
        }
        const uint32_t z = 0;
        ue::Write(p, &z, bytes);
    }
    // Verified in native HasEnoughItems: FName aligns to 4, pair=12 + hash=8.
    // UObject pointer value maps (RowMap) align to 8 and remain stride24.
    void Zero(uint8_t* rec, int mats, int dust) {
        Set(rec + dust);
        auto* data = ue::Get<uint8_t*>(rec + mats);
        for (int i : Slots(rec+mats,64)) {
            if (!ue::Get<uint32_t>(data + 0x14 * i))continue;
            activeOwner_.materialHeader=rec+mats;activeOwner_.materialData=data;
            activeOwner_.materialSlot=data+0x14*i;activeOwner_.materialName=ue::Get<uint64_t>(data+0x14*i);
            Set(data + 0x14 * i + 8);
        }
        activeOwner_.materialHeader=nullptr;
    }

    static int Field(ue::UObject* fn,const char* name,const char* type,int size,bool result=false) {
        auto* f=ue::Get<uint8_t*>(fn+ue::Layout::StructChildProps);
        for(int i=0;f&&i<128;++i,f=ue::Get<uint8_t*>(f+ue::Layout::FieldNext)) {
            if(ue::Name(ue::Get<uint32_t>(f+ue::Layout::FieldName))!=name)continue;
            auto* fc=ue::Get<uint8_t*>(f+ue::Layout::FieldClass);const int off=ue::Get<int>(f+ue::Layout::PropOffset,-1),total=ue::Get<int>(fn+ue::Layout::StructSize,-1);
            const auto actual=fc?ue::Name(ue::Get<uint32_t>(fc+ue::Layout::FieldClassName)):std::string{};
            const bool objectAlias=std::string_view(type)=="ObjectProperty"&&(actual=="ClassProperty"||actual=="ClassPtrProperty"||actual=="ObjectPtrProperty");
            if(!fc || (actual!=type&&!objectAlias) || ue::Get<int>(f+0x34)!=size ||
               !(ue::Get<uint64_t>(f+0x38)&0x80) || bool(ue::Get<uint64_t>(f+0x38)&0x400)!=result || off<0 || total>4096 || off+size>total)return -1;
            return off;
        }
        return -1;
    }
    bool PrepareChecks() {
        checks_.clear();
        for(bool map:{false,true}) {
            auto* fn=pe::FindFunction(bagCls_,map?"HasEnoughItems":"HasEnoughItem");if(!fn)return false;
            Check c;c.function=ObjectId(fn);c.map=map;c.input=Field(fn,map?"RequiredItems":"Num",map?"MapProperty":"IntProperty",map?80:4);
            c.result=Field(fn,"ReturnValue","BoolProperty",1,true);
            if(!map){c.required=Field(fn,"RequireNum","IntProperty",4);if(Field(fn,"ItemId","NameProperty",8)<0||c.required<0)return false;}
            if(!c.function.Alive()||c.input<0||c.result<0)return false;
            checks_.push_back(c);
        }
        return true;
    }
    bool ZeroCheck(const Check& c,ue::UObject* o,void* raw) const {
        if(!enabled||!raw||!c.function.Alive()||!ObjectId(o).Alive()||!ue::IsA(o,bagCls_)||ue::IsDefault(o))return false;
        auto* args=static_cast<uint8_t*>(raw);
        if(c.map) {
            bool valid=false;auto* map=args+c.input;const auto slots=Slots(map,4096,&valid);if(!valid)return false;
            auto* data=ue::Get<uint8_t*>(map);
            for(int i:slots)if(ue::Get<int>(data+0x14*i+8,-1)!=0)return false;
        } else if(ue::Get<int>(args+c.input,-1)!=0)return false;
        if(c.required>=0){const int zero=0;if(!ue::Write(args+c.required,&zero,4))return false;}
        const uint8_t yes=1;return ue::Write(args+c.result,&yes,1);
    }
    bool ZeroMap(ue::UObject* o,const void* raw) const {
        if(!enabled||!raw||!ObjectId(o).Alive()||!ue::IsA(o,bagCls_)||ue::IsDefault(o))return false;
        auto* map=static_cast<uint8_t*>(const_cast<void*>(raw));bool valid=false;const auto slots=Slots(map,4096,&valid);if(!valid)return false;
        auto* data=ue::Get<uint8_t*>(map);
        for(int i:slots)if(ue::Get<int>(data+0x14*i+8,-1)!=0)return false;
        return true;
    }
    static bool __fastcall NativeCheck(ue::UObject* object,const void* map) {
        if(auto* self=activeNative_;self&&GetCurrentThreadId()==self->nativeThread_) {
            try {if(self->ZeroMap(object,map)){if(self->nativeZeroLogs_++<3)log::Info("升级无消耗：原生零材料检查放行（不要求曾持有，不改正数要求）");return true;}}catch(...){}
        }
        return nativeOriginal_(object,map);
    }
    void RefreshUpgradeCache() {
        ue::UObject *player=nullptr,*bag=nullptr;if(!inv::CurrentActors(player,bag))return;
        auto* cls=ue::FindClass("BP_UIGameplayLogicComponent_C");if(!cls)return;
        auto* get=pe::FindFunction(ue::ClassOf(player),"GetComponentByClass");if(!get)return;
        const int input=Field(get,"ComponentClass","ObjectProperty",8),output=Field(get,"ReturnValue","ObjectProperty",8,true);
        if(input<0||output<0)return;
        std::array<uint8_t,4096> args{};memcpy(args.data()+input,&cls,8);pe::Call(player,get,args.data());auto* component=ue::Get<ue::UObject*>(args.data()+output);
        if(!ObjectId(component).Alive()||ue::Get<ue::UObject*>(component+ue::Layout::ObjOuter)!=player||!ue::IsA(component,cls))return;
        auto* recheck=pe::FindFunction(cls,"ReCheckItemUpgradeState");if(!recheck)return;
        const int off=Field(recheck,"BackPackSubsystem","ObjectProperty",8);if(off<0)return;
        args.fill(0);memcpy(args.data()+off,&bag,8);pe::Call(component,recheck,args.data());
        log::Info("升级无消耗：当前玩家强化状态及材料缓存已刷新（{}）",enabled?"无消耗":"恢复原配方");
    }
    using NativeEnough=bool(__fastcall*)(ue::UObject*,const void*);
    inline static NativeEnough nativeOriginal_=nullptr;
    inline static UpgradeFree* activeNative_=nullptr;
    static constexpr const char* kNativeHook="UpgradeFree.ZeroRequirements";
    void* nativeTarget_=nullptr;bool nativeCreated_=false;DWORD nativeThread_=0;int nativeZeroLogs_=0;
    ue::UObject *dtCls_ = nullptr,*bagCls_=nullptr;
    struct Owner {
        uint8_t *rows=nullptr,*rowSlot=nullptr,*row=nullptr;uint64_t rowName=0;
        uint8_t *arrayHeader=nullptr,*arrayData=nullptr,*materialHeader=nullptr,*materialData=nullptr,*materialSlot=nullptr;uint64_t materialName=0;
        bool Alive(ue::UObject* table) const {
            return ue::Get<uint8_t*>(table+0x30)==rows && ue::Get<uint64_t>(rowSlot)==rowName && ue::Get<uint8_t*>(rowSlot+8)==row &&
                (!arrayHeader || ue::Get<uint8_t*>(arrayHeader)==arrayData) &&
                (!materialHeader || (ue::Get<uint8_t*>(materialHeader)==materialData && ue::Get<uint64_t>(materialSlot)==materialName));
        }
    } activeOwner_;
    struct Saved {uint8_t* p;uint32_t value;int bytes;ue::UObject* table;int index;ue::UObject *cls,*outer;uint64_t name;Owner owner;};
    std::vector<Saved> saved_;
    std::unordered_map<uint8_t*,size_t> savedIndex_;
    std::unordered_map<std::string,Table> tables_;
    std::vector<Check> checks_;
    ue::UObject* activeTable_=nullptr;
    ULONGLONG nextCheck_=0;
    int scanCursor_=0;
    bool reported_=false;
};

