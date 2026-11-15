#!/usr/bin/env python3
"""What ran, from the counts of a WM_PROFILE build (source/wm_internal.h).

    WM_PROFILE_FILE=build/prof/coremark build/wm-bench-profile prog.wasm
    tools/profile.py build/prof/coremark.*  [--top N]

Several files (processes, programs) add up. Prints the records the fast
loop ran by kind, the pairs of plain records that ran in a row (candidates
for fusing), and the opcodes the loop in place ran.
"""
import os
import re
import sys
from collections import Counter

HERE = os.path.dirname(os.path.abspath(__file__))
OPS = {
    0x00: "unreachable", 0x01: "nop", 0x02: "block", 0x03: "loop", 0x04: "if", 0x05: "else", 0x0B: "end",
    0x0C: "br", 0x0D: "br_if", 0x0E: "br_table", 0x0F: "return", 0x10: "call", 0x11: "call_indirect",
    0x1A: "drop", 0x1B: "select", 0x1C: "select t", 0x20: "local.get", 0x21: "local.set", 0x22: "local.tee",
    0x23: "global.get", 0x24: "global.set", 0x25: "table.get", 0x26: "table.set", 0x28: "i32.load",
    0x29: "i64.load", 0x2A: "f32.load", 0x2B: "f64.load", 0x2C: "i32.load8_s", 0x2D: "i32.load8_u",
    0x2E: "i32.load16_s", 0x2F: "i32.load16_u", 0x30: "i64.load8_s", 0x31: "i64.load8_u",
    0x32: "i64.load16_s", 0x33: "i64.load16_u", 0x34: "i64.load32_s", 0x35: "i64.load32_u",
    0x36: "i32.store", 0x37: "i64.store", 0x38: "f32.store", 0x39: "f64.store", 0x3A: "i32.store8",
    0x3B: "i32.store16", 0x3C: "i64.store8", 0x3D: "i64.store16", 0x3E: "i64.store32",
    0x3F: "memory.size", 0x40: "memory.grow", 0x41: "i32.const", 0x42: "i64.const", 0x43: "f32.const",
    0x44: "f64.const", 0x45: "i32.eqz", 0x46: "i32.eq", 0x47: "i32.ne", 0x48: "i32.lt_s", 0x49: "i32.lt_u",
    0x4A: "i32.gt_s", 0x4B: "i32.gt_u", 0x4C: "i32.le_s", 0x4D: "i32.le_u", 0x4E: "i32.ge_s",
    0x4F: "i32.ge_u", 0x50: "i64.eqz", 0x51: "i64.eq", 0x52: "i64.ne", 0x53: "i64.lt_s", 0x54: "i64.lt_u",
    0x55: "i64.gt_s", 0x56: "i64.gt_u", 0x57: "i64.le_s", 0x58: "i64.le_u", 0x59: "i64.ge_s",
    0x5A: "i64.ge_u", 0x67: "i32.clz", 0x68: "i32.ctz", 0x69: "i32.popcnt", 0x6A: "i32.add",
    0x6B: "i32.sub", 0x6C: "i32.mul", 0x6D: "i32.div_s", 0x6E: "i32.div_u", 0x6F: "i32.rem_s",
    0x70: "i32.rem_u", 0x71: "i32.and", 0x72: "i32.or", 0x73: "i32.xor", 0x74: "i32.shl", 0x75: "i32.shr_s",
    0x76: "i32.shr_u", 0x77: "i32.rotl", 0x78: "i32.rotr", 0x7C: "i64.add", 0x7D: "i64.sub", 0x7E: "i64.mul",
    0x83: "i64.and", 0x84: "i64.or", 0x85: "i64.xor", 0x86: "i64.shl", 0x87: "i64.shr_s", 0x88: "i64.shr_u",
    0xA7: "i32.wrap_i64", 0xAC: "i64.extend_i32_s", 0xAD: "i64.extend_i32_u", 0xC0: "i32.extend8_s",
    0xC1: "i32.extend16_s", 0xFC: "0xFC prefix",
}


def fused_kinds():
    """The fused records' names, from source/wm_ops.def."""
    text = open(os.path.join(HERE, "..", "source", "wm_ops.def")).read()
    return set(re.findall(r"^WM_RECORD_FUSED(?:_BRANCH)?\(\w+, (\w+)\)", text, re.M))


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
    records, pairs, ops = Counter(), Counter(), Counter()
    for path in args:
        for line in open(path):
            parts = line.split()
            if parts[0] == "record":
                records[parts[1]] += int(parts[2])
            elif parts[0] == "pair":
                pairs[(parts[1], parts[2])] += int(parts[3])
            elif parts[0] == "op":
                ops[int(parts[1], 16)] += int(parts[2])
    fused = fused_kinds()
    dispatches = sum(records.values())
    in_fused = sum(n for k, n in records.items() if k in fused)
    print(f"records run: {dispatches:,}" + (f", {in_fused / dispatches * 100:.1f}% of them fused" if dispatches else ""))
    table("Records by kind", records, dispatches, top)
    plain = Counter({p: n for p, n in pairs.items() if p[0] not in fused and p[1] not in fused})
    table("Plain records in a row (candidates for fusing)", plain, dispatches, top, lambda p: f"{p[0]} -> {p[1]}")
    table("Opcodes in place", ops, sum(ops.values()), top, lambda op: OPS.get(op, f"0x{op:02X}"))


if __name__ == "__main__":
    main()
