#!/usr/bin/env python3
"""Lint: no direct component member writes outside editor/commands/ (§6.2 review rule).

Every user-facing editor mutation must route through a command (raven::ICommand via
editor/commands/*) so it is undoable. This lint flags direct writes such as:

    world.GetComponent<TransformComponent>(e).position = v;   // chained write
    auto& t = world.GetComponent<TransformComponent>(e);      // aliased write
    t.position = v;

Escape hatch for intentional exceptions (should not exist): append
`// raven-allow-direct-write` on the offending line.

Exits 1 with a file:line report when any violation is found.
"""

import os
import re
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EDITOR = os.path.join(REPO, "editor")
COMMANDS_DIR = os.path.join(EDITOR, "commands")
ALLOW = "raven-allow-direct-write"

# Chained: ...GetComponent<T>(...).member <op> (but not ==/!=)
CHAINED = re.compile(
    r"GetComponent\s*<\s*[A-Za-z_][\w:]*\s*>\s*\([^;]*?\)\s*"   # GetComponent<T>(...)
    r"\.\s*[A-Za-z_]\w*\s*"                                     # .member
    r"(?:\.\s*[A-Za-z_]\w*\s*)*"                                # (nested member)*
    r"(=(?![=<>!])|\+=|-=|\*=|/=|%=|\+\+|--)"                   # assignment-ish operator
)
# Aliased: auto& name = ...GetComponent<...>
ALIAS_DECL = re.compile(r"auto\s*&\s*([A-Za-z_]\w*)\s*=[^;]*?GetComponent\s*<")


def member_write_re(name):
    return re.compile(
        r"\b" + re.escape(name) + r"\s*\.\s*[A-Za-z_]\w*\s*"
        r"(?:\.\s*[A-Za-z_]\w*\s*)*"
        r"(=(?![=<>!])|\+=|-=|\*=|/=|%=|\+\+|--)"
    )


def scan_file(path):
    violations = []
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        lines = f.readlines()
    aliased = set()
    for lineno, line in enumerate(lines, 1):
        code = line.split("//", 1)[0] if ALLOW not in line else ""   # allow-marked lines are skipped
        if code:
            for m in CHAINED.finditer(code):
                violations.append((lineno, ("chained", m.group(0).strip())))
            for m in ALIAS_DECL.finditer(code):
                aliased.add(m.group(1))
                continue
            for name in aliased:
                if member_write_re(name).search(code):
                    violations.append((lineno, ("aliased '%s'" % name, code.strip())))
    return violations


def main():
    total = 0
    for root, dirs, files in os.walk(EDITOR):
        dirs[:] = [d for d in dirs if d not in (".cache", "build")]
        for fn in files:
            if not (fn.endswith(".h") or fn.endswith(".cpp")):
                continue
            path = os.path.join(root, fn)
            if os.path.commonpath([os.path.abspath(path), os.path.abspath(COMMANDS_DIR)]) == os.path.abspath(COMMANDS_DIR):
                continue   # editor/commands/* is where the writes live
            for lineno, (kind, snippet) in scan_file(path):
                rel = os.path.relpath(path, REPO)
                print("%s:%d: %s write: %s" % (rel, lineno, kind, snippet[:120]))
                total += 1
    if total:
        print("\n%d direct component write(s) outside editor/commands/ — route mutations through a command (§6.2)." % total)
        return 1
    print("check_editor_no_direct_writes: OK — no direct component writes outside editor/commands/")
    return 0


if __name__ == "__main__":
    sys.exit(main())
