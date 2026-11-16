"""影塔开发期控制台（覆盖层界面做好之前用）：
  python -I mods/tower/tools/tower.py load            把 build/core/modules/PBZTower.dll 交给游戏里的运行库
  python -I mods/tower/tools/tower.py start [层数]     开一局（默认 3 层）
  python -I mods/tower/tools/tower.py next            开下一层（在你身边生成本层 Boss）
  python -I mods/tower/tools/tower.py pick 1|2|3      3 选 1
  python -I mods/tower/tools/tower.py state | stop
  python -I mods/tower/tools/tower.py probe           读玩家强度属性（标定统一起点用）
  python -I mods/tower/tools/tower.py use heal|freeze|slow|sunder|shachi   用道具
  python -I mods/tower/tools/tower.py reroll          刷新令重抽三选一
  python -I mods/tower/tools/tower.py st 1|2|3        用神通栏里的第 n 个
  python -I mods/tower/tools/tower.py resume          继续上一局（局存档）
  python -I mods/tower/tools/tower.py grant           待领取兵器写进存档（先自己备份存档！）
  python -I mods/tower/tools/tower.py suspend         暂离（保存后结束本次，之后 resume）
  python -I mods/tower/tools/tower.py arena [list]    擂台：各区候选、是否已标定中心、传送状态
  python -I mods/tower/tools/tower.py arena mark <id> 把你现在站的地方标定为擂台中心（站在平坦开阔处）
  python -I mods/tower/tools/tower.py arena go <id|home>   试传到擂台 / 传回进塔位置
  python -I mods/tower/tools/tower.py arena unmark <id>
运行库要先在游戏里（打开修改器，或 tools/core/dev.py inject）。
"""
import ctypes as C
import ctypes.wintypes as W
import json
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
k32 = C.WinDLL("kernel32", use_last_error=True)
k32.CreateFileW.restype = W.HANDLE


def send(obj, wait=2.0):
    h = k32.CreateFileW(r"\\.\pipe\PBZLab", 0xC0000000, 0, None, 3, 0, None)
    if h in (None, 0, W.HANDLE(-1).value):
        print("管道未就绪：先打开修改器或注入运行库"); return None
    if obj.get("op") == "tower": obj.setdefault("client", "console")
    b = (json.dumps(obj, ensure_ascii=False) + "\n").encode("utf-8"); n = W.DWORD()
    k32.WriteFile(h, b, len(b), C.byref(n), None)
    end, buf, avail = time.time() + wait, b"", W.DWORD()
    while time.time() < end:
        if k32.PeekNamedPipe(h, None, 0, None, C.byref(avail), None) and avail.value:
            t = C.create_string_buffer(avail.value); k32.ReadFile(h, t, avail.value, C.byref(n), None); buf += t.raw[:n.value]
        else:
            time.sleep(0.05)
    k32.CloseHandle(h)
    out = []
    for line in buf.decode("utf-8", "replace").splitlines():
        try: out.append(json.loads(line))
        except ValueError: pass
    return out


def name(x):
    return x["name"] if isinstance(x, dict) else str(x)  # 新模块给 {id,name}，旧模块给纯文字


