#pragma once
#include <string>
#include <string_view>
#include "tower_json.h"
namespace tower::spawnreceipt {
struct Actor {bool ok=false;std::string name,path,why;};
// actual：核心按独立阶段规则替换后的真实路径（仆从走修改器路径时，雷荡/赤炼会换成终阶段）；其余调用 actual == wanted
inline Actor Read(std::string_view text,std::string_view request,std::string_view wanted,std::string_view actual){
 json::Value root;
 if(!json::Parse(std::string(text),&root))return {false,{},{},"生成回执不是有效JSON"};
 auto str=[](const json::Value& v,const char* key)->std::string_view{auto* p=v.Find(key);return p && p->kind==json::Value::String?p->text:std::string_view{};};
 if(str(root,"request_id")!=request || str(root,"requested_path")!=wanted || str(root,"path")!=actual)return {false,{},{},"生成回执请求/身份不一致"};
 auto* rows=root.Find("actors");
 if(!rows || rows->kind!=json::Value::Array)return {false,{},{},"生成回执缺少所有权记录"};
 Actor result;int matches=0;
 for(const auto& row:rows->items)if(str(row,"tag")==request){++matches;result.name=str(row,"name");result.path=str(row,"path");}
 if(matches!=1 || result.name.empty() || result.path!=actual)return {false,{},{},"生成回执没有唯一同身份的本请求Actor"};
 result.ok=true;return result;
}
inline Actor Read(std::string_view text,std::string_view request,std::string_view wanted){return Read(text,request,wanted,wanted);}
}
