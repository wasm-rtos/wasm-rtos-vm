#!/usr/bin/env python3
"""What ran, from the counts of a WM_PROFILE build (source/wm_internal.h).

    WM_PROFILE_FILE=build/prof/coremark build/wm-bench-profile prog.wasm
    tools/profile.py build/prof/coremark.*  [--top N]

Several files (processes, programs) add up. Prints the records the fast
loop ran by kind, the pairs of plain records that ran in a row and of a
fused record then a plain one (candidates for fusing), the opcodes that went through its shared numeric and access
records, and the opcodes the loop in place ran.
"""
import os
import re
import sys
from collections import Counter

HERE = os.path.dirname(os.path.abspath(__file__))
DEFS = open(os.path.join(HERE, "..", "source", "wm_defs.def")).read()
SPACES = ("i32", "i64", "f32", "f64", "local", "global", "table", "memory", "ref", "data", "elem")


def text_name(name):
    """i32_load8_s as WebAssembly writes it: i32.load8_s; br_if stays."""
    head, _, rest = name.partition("_")
    return f"{head}.{rest}" if rest and head in SPACES else name


# Every opcode's name, from source/wm_defs.def (the i32 instructions both loops share are a list there).
OPS = {int(op, 16): text_name(name)
       for op, name in re.findall(r"^WM_WASM(?:_ACCESS|_NUM)?\((0x[0-9A-F]{2}), \w+, (\w+)", DEFS, re.M)}
OPS.update({int(op, 16): "i32." + name
            for name, op in re.findall(r"X\(\w+, (\w+), (0x[0-9A-F]{2}), [12],", DEFS)})
OPS[0xFC] = "0xFC prefix"


def fused_kinds():
    """The records made from the shared i32 lists (two forms each, and compares with branches)."""
    names = re.findall(r"X\(\w+, (\w+), 0x[0-9A-F]{2}, [12],", DEFS)
    return {f"{form}_{n}" for n in names for form in ("ss", "sc", "brif_ss", "brif_sc")}


def table(title, counts, total, top, name=str):
    print(f"\n{title}")
    if not total:
        print("  none")
        return
    for key, n in counts.most_common(top):
        print(f"  {n / total * 100:6.2f}%  {n:>14,}  {name(key)}")


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    top = 25
    if "--top" in sys.argv:
        top = int(sys.argv[sys.argv.index("--top") + 1])
        args = [a for a in args if a != str(top)]
    records, pairs, ops, shared = Counter(), Counter(), Counter(), Counter()
    for path in args:
        for line in open(path):
            parts = line.split()
            if parts[0] == "record":
                records[parts[1]] += int(parts[2])
            elif parts[0] == "pair":
                pairs[(parts[1], parts[2])] += int(parts[3])
            elif parts[0] == "op":
                ops[int(parts[1], 16)] += int(parts[2])
            elif parts[0] == "shared":
                shared[int(parts[1], 16)] += int(parts[2])
    fused = fused_kinds()
    dispatches = sum(records.values())
    in_fused = sum(n for k, n in records.items() if k in fused)
    print(f"records run: {dispatches:,}" + (f", {in_fused / dispatches * 100:.1f}% of them fused" if dispatches else ""))
    table("Records by kind", records, dispatches, top)
    plain = Counter({p: n for p, n in pairs.items() if p[0] not in fused and p[1] not in fused})
    table("Plain records in a row (candidates for fusing)", plain, dispatches, top, lambda p: f"{p[0]} -> {p[1]}")
    # A fused record before a plain one: the run can grow by the second (decision 14).
    after = Counter({p: n for p, n in pairs.items() if p[0] in fused and p[1] not in fused})
    table("Fused records, then a plain one (candidates for longer runs)", after, dispatches, top,
          lambda p: f"{p[0]} -> {p[1]}")
    table("Through the shared numeric and access records", shared, dispatches, top,
          lambda op: OPS.get(op, f"0x{op:02X}"))
    table("Opcodes in place", ops, sum(ops.values()), top, lambda op: OPS.get(op, f"0x{op:02X}"))


if __name__ == "__main__":
    main()
