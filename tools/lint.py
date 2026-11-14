#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The Python tools read over without being run (the unit tests' `tools_lint`, and so every push).

A Python file's mistakes show only when the line they are on runs, and a tool's rare paths (an
error's message, an update undone, an odd dump) run first in a player's hands. Checked here, with
Python's own modules only:

  - each file compiles, warnings counted as errors (a backslash that isn't an escape, `is` with a
    number or a text);
  - every name a file reads is one it defines, imports or Python has (a misspelt variable or
    function, an import forgotten).

    tools/lint.py [FILE...]      the files given, else tools/*.py and the tests' own Python files
"""
import ast
import builtins
import glob
import os
import sys
import warnings

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
# names a module has without saying so
MODULE_NAMES = {"__file__", "__name__", "__doc__", "__builtins__", "__spec__", "__loader__", "__package__", "__path__", "__debug__",
                "__class__", "__annotations__", "__dict__", "__qualname__", "__module__"}


def defined_names(tree):
    """every name the file gives a meaning to, in any place of it"""
    names = set(dir(builtins)) | MODULE_NAMES
    star = False
    for node in ast.walk(tree):
        if isinstance(node, ast.Name) and isinstance(node.ctx, (ast.Store, ast.Del)):
            names.add(node.id)
        elif isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef, ast.ClassDef)):
            names.add(node.name)
        elif isinstance(node, ast.arg):
            names.add(node.arg)
        elif isinstance(node, (ast.Import, ast.ImportFrom)):
            for a in node.names:
                if a.name == "*":
                    star = True
                names.add((a.asname or a.name).split(".")[0])
        elif isinstance(node, ast.ExceptHandler) and node.name:
            names.add(node.name)
        elif isinstance(node, (ast.Global, ast.Nonlocal)):
            names.update(node.names)
        elif isinstance(node, (ast.MatchAs, ast.MatchStar)) and node.name:
            names.add(node.name)
        elif isinstance(node, ast.MatchMapping) and node.rest:
            names.add(node.rest)
        elif hasattr(ast, "TypeVar") and isinstance(node, (ast.TypeVar, ast.ParamSpec, ast.TypeVarTuple)):
            names.add(node.name)
    return names, star


def check(path):
    """the file's findings, as "path:line: what" """
    found = []
    shown = os.path.relpath(path, ROOT) if os.path.abspath(path).startswith(ROOT + os.sep) else path
    try:
        with open(path, encoding="utf-8") as f:
            source = f.read()
    except (OSError, UnicodeDecodeError) as e:
        return [f"{shown}: cannot be read ({e})"]
    with warnings.catch_warnings():
        warnings.simplefilter("error")
        try:
            tree = ast.parse(source, filename=shown)
            compile(source, shown, "exec")
        except SyntaxError as e:
            return [f"{shown}:{e.lineno or 0}: {e.msg}"]
        except Warning as e:   # (a SyntaxWarning made an error)
            return [f"{shown}:{getattr(e, 'lineno', 0) or 0}: {e}"]
    names, star = defined_names(tree)
    if not star:   # (with `from x import *` a name may come from there)
        seen = set()
        for node in ast.walk(tree):
            if isinstance(node, ast.Name) and isinstance(node.ctx, ast.Load) and node.id not in names and (node.id, node.lineno) not in seen:
                seen.add((node.id, node.lineno))
                found.append(f"{shown}:{node.lineno}: the name '{node.id}' is read but never defined or imported")
    return sorted(found, key=lambda s: (s.split(":")[0], int(s.split(":")[1]) if s.split(":")[1].isdigit() else 0))


def default_files():
    files = sorted(glob.glob(os.path.join(ROOT, "tools", "*.py")))
    files += sorted(glob.glob(os.path.join(ROOT, "tests", "**", "*.py"), recursive=True))
    files += sorted(glob.glob(os.path.join(ROOT, "oracle", "*.py")))
    return files


def main(argv):
    files = argv[1:] or default_files()
    found = []
    for f in files:
        found += check(f)
    for line in found:
        print(line)
    print(f"lint: {len(files)} Python files, {len(found)} finding{'s' if len(found) != 1 else ''}")
    return 1 if found else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
