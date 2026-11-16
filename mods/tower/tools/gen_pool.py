"""Read JSON audit catalog and tiers. Never overwrite engine headers.

--check validates offline preparation tables. Default stdout contains only
live-audited selectable bosses. --include-unverified is for offline review only.
The engine's old compiled header remains its fallback until Claude adds JSON I/O.
"""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from validate_data import DATA, Validator


def select(rows, include_unverified=False):
    return sorted((b for b in rows if b.get("selectable") and b.get("role") == "primary"
                   and (include_unverified or b.get("spawn_verified") is True)),
                  key=lambda b: (b["tier"], b["path"]))


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--check", action="store_true")
    p.add_argument("--include-unverified", action="store_true")
    args = p.parse_args()
    checked = Validator(DATA).validate()
    if checked.errors:
        raise SystemExit("\n".join(checked.errors))
    pool = select(checked.rows["bosses"], args.include_unverified)
    if args.check:
        print(f"boss JSON audit catalog: {len(checked.rows['bosses'])}; live-ready selectable: {len(select(checked.rows['bosses']))}; OK")
    else:
        print(json.dumps({"schema": 1, "source": "mods/tower/data/bosses.json", "generated": checked.docs["bosses"]["generated"], "includes_unverified": args.include_unverified, "items": pool}, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
