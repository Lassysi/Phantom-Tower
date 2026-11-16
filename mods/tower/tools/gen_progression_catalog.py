# 由 Codex 的装备进度表（mods/tower/data/equipment-progression.json）生成影塔内置的装备准入表
# （src/progression_catalog.generated.h，ParseCatalog 的一行一件格式）。
# 只认合同里已批准的字段：first_tower_zone（null = 待核，-1 不进池）与 allow_reward / allow_shop；
# proposed_first_tower_zone 是待审提案，这里不读。区从 1 起编号，引擎内从 0 起。
# 用法：python -I mods/tower/tools/gen_progression_catalog.py [--check]
import hashlib
import json
import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
SRC = ROOT / "data" / "equipment-progression.json"
OUT = ROOT / "src" / "progression_catalog.generated.h"
KIND = {"weapon": "main", "shaqi_weapon": "shaqi", "accessory": "acc", "medicine": "med"}


def main():
    raw = SRC.read_bytes()
    data = json.loads(raw.decode("utf-8"))
    if data.get("schema") != 1 or not data.get("policy", {}).get("pending_is_blocked"):
        sys.exit("equipment-progression.json: unexpected schema/policy, refusing to generate")
    lines, open_count = [], 0
    for item in data["items"]:
        cat = KIND.get(item["kind"])
        if not cat:
            sys.exit(f"unknown kind {item['kind']} for {item['id']}")
        zone = item["first_tower_zone"]
        if zone is None:
            z, reward, shop = -1, 0, 0  # 待核 / 排除：禁入，不兜底成一区
        else:
            if not 1 <= zone <= 5:
                sys.exit(f"{item['id']}: first_tower_zone {zone} out of range")
            z, reward, shop = zone - 1, int(bool(item["allow_reward"])), int(bool(item["allow_shop"]))
            open_count += reward or shop
        boss = int(bool(item["boss_associated"]))
        lines.append(f"{cat} {item['id']} {z} {boss} {reward} {shop}")
    digest = hashlib.sha256(raw).hexdigest()
    body = "\n".join(lines)
    text = (
        "// 生成文件，勿手改：python -I mods/tower/tools/gen_progression_catalog.py\n"
        f"// 来源 mods/tower/data/equipment-progression.json sha256={digest}\n"
        f"// {len(lines)} 项，其中 {open_count} 项可进奖励或商店；其余待核 / 排除（区 -1）\n"
        "#pragma once\n"
        "namespace tower::rewards {\n"
        f"inline constexpr const char* kProgressionSha256 = \"{digest}\";\n"
        f"inline constexpr int kProgressionItems = {len(lines)};\n"
        f"inline constexpr const char* kProgressionCatalog = R\"PBZ(\n{body}\n)PBZ\";\n"
        "}  // namespace tower::rewards\n")
    if "--check" in sys.argv:
        if not OUT.exists() or OUT.read_text(encoding="utf-8") != text:
            sys.exit(f"{OUT.name} is stale: run python -I mods/tower/tools/gen_progression_catalog.py")
        print(f"{OUT.name}: up to date ({len(lines)} items, {open_count} open)")
        return
    OUT.write_text(text, encoding="utf-8", newline="\n")
    print(f"{OUT.name}: {len(lines)} items, {open_count} open, sha256 {digest[:12]}")


if __name__ == "__main__":
    main()
