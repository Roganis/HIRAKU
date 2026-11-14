#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""tools/lint.py against files made up here: what it must find, what it must leave alone; then
over the repository's own Python files, which must be clean."""
import os
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import lint  # noqa: E402

failed = 0


def check(ok, what):
    global failed
    print(("ok    " if ok else "FAIL  ") + what)
    failed += not ok


with tempfile.TemporaryDirectory() as d:
    def findings(source):
        path = os.path.join(d, "sample.py")
        with open(path, "w", encoding="utf-8") as f:
            f.write(source)
        return lint.check(path)

    clean = (
        "import os, sys as system\n"
        "from collections import OrderedDict as Ordered\n"
        "TOTAL = 0\n"
        "def add(a, *rest, key=None, **more):\n"
        "    global TOTAL\n"
        "    TOTAL += a + len(rest) + len(more)\n"
        "    squares = [n * n for n in rest if (m := n) and m]\n"
        "    try:\n"
        "        return os.sep, system.argv, Ordered(), squares, key\n"
        "    except OSError as e:\n"
        "        return str(e)\n"
        "class Thing:\n"
        "    def name(self):\n"
        "        return __class__.__name__, __file__\n"
        "with open(__file__) as handle:\n"
        "    for number, line in enumerate(handle):\n"
        "        print(number, line, add(1), Thing().name())\n"
    )
    check(findings(clean) == [], "a file that is fine: nothing found")
    f = findings("def report(count):\n    return 'found %d' % cuont\n")
    check(len(f) == 1 and ":2:" in f[0] and "'cuont'" in f[0], "a misspelt name: found, with its line")
    f = findings("def late():\n    return json.dumps({})\n")
    check(len(f) == 1 and "'json'" in f[0], "a module used and never imported: found")
    check(findings("from os.path import *\nprint(join('a', 'b'), nothing_of_the_kind)\n") == [], "with a star import a name may come from it: not judged")
    f = findings("def broken(:\n    pass\n")
    check(len(f) == 1 and ":1:" in f[0], "a file that doesn't compile: found")
    check(findings("X = 'a\\\\d'\n") == [], "a doubled backslash: fine")
    f = findings("X = 'a\\d'\n")
    check(len(f) == 1, "a backslash that is no escape: found (a warning counted as an error)")
    check(len(lint.check(os.path.join(d, "not-there.py"))) == 1, "a file that isn't there: said")

own = lint.default_files()
found = [line for path in own for line in lint.check(path)]
for line in found:
    print(line)
check(len(own) > 30 and not found, f"the repository's own {len(own)} Python files: clean")
print("%d failed" % failed if failed else "all hold")
sys.exit(1 if failed else 0)
