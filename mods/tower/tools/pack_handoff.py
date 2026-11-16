# 交付包：build/handoff/<commit>/ 放 PBZCore / PBZTower 的 DLL + PDB 和 manifest.json（SHA256、PE / PDB GUID 与 age、是否一致）。
# 用法（仓库根目录）：python -X utf8 -I mods/tower/tools/pack_handoff.py <源码提交短哈希>
# 已存在的目录绝不覆盖（每批新包一个新目录）。PDB 的 age 取 DBI 流（与 PE 的 RSDS 对比）。
import hashlib, json, os, shutil, struct, sys, uuid
commit = sys.argv[1]
root = r'E:\PBZ-claude\build'
out = os.path.join(root, 'handoff', commit)
assert not os.path.exists(out), 'package exists, never overwrite'
os.makedirs(out)
src = {'PBZCore': os.path.join(root, 'core', 'PBZCore'), 'PBZTower': os.path.join(root, 'core', 'modules', 'PBZTower')}

def sha(p): return hashlib.sha256(open(p, 'rb').read()).hexdigest()

def pe_rsds(p):
    d = open(p, 'rb').read()
    i = d.find(b'RSDS')
    while i >= 0:
        g = d[i + 4:i + 20]; age = struct.unpack_from('<I', d, i + 20)[0]
        if d[i + 24:].split(b'\0', 1)[0].lower().endswith(b'.pdb'):
            return str(uuid.UUID(bytes_le=g)).replace('-', '').upper(), age
        i = d.find(b'RSDS', i + 4)

def pdb_info(p):
    d = open(p, 'rb').read()
    bs = struct.unpack_from('<I', d, 32)[0]
    dirsize = struct.unpack_from('<I', d, 44)[0]
    mapblk = struct.unpack_from('<I', d, 52)[0]
    ndir = (dirsize + bs - 1) // bs
    dirblocks = struct.unpack_from(f'<{ndir}I', d, mapblk * bs)
    dr = b''.join(d[b * bs:(b + 1) * bs] for b in dirblocks)[:dirsize]
    n = struct.unpack_from('<I', dr, 0)[0]
    sizes = struct.unpack_from(f'<{n}I', dr, 4)
    off = 4 + 4 * n
    blocks = []
    for s in sizes:
        k = 0 if s == 0xFFFFFFFF else (s + bs - 1) // bs
        blocks.append(struct.unpack_from(f'<{k}I', dr, off)); off += 4 * k
    s1 = b''.join(d[b * bs:(b + 1) * bs] for b in blocks[1])
    # DBI stream (3) age is what the PE matches; info stream (1) has the GUID
    s3 = b''.join(d[b * bs:(b + 1) * bs] for b in blocks[3])
    g = s1[12:28]
    age = struct.unpack_from('<I', s3, 8)[0]
    return str(uuid.UUID(bytes_le=g)).replace('-', '').upper(), age

m = {'source_commit': commit, 'module_abi': 6}
for name, base in src.items():
    for ext in ('.dll', '.pdb'):
        shutil.copy2(base + ext, os.path.join(out, name + ext))
    dll, pdb = os.path.join(out, name + '.dll'), os.path.join(out, name + '.pdb')
    pg, pa = pe_rsds(dll); dg, da = pdb_info(pdb)
    m[name] = dict(dll_sha256=sha(dll), pdb_sha256=sha(pdb), pe_guid=pg, pe_age=pa, pdb_guid=dg, pdb_age=da, match=(pg, pa) == (dg, da))
json.dump(m, open(os.path.join(out, 'manifest.json'), 'w'), indent=1)
print(json.dumps(m, indent=1))
