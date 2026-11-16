// L1：进程内 UE 反射访问。只依赖运行时数据结构特征，不写死地址。
// 布局（Build 26266538 实测，见 notes/stage0.md）：
//   GUObjectArray.Objects 前 8 字节是 MaxChunks/NumChunks，后 8 字节是 Num/Max；FUObjectItem 24 字节，UObject* 在 +8
//   UObject：+0x0C 索引 +0x10 类 +0x18 名字 +0x20 Outer
//   UStruct：+0x40 Super +0x50 ChildProperties +0xA8 PropertiesSize
//   FField：+0x08 Class +0x10 Next +0x20 Name；FFieldClass 名字 +0x30；FProperty 偏移 +0x44
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

namespace pbz::ue {

using UObject = uint8_t;

struct Layout {
    static constexpr int ObjIndex = 0x0C, ObjClass = 0x10, ObjName = 0x18, ObjOuter = 0x20;
    static constexpr int StructSuper = 0x40, StructChildren = 0x48, StructChildProps = 0x50, StructSize = 0xA8;
    static constexpr int UFieldNext = 0x28;  // UStruct::Children 链（UFunction）的 Next
    static constexpr int FieldClass = 0x08, FieldNext = 0x10, FieldName = 0x20, FieldClassName = 0x30;
    static constexpr int PropOffset = 0x44;
    static constexpr int ItemSize = 24, ItemObject = 8;
};

bool Init(std::string* why);            // 定位 GObjects 与 FNamePool
int Num();
UObject* Obj(int index);
int ObjBatch(int start, int count, UObject** out);  // 批量取对象指针（一次受保护读一段），返回实际个数
std::string Name(uint32_t comparisonIndex);
std::string ObjName(UObject* o);
UObject* ClassOf(UObject* o);
std::string ClassName(UObject* o);
bool IsA(UObject* o, UObject* cls);
UObject* FindClass(std::string_view name);           // 按短名找 UClass/UScriptStruct
int PropOffset(UObject* structObj, std::string_view prop);  // 沿 Super 链找属性偏移，找不到返回 -1
void ForEach(const std::function<bool(int, UObject*)>& fn);  // 返回 false 停止
bool IsDefault(UObject* o);               // CDO / 原型对象（Default__ 前缀）
bool Live(UObject* o);                    // 对象仍在对象表里、未销毁、未标垃圾（旧指针、已释放内存都返回 false）
void NameRaw(uint32_t idx, char* out, size_t cap);  // 不加锁、不分配的名字读取（异常诊断用）

// 带 SEH 保护的读写
bool Read(const void* p, void* out, size_t n);
bool Write(void* p, const void* in, size_t n);  // 目标已释放时返回 false，不崩
template <class T> T Get(const void* p, T def = T{}) { T v; return Read(p, &v, sizeof v) ? v : def; }

}  // namespace pbz::ue
