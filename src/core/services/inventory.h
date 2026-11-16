// L1：背包读写（物品编辑页用）。全部走游戏自己的背包接口（PBZBackPackSubsystem / 玩家 ChangeDustNum），
// 不直接改背包表；只在游戏线程调用。
#pragma once
#include <cstdint>
#include <string>
#include "ue.h"

namespace pbz::inv {

// 当前持有：items + session/dust/stack/acquired/owned/policy。
// policy 为原生类别、可编辑数量、内部部件与额外内容权益的只读判定。
// 集合读取失败明确 ok=false；单个可识别记录异常时 partial=true，
// 对应 policy.read=false 且禁止写入，不把未读取显示成零。换玩家/World 后必须重新 List。
std::string List();

// kind：dust（num = 目标余额）、stack（num = 目标数量）、weapon / shaqi_weapon / accessory / medicine / skin / item（只添加一件）
// 数量必须为整数（stack 0..99999，dust 0..999999999）；null 重复性不开放数量编辑。
// 返回 item + ok/id/msg/actual/requested/pending/session；成功须经原生持有回读。
// session 必须来自最近一次成功 List；旧客户端缺 token 时拒绝写入。
std::string Apply(const std::string& id, const std::string& kind, long long num, const std::string& session = "");
// IPC 字段必须完整解析为整数，避免 atoll("1.5") 静默变为 1。
std::string ApplyRequest(const std::string& id, const std::string& kind, const std::string& number, const std::string& session);

// 字符串 → FName（经 KismetStringLibrary.Conv_StringToName；失败返回 0）。游戏线程调用
uint64_t ToFName(const std::string& s);
// Read-only identity-validated context, game thread only; never grants editing permission.
bool CurrentActors(ue::UObject*& player,ue::UObject*& backpack);

}  // namespace pbz::inv