def show(msgs):
    for m in msgs or []:
        if m.get("op") == "module":
            print("模块：", "已加载" if m.get("ok") else m.get("error"))
        if m.get("op") == "tower" and m.get("event") == "arena":
            if "arenas" in m:
                for a in m["arenas"]:
                    print(f"  区{a['zone']} {a['id']:12} {a['map']:16} 钟 {a['bell']:16} {'已标定' if a['marked'] else '未标定（落在钟旁）'}")
                print(f"  传送：{m.get('travel')} {m.get('why', '')}")
            else:
                print("擂台：", m.get("msg"), {k: m[k] for k in ("id", "level", "x", "y", "z") if k in m} or "")
        if m.get("op") == "tower" and m.get("event") == "probe":
            for k, (base, cur) in m["attrs"].items():
                print(f"  {k:36} 基础 {base:12.3f}  当前 {cur:12.3f}")
        if m.get("op") == "tower" and m.get("event") == "state":
            vs = f"（1v{m['count']}）" if m.get("count", 1) > 1 else ""
            print(f"[{m['phase']}] 第 {m['floor']}/{m['floors']} 层{vs}  Boss：{m['boss'] or '-'}  {m['msg']}")
            g = m.get("gains", {})
            if g.get("jade"):
                ws = "、".join(name(x) for x in g["weapons"]) or "-"
                ss = "、".join(name(x) for x in g["shentong"]) or "-"
                print(f"  本层收获：魂玉 +{g['jade']}  兵器 {ws}  神通 {ss}")
            kinds = {"card": "卡", "item": "道具", "jade": "魂玉"}
            for i, c in enumerate(m["offer"], 1):
                print(f"  {i}. [{kinds[c['kind']]}] {c['name']}  {c['text']}")
            items = "、".join(x["name"] + "×" + str(x["count"]) for x in m.get("items", [])) or "-"
            bag = "、".join(x["name"] for x in m.get("shentong", [])) or "-"
            print(f"  魂玉 {m.get('jade', 0)}  道具 {items}  神通 {bag}")
            if m.get("weapons"):
                print("  兵器：", "、".join(x["name"] + ("（已入存档）" if x["granted"] else "") for x in m["weapons"]))
            if m["picked"]:
                print("  已选卡：", "、".join(name(x) for x in m["picked"]))


def tower(obj, wait=2.0):
    """发塔命令；没有任何影塔回应（模块没加载，比如运行库被热换过）就自动加载一次再重发，并说明原因"""
    msgs = send(obj, wait)
    if msgs and any(m.get("op") == "tower" for m in msgs):
        return msgs
    if msgs is None:  # 管道都连不上：send 已经提示过
        return []
    print("影塔模块没有回应（可能运行库被热换过），自动加载后重试……")
    show(send({"op": "module", "cmd": "load", "path": str(ROOT / "build/core/modules/PBZTower.dll")}))
    time.sleep(1.5)
    msgs = send(obj, wait) or []
    if not any(m.get("op") == "tower" for m in msgs):
        print("仍然没有回应：请把 %LOCALAPPDATA%\PBZLab\core.log 最后几十行发给 Claude")
    return msgs


def main():
    sys.stdout.reconfigure(encoding="utf-8")
    cmd = sys.argv[1] if len(sys.argv) > 1 else "state"
    if cmd == "load":
        show(send({"op": "module", "cmd": "load", "path": str(ROOT / "build/core/modules/PBZTower.dll")}))
    elif cmd == "start":
        obj = {"op": "tower", "cmd": "start", "floors": int(sys.argv[2]) if len(sys.argv) > 2 else 3}
        if "noarena" in sys.argv[3:]: obj["arena"] = "off"  # 不传擂台，就地打
        show(tower(obj))
    elif cmd == "pick":
        show(tower({"op": "tower", "cmd": "pick", "slot": int(sys.argv[2]) - 1}))
    elif cmd == "next":
        show(tower({"op": "tower", "cmd": "next"}, 4.0))
    elif cmd == "use":
        show(tower({"op": "tower", "cmd": "use", "item": sys.argv[2]}))
    elif cmd == "st":
        show(tower({"op": "tower", "cmd": "st", "slot": int(sys.argv[2]) - 1}))
    elif cmd == "arena":
        v = sys.argv[2] if len(sys.argv) > 2 else "list"
        show(tower({"op": "tower", "cmd": "arena", "value": v, "id": sys.argv[3] if len(sys.argv) > 3 else ""}))
    elif cmd == "grant":
        show(tower({"op": "tower", "cmd": "grant", "backup_ok": True}, 4.0))
    else:
        show(tower({"op": "tower", "cmd": cmd}))


if __name__ == "__main__":
    main()
