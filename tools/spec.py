#!/usr/bin/env python3
"""Run the WebAssembly spec tests (JSON and .wasm from wast2json) against wm-spec.

    tools/spec.py <wm-spec binary> <json directory> [test names...] [-v] [--trusted]

--trusted is for engines built without validation: checks that a module is
refused are skipped, as such an engine checks nothing.
"""
import json
import os
import subprocess
import sys


class Engine:
    def __init__(self, binary):
        self.binary = binary
        self.start()

    def start(self):
        self.proc = subprocess.Popen([self.binary], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)

    def ask(self, line):
        try:
            self.proc.stdin.write(line + "\n")
            self.proc.stdin.flush()
            answer = self.proc.stdout.readline()
        except BrokenPipeError:
            answer = ""
        if not answer:
            self.proc.wait()
            code = self.proc.returncode
            self.start()
            return "crash exit %s" % code
        return answer.strip()


def canonical_nan(t, bits):
    return (bits & 0x7FFFFFFF) == 0x7FC00000 if t == "f32" else (bits & 0x7FFFFFFFFFFFFFFF) == 0x7FF8000000000000


def arithmetic_nan(t, bits):
    return (bits & 0x7FC00000) == 0x7FC00000 if t == "f32" else (bits & 0x7FF8000000000000) == 0x7FF8000000000000


def matches(expected, actual):
    t, value = actual.split(":", 1)
    if expected["type"] != t:
        return False
    want = expected.get("value")
    if t in ("i32", "i64", "f32", "f64"):
        bits = int(value)
        if want == "nan:canonical":
            return canonical_nan(t, bits)
        if want == "nan:arithmetic":
            return arithmetic_nan(t, bits)
        return int(want) == bits
    if want == "null":
        return value == "null"
    if t == "funcref":
        return value != "null"
    return value == want


def hexname(name):
    return name.encode("utf-8").hex() or "-"


def arg(a):
    return "%s:%s" % (a["type"], a["value"])


def module_name(name):
    return name if name else "-"


def run_file(engine, path, verbose):
    data = json.load(open(path))
    base = os.path.dirname(path)
    passed = failed = skipped = 0
    failures = []
    engine.ask("reset")

    def action(a):
        target = module_name(a.get("module"))
        if a["type"] == "invoke":
            line = "invoke %s %s %s" % (target, hexname(a["field"]), " ".join(arg(x) for x in a["args"]))
        else:
            line = "get %s %s" % (target, hexname(a["field"]))
        return engine.ask(line.strip())

    for cmd in data["commands"]:
        kind = cmd["type"]
        where = "%s:%d" % (os.path.basename(path), cmd["line"])
        ok = True
        detail = ""
        if kind == "module":
            answer = engine.ask("module %s %s 1" % (os.path.join(base, cmd["filename"]), module_name(cmd.get("name"))))
            ok = answer == "ok"
            detail = answer
        elif kind == "register":
            answer = engine.ask("register %s %s" % (hexname(cmd["as"]), module_name(cmd.get("name"))))
            ok = answer == "ok"
            detail = answer
        elif kind == "action":
            answer = action(cmd["action"])
            ok = answer.startswith("ok")
            detail = answer
        elif kind == "assert_return":
            answer = action(cmd["action"])
            if not answer.startswith("ok"):
                ok = False
            else:
                values = answer.split()[1:]
                expected = cmd["expected"]
                ok = len(values) == len(expected) and all(matches(e, v) for e, v in zip(expected, values))
            detail = "%s, expected %s" % (answer, cmd["expected"])
        elif kind in ("assert_trap", "assert_exhaustion"):
            if "action" in cmd:
                answer = action(cmd["action"])
            else:
                answer = engine.ask("module %s - 0" % os.path.join(base, cmd["filename"]))
            prefix = "error trap:" if kind == "assert_trap" else "error exhaustion:"
            ok = answer.startswith(prefix)
            detail = "%s, expected %s" % (answer, cmd["text"])
        elif kind in ("assert_invalid", "assert_malformed"):
            if cmd.get("module_type") == "text" or TRUSTED:
                skipped += 1
                continue
            answer = engine.ask("check %s" % os.path.join(base, cmd["filename"]))
            ok = answer.startswith("error malformed:") or answer.startswith("error invalid:")
            detail = "%s, expected %s" % (answer, cmd["text"])
        elif kind in ("assert_unlinkable", "assert_uninstantiable"):
            answer = engine.ask("module %s - 0" % os.path.join(base, cmd["filename"]))
            prefix = "error unlinkable:" if kind == "assert_unlinkable" else "error trap:"
            ok = answer.startswith(prefix)
            detail = "%s, expected %s" % (answer, cmd["text"])
        else:
            skipped += 1
            continue
        if ok:
            passed += 1
        else:
            failed += 1
            failures.append("%s %s: %s" % (where, kind, detail))
    if verbose:
        for f in failures:
            print("  FAIL " + f)
    return passed, failed, skipped, failures


TRUSTED = "--trusted" in sys.argv


def main():
    args = [a for a in sys.argv[1:] if a not in ("-v", "--trusted")]
    verbose = "-v" in sys.argv
    binary, directory = args[0], args[1]
    names = args[2:]
    files = sorted(f for f in os.listdir(directory) if f.endswith(".json"))
    if names:
        files = [f for f in files if f[:-5] in names]
    engine = Engine(binary)
    total_passed = total_failed = total_skipped = 0
    for f in files:
        passed, failed, skipped, failures = run_file(engine, os.path.join(directory, f), verbose)
        total_passed += passed
        total_failed += failed
        total_skipped += skipped
        status = "ok  " if failed == 0 else "FAIL"
        print("%s %-28s %5d passed %4d failed" % (status, f[:-5], passed, failed))
        if failed and not verbose:
            for line in failures[:3]:
                print("     " + line)
    print("total: %d passed, %d failed, %d skipped (text-format or validation checks)" % (total_passed, total_failed, total_skipped))
    sys.exit(1 if total_failed else 0)


if __name__ == "__main__":
    main()
