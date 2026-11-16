# 发布前自查：统计每个海克斯在正式版（不在 #ifdef PBZ_HEX_TEST 内）有多少处引用。release=0 且不是在 tower_hex.h 聚合函数里用 Has(X) 的，要逐个查。
# 用法：cd mods/tower/src; python -X utf8 ../tools/hex-release-scan.py
import re,glob,collections
ids="Tank Swift Icathia Leech Ocean Sharp Bell Battle BreakGrowth Desperate Sturdy Keen Pursuit Bulwark Escape Grit KillingIntent Thrower Scientist Forge Investor Burn Heavy Scavenger AttributeStack GlassCannon BloodPact Decapitate Sweep Fury Opening FirstStrike Reaper Interest Charmsmith Arsenal SnowBall Fortune Prepared Ascetic Flawless Blitz Echo Arcane BackToBasics Transmutation ServantMaster DivineAid".split()
rel=collections.Counter(); qa=collections.Counter()
for f in glob.glob('*.cpp')+glob.glob('*.h')+glob.glob('*.inc'):
    stack=[]
    for line in open(f,encoding='utf-8',errors='ignore'):
        s=line.strip()
        if s.startswith('#if'):
            stack.append('Q' if 'PBZ_HEX_TEST' in s and 'ifndef' not in s and '!' not in s else ('R' if 'ifndef PBZ_HEX_TEST' in s else 'O'))
        elif s.startswith('#else') and stack:
            stack[-1]={'Q':'R','R':'Q'}.get(stack[-1],stack[-1])
        elif s.startswith('#endif') and stack: stack.pop()
        inq='Q' in stack
        for i in ids:
            if re.search(r'\b(hex|hx)::'+i+r'\b',line) or re.search(r'\bcase '+i+r'\b',line):
                (qa if inq else rel)[(i,f)]+=1
for i in ids:
    r=sum(v for (k,f),v in rel.items() if k==i); q=sum(v for (k,f),v in qa.items() if k==i)
    files=sorted({f for (k,f) in rel if k==i})
    print(f"{i:15} release={r:3} qa_only={q:2} {files}")
