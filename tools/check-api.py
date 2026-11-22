#!/usr/bin/env python3
"""The public API, frozen at 1.0: every name docs/api-1.0.txt lists for a
header is still declared there, and every name a public header declares
is listed. Names are only added after 1.0 (add them to the list in the
same change); none is removed or renamed.

  tools/check-api.py           check
  tools/check-api.py --write   rewrite the list from the headers
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LIST = os.path.join(ROOT, "docs", "api-1.0.txt")
HEADERS = ["source/wm.h", "source/wm_wasi.h", "source/wm_dylink.h", "source/wm_heap.h"]
SKIP = {"if", "for", "while", "switch", "return", "sizeof", "defined", "__attribute__", "import_module",
        "import_name", "typedef", "do", "void", "int", "char", "unsigned", "signed", "short", "long", "float",
        "double", "const", "volatile", "static", "inline", "extern", "struct", "enum", "union"}


def names(path):
    """Macros, functions, types and the structs' and enums' names a header declares."""
    text = open(os.path.join(ROOT, path)).read()
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    text = re.sub(r"//[^\n]*", "", text)
    found = set()
    for match in re.finditer(r"^\s*#\s*define\s+(\w+)", text, re.M):
        if not match.group(1).endswith("_H"):
            found.add(match.group(1))
    text = re.sub(r"^[ \t]*#[^\n]*", "", text.replace("\\\n", " "), flags=re.M)
    flat = re.sub(r"\s+", " ", text)
    for match in re.finditer(r"\b(\w+)\s*\(", flat):
        if match.group(1) not in SKIP:
            found.add(match.group(1))
    for match in re.finditer(r"\b(?:struct|enum|union)\s+(\w+)", flat):
        found.add(match.group(1))
    for start in [m.start() for m in re.finditer(r"\btypedef\b", flat)]:
        depth, at = 0, start
        while at < len(flat) and not (flat[at] == ";" and depth == 0):
            depth += {"{": 1, "}": -1}.get(flat[at], 0)
            at += 1
        tail = flat[start:at]
        pointer = re.search(r"\(\s*\*\s*(\w+)\s*\)\s*\(", tail[tail.rfind("}") + 1:])
        last = re.findall(r"(\w+)\s*(?:\[[^\]]*\])?\s*$", tail)
        if pointer:
            found.add(pointer.group(1))
        elif last:
            found.add(last[0])
    for body in re.findall(r"\benum\b[^{;]*\{([^}]*)\}", flat):
        for item in body.split(","):
            item = item.split("=")[0].strip()
            if re.fullmatch(r"\w+", item):
                found.add(item)
    return found


def read_list():
    listed, header = {}, None
    for line in open(LIST):
        line = line.strip()
        if line.startswith("# "):
            header = line[2:]
            listed[header] = set()
        elif line and not line.startswith("#"):
            listed[header].add(line)
    return listed


def main():
    current = {header: names(header) for header in HEADERS}
    if "--write" in sys.argv:
        with open(LIST, "w") as out:
            out.write("# The public API at 1.0, by header (tools/check-api.py). Names are only added.\n")
            for header in HEADERS:
                out.write("\n# %s\n" % header)
                out.write("".join(name + "\n" for name in sorted(current[header])))
        print("wrote %s" % os.path.relpath(LIST, ROOT))
        return 0
    listed, fail = read_list(), 0
    for header in HEADERS:
        gone = sorted(listed.get(header, set()) - current[header])
        new = sorted(current[header] - listed.get(header, set()))
        for name in gone:
            print("FAIL %s: %s is gone (the API only grows after 1.0)" % (header, name))
        for name in new:
            print("FAIL %s: %s is new: add it to docs/api-1.0.txt" % (header, name))
        fail += len(gone) + len(new)
    if not fail:
        print("ok   API: %d names in %d headers" % (sum(len(v) for v in current.values()), len(HEADERS)))
    return 1 if fail else 0


if __name__ == "__main__":
    sys.exit(main())
