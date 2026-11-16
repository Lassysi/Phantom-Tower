"""Compare encounter definitions and native contracts to the v1.3.5 tag.

The two final numeric fields are intentionally omitted: 2.0 has separate,
user-approved balance changes. This does not validate runtime or random order.
"""
from pathlib import Path
import hashlib
import re

src = Path(__file__).resolve().parents[1] / 'src'
baseline = {
    'tower_replay.h': '931f45bdde4d6b01470b5ebec9c8885335d13e97a76a654af06ad708d8686787',
    'tower_interactions.cpp': '352fc98f5ad2af3fe57c978dffad29208a474a8448799ec6808a310dad901581',
    'tower_venue.cpp': '3a717344b115714972d485ef65ceee39db2c7d5b6ade1f975b3a0b41b8d2f218',  # 3.0.0：雾门每秒核对、被原生改回就重关（用户 2026-11-12 雾门锁不住）
}
def digest(s): return hashlib.sha256(s.encode('utf-8')).hexdigest()
for name, expected in baseline.items():
    actual = digest((src / name).read_text(encoding='utf-8').replace('\r\n', '\n'))
    if actual != expected: raise SystemExit(f'v1.3.5 boss contract changed: {name}')
floor_text = (src / 'floors.generated.h').read_text(encoding='utf-8').replace('\r\n', '\n')
for section, count, expected in [
    ('kFloors', 50, '49049ae831660fe84c4f695e62223209a15767e051185b66f8a454614e3499ab'),
    ('kHiddenFloors', 1, '44c6dc894fd6c95d478fd70061e3ead23e7aaaea1ec078dfda71b6e0bace3d6e'),
]:
    body = floor_text.split(f'inline constexpr Floor {section}[] = {{', 1)[1].split('\n};', 1)[0]
    rows = [re.sub(r',\s*\d+(?:\.\d+)?,\s*\d+(?:\.\d+)?\},?\s*$', '', line.strip())
            for line in body.splitlines() if re.match(r'\s*\{\d+,', line)]
    if len(rows) != count or digest('\n'.join(rows) + '\n') != expected:
        raise SystemExit(f'v1.3.5 boss stage structure changed: {section}')
print('BOSS_BASELINE_OK 50 stages + hidden stage, 3 native contract files (static)')
