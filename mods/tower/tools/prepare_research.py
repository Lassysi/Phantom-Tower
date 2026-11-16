"""Emit source-linked offline handoff notes; verify the SDK signatures we quote."""
from __future__ import annotations

import argparse
import hashlib
import json
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
SDK = "notes/dump/sdk/sdk.txt"
DATA = ROOT / "mods/tower/data"


def rows(name):
    return json.loads((DATA / (name + ".json")).read_text(encoding="utf8"))["items"]


class Research:
    def __init__(self):
        self.lines = (ROOT / SDK).read_text(encoding="utf8").splitlines()
        self.blocks = {}
        block = None
        for n, line in enumerate(self.lines, 1):
            match = re.match(r"^\w*(?:Class|Struct) (\S+) :", line)
            if match:
                block = match[1]
                self.blocks[block] = []
            if block:
                self.blocks[block].append((n, line))
        self.hash = hashlib.sha256((ROOT / SDK).read_bytes()).hexdigest()

    def selected(self, owner, needles):
        block = self.blocks[owner]
        selected = []
        for needle in needles:
            found = [(n, line.strip()) for n, line in block if needle in line]
            if len(found) != 1:
                raise ValueError(f"SDK needle ambiguous/absent: {owner}: {needle}: {found}")
            selected += found
        result = [f"**`{owner}`**（静态）：", "", "```text"]
        result += [line for _, line in selected]
        result += ["```", "", "来源：" + "、".join(f"`{SDK}:{n}`" for n, _ in selected) + "。", ""]
        return "\n".join(result)

    def header(self, title):
        return f"# {title}\n\n2026-10-30 · 功能 ID T2/T3 · 证据等级：静态；玩法有效性均 `unverified`。全程离线，未连接管道、注入、刷怪或写游戏存档。\n\nSDK SHA-256 `{self.hash}`。行号仅用于这份导出，运行时必须按反射名及参数布局重验，不可照抄偏移。\n\n"

    def equip(self):
        text = self.header("影塔装备接口预备")
        text += "## 获取与读取\n\n优先从当前世界获取 `PBZBackPackSubsystem`，不要缓存跨读档/换地图的 UObject。`MainWeapons` / `ShaqiWeapons` 是装备槽，所有拥有物在 `MainWeaponInfos` / `ShaqiWeaponInfos`；不能拿拥有列表代替当前装备。索引先验边界，再对应槽内 ID。\n\n"
        text += self.selected("/Script/ProjectPBZ.PBZBackPackSubsystem", ["fn GetBackPackSubsytemByContextObject(", "fn GetEquipmentNormalWeapons(", "fn GetShaqiWeapons(", "fn GetCurrentMainWeaponIndex(", "fn GetCurrentShaqiWeaponIndex(", "fn GetAccessoryID(", "fn GetAccessoryInfos(", " AssemblyAccessory", " CurrentMainWeaponID", " MainWeaponInfos", " ShaqiWeaponInfos"])
        text += "## 原生换装候选\n\n"
        text += self.selected("/Script/ProjectPBZ.PBZBackPackSubsystem", ["fn PushEquippedWeapons(", "fn PopEquippedWeapons(", "fn HasEquippedWeaponsSnapshot(", "fn SetCurrentWeaponByIndex(", "fn ReplaceWeaponByIndex(", "fn ReplaceShaqiWeaponByIndex(", "fn AssembleAccessory(", "fn TakeOffAccessory(", "fn ApplyForcedWeaponLoadout(", "fn RestoreForcedWeaponLoadout("])
        text += "`PushEquippedWeapons` / `PopEquippedWeapons` 是最值得先查的成对接口，但只存在一个 `EquippedSnapshot` 和一个 `bHasEquippedSnapshot`，**不能假定可重入栈**。已有快照时拒绝覆盖，先辨认剧情强制换装持有者。`EquippedWeaponSnapshot` 没有饰品，因此饰品必须另记槽位、ID、空槽；卸下会否压缩槽数组需实测，不能盲目正序遍历删除。不要用 `RemoveWeaponByID` 做临时卸装，它是拥有集合操作。\n\n"
        text += self.selected("/Script/ProjectPBZ.EquippedWeaponSnapshot", [" SavedMainWeapons", " SavedCurrentWeaponIndex", " SavedCurrentMainWeaponID", " SavedShaqiWeapons", " SavedCurrentShaqiWeaponIndex"])
        text += "## 战斗组件与背包一致性\n\n"
        text += self.selected("/Script/SGameCombatSystem.CharacterWeaponsComponent", ["fn SetCurrentWeapon(", "fn PendingSetCurrentWeapon(", "fn GetCurrentWeapon(", "fn IsInWeaponTransition("])
        text += "组件使用 GameplayTag + 槽索引和 RuntimeWeaponSingleData，不是物品 ID。绕过背包直接设组件可能不刷新 GE、动画层、菜单和存档，先验证原生背包调用是否已经完成这条链。现有 `src/core/services/inventory.cpp` 已展示参数名/类型/返回布局校验、世界代次与 session；由 Claude 复用服务，不在数据脚本里写进程调用。\n\n"
        text += "## EQUIP-ROUNDTRIP（待实机）\n\n1. Claude 在游戏线程只读记录 session、两组装备槽/索引、饰品槽、弹药、等级/铸魂和是否已有强制装束快照；用现有存档，不为实验授予兵器。\n2. 先备份；在钟边、非过场且不在武器过渡时，选两件已拥有兵器，Push → 切换 → 读回背包与战斗组件 → Pop；确认原索引、动作、GE、菜单一致。\n3. 单独测饰品卸下一槽与恢复，包括空槽、重复槽防护；比较属性，不只看函数调用完成。\n4. 换地图/读档/死亡/正常离塔/强关分别检查；崩溃恢复日志必须先于临时变更落盘，绑定存档身份。恢复只改本塔持有的临时状态。\n\n已知问题：未证实上述原生方法是否触发自动存档、异步资源加载或进入剧情限制；`Push/Pop` 对未拥有物品和崩溃后恢复不作保证。\n"
        return text

    def pause(self):
        text = self.header("影塔暂停与输入隔离预备")
        text += "## 可查的原生暂停\n\n"
        text += self.selected("/Script/Engine.GameplayStatics", ["fn SetGamePaused(", "fn IsGamePaused("])
        text += self.selected("/Script/ProjectPBZ.PBZPlayerController", ["fn IsPaused(", " OnGamePausedChanged", "fn SetCameraMoveableWhenPaused("])
        text += self.selected("/Script/ProjectPBZ.PBZActivatableWidget", [" InputConfig", " bPauseGameWhenOpen", " CurrentTimeDilation", "fn ResetHoldByKey("])
        text += "关键继承关系：`W_ConfirmationDialog_C → PBZConfirmationScreen → CommonGameDialog → CommonActivatableWidget`；**不经过 PBZActivatableWidget**。不能把 `bPauseGameWhenOpen` 的偏移写到当前确认框。SDK 中 `CommonUIActionRouterBase` 没有导出的 UFunction，不能凭 UE 通用文档假设 SetActiveUIInputConfig 可反射调用。\n\n"
        text += "## 输入模式与成对令牌\n\n"
        text += self.selected("/Script/CommonGame.CommonUIExtensions", ["fn SuspendInputForPlayer(", "fn ResumeInputForPlayer(", "fn PushContentToLayer_ForPlayer(", "fn PushStreamedContentToLayer_ForPlayer("])
        text += self.selected("/Script/NBInput.NBInputComponent", ["fn AddInputModeTag(", "fn RemoveInputModeTag(", "fn IsGameInputMode("])
        text += self.selected("/Script/ProjectPBZ.PBZBlueprintFunctionLibrary", ["fn AddInputModeTag(", "fn RemoveInputModeTag("])
        text += self.selected("/Script/Engine.Controller", ["fn SetIgnoreMoveInput(", "fn SetIgnoreLookInput(", "fn IsMoveInputIgnored(", "fn IsLookInputIgnored("])
        text += "推荐顺序（推断，待验证）：先沿原生 Modal 的输入模式路由；若不隔离攻击，再查 NBInput 的独立 Imposer 对象与 Menu tag。`SuspendInputForPlayer` 可能连菜单输入一起屏蔽，适合加载短临界区，不能不经验证就覆盖整段选卡。IgnoreMove/Look 不覆盖攻击和闪避，不能单独作为验收。不要 ResetToDefault / ResetIgnoreInputFlags 清别人的限制，不用全局 TimeDilation=0 冒充暂停。\n\n"
        text += "## PAUSE-INPUT（待 Claude 实机）\n\n1. 记录世界、PC、原暂停状态和已有输入持有者；打开选卡，只让本塔新加暂停/输入租约，令牌绑定 run_id 与当前对话框对象。\n2. 逐个验证移动、镜头、普攻、格挡、闪避、手柄确认/取消；确认 UI 仍可导航，怪物/投射物/计时停止。用按住攻击再关闭菜单验证穿透。\n3. 选项确认后等相关确认键释放再还原；只释放本塔令牌。原先已经暂停或别的菜单接管时，不擅自取消。\n4. 关闭、取消、失焦、换图、玩家/PC 重建、失联、核心停用都走同一清理；检查原生菜单与修改器时间倍率仍正常。\n5. `src/core/services/gamethread.cpp` 的功能回调放在 GameEngine Tick 后；静态位置支持继续清理的可能性，但暂停时该 Hook 是否持续运行仍需实机观察，超时使用单调时钟，不能用已停止的游戏 DeltaTime。\n\n已知问题：暂停、Modal 输入优先级、输入计数/Tag 恢复语义尚未实测。本次不改变当前引擎“不暂停”的行为。\n"
        return text

    def native_ui(self):
        text = self.header("影塔原生提示控件预备")
        text += "## 物品与尘埃提示\n\n"
        text += self.selected("/Script/ProjectPBZ.PBZHUDLayout", ["fn GetHUDLayout(", "fn DisplayItem(", "fn RegisterBossHPBar(", "fn UnRegisterBossHPBar(", "fn GetBossHPBarInstances("])
        text += self.selected("/Game/PBZContent/UI/FrontEnd/WB_ItemTips.WB_ItemTips_C", ["fn DisplayItem(", "fn Add Dust To Array(", "fn Add to Cache Array("])
        text += "调用 HUD `DisplayItem(真实原生 PropID, true, Num)` 是显示候选，不是授予奖励。传 `tower.floor` 之类虚构 ID 无物品表图标/文字依据；魂玉也不是游戏尘埃，不能拿 Add Dust 伪装。实际授予由备份+session 校验的 grant 路径负责，并确认提示是否已由原生调用自动出现，避免重复。\n\n"
        text += self.selected("/Script/ProjectPBZ.PBZBackPackSubsystem", ["fn TryToPushPickupUI(", "fn CanPushPickupUI(", "fn IsDisplayPickupUI(", "fn IsActivatePickupWidget(", " ActivePickupWidget", " DisplayItemData"])
        text += self.selected("/Script/ProjectPBZ.DisplayItemUIData", [" ItemName", " IsMainWeapon", " DisplayItemType", " bDisplayOnly", " bAlreadyOwned"])
        text += "`bDisplayOnly` 提供纯展示方向，但不能证明某个上游调用不会改背包；不要往 `DisplayItemData` 直接塞字节来代替原生排队接口。\n\n"
        text += "## 自定义层数/收获文字\n\n"
        text += self.selected("/Script/ProjectPBZ.PBZBlueprintFunctionLibrary", ["fn ShowTextTipsByKey("])
        text += self.selected("/Game/PBZContent/UI/FrontEnd/WB_TextTipsObj.WB_TextTipsObj_C", [" TextKey", " ShowText"])
        text += self.selected("/Game/PBZContent/UI/FrontEnd/W_TIpsLayer.W_TIpsLayer_C", [" Show Text", "fn OnConfigureWidgetForData(", "fn OnGetWidgetClassForData(", "fn DestroyTextTips("])
        text += self.selected("/Script/UIExtension.UIExtensionSubsystem", ["fn K2_RegisterExtensionAsDataForContext(", "fn UnregisterExtension("])
        text += "`ShowTextTipsByKey` 收**本地化键**，不是自由文本，不能把“第 3 层”直接当 TextKey。可研究创建 WB_TextTipsObj 并填 ShowText，再通过 Tips 层自己的 UIExtension tag 注册；当前 dump 没有确认运行中 ExtensionPoint 的具体 tag/数据类映射，标 `unverified`。FText 用游戏 Conv_StringToText 构建并管理生命周期，不能 memcpy 后双重释放。持久层数 HUD 还需验证提示动画是否会自动销毁，不先承诺常驻。\n\n"
        text += "## 通知队列与确认框\n\n"
        text += self.selected("/Script/ProjectPBZ.PBZNotificationQueueSubsystem", ["fn Enqueue(", "fn RegisterReceiver(", "fn IsNotificationSuppressed(", "fn NotifyDisplayFinished("])
        text += "PBZNotificationPayload 只有 tag/时长/聚合键，派生 PBZQuestNotificationPayload 则是任务文字。未找到通用文字 Payload；不要把任务队列当任意横幅接口。确认框与钟入口的已实测路径沿用 `notes/handoff-tower.md`，本次没有重复或升级实机证据。\n\n"
        text += "## UI-TIPS（待实机）\n\n1. 先观察玩家自然拾取一件已有消耗品的调用流，确定 HUD、Tips widget、扩展 tag 和对象生命周期。\n2. 只显示一条该真实 ID 的提示，比较背包/session/存档无变化；物品提示与 grant 分开验收。\n3. 测一条中英自定义层数提示的 ShowText 路径、换图/菜单遮挡/字幕并存、重复排队与句柄清理；不使用未知本地化键。\n4. Boss 血条只登记本塔 actor，转阶段先解绑旧句柄再登记新 actor，结束只清自己的句柄。\n"
        return text

    def phase(self):
        text = self.header("影塔两阶段与 Boss 分类预备")
        text += "## 静态边界\n\n本次 `bosses.json` 保留原粗池 86 条作为审计目录，所有 `spawn_verified:false`，不是可直接刷的白名单。A 表示保守预分类中未发现已知场景依赖，**不代表已经证明通用平地可打**。`class` 为主类，`classes` 保留 B/C/D/F 并存；NvN 只接主类 A。\n\n蓝图 JSON 导出包含函数声明、组件和默认属性，但没有可重建执行流的 Function 字节码；`sdk.txt` 的本次运行时快照也未包含这些 Boss 的完整 Blueprint 类。看到 `_P2` 不能证明原生死亡后会自动生成二阶段，更不能把关卡剧情节点当已定位完成。\n\n"
        text += "## 两阶段候选（切换都未验证）\n\n| 身份 | 一阶段类 | 二阶段类 | 依据 |\n|---|---|---|---|\n"
        for b in rows("bosses"):
            if b.get("phase2"):
                text += f"| {b['identity']} | `{b['id']}` | `{b['phase2'].split('.')[-1]}` | 类存在；方案配对；`unverified` |\n"
        text += "\n荒行子→撕裂锯轮、大师兄→绝望剑意没有 `_P2` 后缀，已显式列出；长鸿一阶段叫 ChangHong_Spear，而二阶段叫 ChangHong_P2，不能简单追加后缀。左殇序章/Ending 版同身份；终局明确取 Ending，阶段二只列为 phase2，不单独抽取。\n\n"
        text += "## 可复查的具体蓝图线索\n\n"
        for stem in ("BP_Monster_MoTian", "BP_Monster_MBoss", "BP_Monster_HuaShan_Blue", "BP_Monster_GuiChai_Red"):
            f = next((ROOT / "notes/dump/enemies").rglob(stem + ".json"))
            d = json.loads(f.read_text(encoding="utf8"))
            names = [x.get("Name", "") for x in d if x.get("Type") == "Function"]
            relevant = [n for n in names if re.search("Death|Dead|Pillar|Spawn|Chain", n, re.I)]
            text += f"- `{f.relative_to(ROOT).as_posix()}`：" + ("、".join(f"`{n}`" for n in relevant) or "仅类/组件声明，没有可证实的阶段切换函数") + "。\n"
        text += "\n魔渊的 SpawnPillar/DespawnPillar 支持机关依赖方向；火流星的 Chain 是自身武器/特效组件，并不自动证明需要关卡悬挂点。其他 B 预分类仍以方案为保守限制，未复核出锚点的保持 classification_evidence=unverified，不把普通 SceneComponent 或 AnimSequence 误判为场景机关。\n\n"
        text += "## PHASE-TRANSITION（引擎串联前待实机）\n\n1. 先捕获一场原生两阶段战：死亡/破防事件、旧 actor 生命周期、新 actor 的类路径/出生 Transform、FlowGraph tag/序列。区分原 actor 换皮、延迟重生、关卡刷第二个 actor。\n2. 所有塔生成 actor 使用同一 run/floor 的独立标签；只在旧阶段确认完成后串联新阶段，不以单次 HP=0 判整层通关；新 actor 注册完成再计战斗。\n3. 两阶段共用总 HP 预算（初拟 50/50，尚未定，不在数据里伪装最终数值），持续卡/层神通是否跨阶段生效、兵器只结一次、复活符与转阶段同时触发均需测试。\n4. 禁用原剧情发奖、传送、关门与过场的路径必须有定位证据，不全局屏蔽 FlowGraph；B 类优先回原生擂台。失败不重放阶段/发奖。\n5. MBoss、魔渊、大师兄等机关与大场景，验证离场后清理不会残留召唤物/柱/特效。\n\n## BOSS-AUDIT（之后协调游戏才执行）\n\n逐个保存日期、游戏 Build、类路径、地图、原生/通用擂台、锁定/可伤害/零血死亡/主动 AI/自动消失、二阶段/机关完整性、大场地要求、清理与恢复。一次只测一身份；召唤上限单独测。只要一项失败，就不把它提升成可用白名单。变体同 identity，阶段二不单独抽，金面具只在成组层 x2。\n"
        return text


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    research = Research()
    for name in ("equip", "pause", "phase", "native_ui"):
        value = getattr(research, name)().encode("utf8")
        dest = ROOT / "notes" / ("codex-tower-" + name.replace("_", "-") + ".md")
        if args.check:
            if not dest.exists() or dest.read_text(encoding="utf-8-sig") != value.decode("utf8"):
                raise SystemExit("stale research note: " + str(dest))
        else:
            dest.write_bytes(value)
        print(dest.relative_to(ROOT).as_posix())


if __name__ == "__main__":
    main()
