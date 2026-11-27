#!/usr/bin/env python3
"""Layering guard — engine and sandbox must never depend on the editor.

Raven layering rule (docs/EDITOR_DESIGN.md §1):

    engine/        static lib — ZERO editor dependencies (headers or link libs)
    editor/        static lib + thin exe — everything editor
    sandbox/       example app — links the engine only, never raven_editor/ravened

Three checks, all fail-fast:

  1. Link graph: no engine target (raven, raven_core, raven_render, ...) and no
     sandbox/test target links raven_editor or ravened.
  2. Includes: no file under engine/ or sandbox/ includes editor paths
     ("editor/...", "raven_editor", "RavenEditorHooks", "raven/editor/...").
  3. editor/ must not be included by third-party (out of scope, kept simple).

Exit code 0 = layering clean; 1 = violation(s) found (printed with file:line).
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

ENGINE_DIR = ROOT / "engine"
SANDBOX_DIR = ROOT / "sandbox"
EDITOR_DIR = ROOT / "editor"
SKIP_DIRS = {".git", "build", "out", "third_party", ".cache", "node_modules"}

FORBIDDEN_LINK = re.compile(r"\b(raven_editor|ravened)\b")
FORBIDDEN_INCLUDE = re.compile(
    r"""#\s*include\s*[<"]\s*(editor/|raven_editor|RavenEditorHooks|raven/editor/)"""
)
SOURCE_SUFFIXES = {".h", ".hpp", ".hh", ".cpp", ".cc", ".cxx", ".inl", ".mm"}

violations = []


def iter_files(base):
    if not base.exists():
        return
    for path in base.rglob("*"):
        if not path.is_file():
            continue
        if any(part in SKIP_DIRS for part in path.parts):
            continue
        yield path


def check_includes():
    for base in (ENGINE_DIR, SANDBOX_DIR):
        for path in iter_files(base):
            if path.suffix not in SOURCE_SUFFIXES:
                continue
            try:
                text = path.read_text(encoding="utf-8", errors="ignore")
            except OSError:
                continue
            for lineno, line in enumerate(text.splitlines(), start=1):
                if FORBIDDEN_INCLUDE.search(line):
                    violations.append(f"{path.relative_to(ROOT)}:{lineno}: editor include in engine/sandbox: {line.strip()}")


def check_link_graph():
    # Scan every CMakeLists.txt for target_link_libraries() calls that name an
    # engine/sandbox/test target and mention an editor target inside the same call.
    call = re.compile(r"target_link_libraries\s*\((.*?)\)", re.DOTALL)
    protected = re.compile(r"\b(raven|raven_core|raven_render|raven_sandbox|raven-sandbox|raven_tests|raven_bench)\b")
    for path in iter_files(ROOT):
        if path.name not in ("CMakeLists.txt",) and not path.name.endswith(".cmake"):
            continue
        try:
            text = path.read_text(encoding="utf-8", errors="ignore")
        except OSError:
            continue
        for match in call.finditer(text):
            body = match.group(1)
            head = body.strip().split(None, 1)[0] if body.strip() else ""
            if not protected.match(head):
                continue
            if FORBIDDEN_LINK.search(body):
                lineno = text[: match.start()].count("\n") + 1
                violations.append(f"{path.relative_to(ROOT)}:{lineno}: '{head}' links an editor target: {body.strip()[:120]}")


def check_engine_has_no_editor_reference():
    # Belt and braces: any occurrence of the editor's install symbol in engine sources.
    for path in iter_files(ENGINE_DIR):
        if path.suffix not in SOURCE_SUFFIXES:
            continue
        try:
            text = path.read_text(encoding="utf-8", errors="ignore")
        except OSError:
            continue
        for lineno, line in enumerate(text.splitlines(), start=1):
            stripped = line.strip()
            # Ignore comment lines (the hook header documents the editor contract in prose).
            if stripped.startswith(("//", "*", "/*")):
                continue
            if "raven_editor" in line or "RavenEditorHooks" in line:
                violations.append(f"{path.relative_to(ROOT)}:{lineno}: editor symbol referenced from engine: {line.strip()}")


def main():
    check_includes()
    check_link_graph()
    check_engine_has_no_editor_reference()
    if violations:
        print("layering violations:\n")
        for v in violations:
            print(f"  {v}")
        print(f"\nFAIL: {len(violations)} layering violation(s)")
        return 1
    print("layering OK: engine/sandbox reference no editor targets, headers or symbols")
    return 0


if __name__ == "__main__":
    sys.exit(main())
