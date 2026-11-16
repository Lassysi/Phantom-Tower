// 自动切档的游戏端（游戏线程）：原生"返回标题"与"继续"。全部在游戏线程调用，每次重查当前世界里的实例，不缓存旧世界对象。
#pragma once
#include <string>

namespace towertitle {

bool Quiet();                      // 没有原生关卡加载（GameInstance.IsLoadingLevel）也没有活动的原生加载屏
bool MenuReady();                  // 开始菜单控件已在、启动警告已结束
bool ToTitle(std::string* why);    // 同 TsHelper.returnStart：ChangeGameType(StartMenu) → GetCorrectStartMenuMapName → OpenLevel
bool Continue(std::string* why);   // 同开始菜单"继续"：PBZGameInstance.LoadGame(续玩槽) → OpenLevel(存档关卡, 存档选项)

}  // namespace towertitle
