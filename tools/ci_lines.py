#!/usr/bin/env python3
"""Source lines of the render path's hot functions, by profiler bucket (CI log; see the workflow).

The profiler reports dsr+0x... offsets rounded down to 64 bytes. For every such bucket inside the functions
named on the command line (substring match on the demangled name), prints the source lines its instructions
come from, innermost inlined function first: `bucket: func file:line; ...`.
"""
import json
import subprocess
import sys

nm, symbolizer, lib = sys.argv[1:4]
wanted = sys.argv[4:]
funcs = []
for line in subprocess.run([nm, "-C", "-S", "--defined-only", lib], capture_output=True, text=True).stdout.splitlines():
    p = line.split(None, 3)
    if len(p) == 4 and p[2] in "tTwW" and any(w in p[3] for w in wanted):
        start, size = int(p[0], 16), int(p[1], 16)
        if size:
            funcs.append((start, size, p[3]))
for start, size, name in sorted(funcs):
    print(f"## {start:x} {name[:160]}")
    first = start & ~63
    addrs = list(range(start, start + size, 4))
    out = subprocess.run([symbolizer, "--obj=" + lib, "-f", "-i", "-C", "--output-style=JSON"],
                         input="\n".join(hex(a) for a in addrs), capture_output=True, text=True).stdout
    buckets = {}
    for line in out.splitlines():
        line = line.strip()
        if not line.startswith("{"):
            continue
        r = json.loads(line)
        a = int(r.get("Address", "0"), 16)
        pairs = []
        for f in r.get("Symbol", []):
            fn = f.get("FunctionName", "").replace("(anonymous namespace)::", "").split("(")[0].split("::")[-1][:40]
            pairs.append(f"{fn} {f.get('FileName', '').rsplit('/', 1)[-1]}:{f.get('Line', 0)}")
        key = " < ".join(pairs[:3])
        b = buckets.setdefault(a & ~63, [])
        if key not in b:
            b.append(key)
    for b in sorted(buckets):
        print(f"{b:x}: " + " | ".join(buckets[b][:6]))
