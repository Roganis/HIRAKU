#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""tools/runpath.py: a packaged program looks for its libraries in the package only. Small programs
built here with a run path as a build leaves it ($ORIGIN/lib, then the folder it linked against).

    test_runpath.py CC      exit 0 when all hold (CC: a C compiler)
"""
import os
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
TOOL = [sys.executable, os.path.join(ROOT, "tools", "runpath.py")]
cc = sys.argv[1] if len(sys.argv) > 1 else "cc"
failed = 0


def check(ok: bool, what: str) -> None:
    global failed
    print(("ok    " if ok else "FAIL  ") + what, flush=True)
    failed += not ok


def run(*args, **kw):
    return subprocess.run(list(args), capture_output=True, text=True, **kw)


def run_path(file: str) -> str:
    """ "RUNPATH a:b", "RPATH a:b" or "" """
    for line in run("readelf", "-d", file).stdout.splitlines():
        for kind in ("RUNPATH", "RPATH"):
            if f"({kind})" in line:
                return kind + " " + line[line.index("[") + 1:line.rindex("]")]
    return ""


with tempfile.TemporaryDirectory() as tmp:
    pkg, deps = os.path.join(tmp, "package", "bin"), os.path.join(tmp, "build-deps", "lib")
    os.makedirs(os.path.join(pkg, "lib"))
    os.makedirs(deps)
    src = os.path.join(tmp, "src")
    os.makedirs(src)
    with open(os.path.join(src, "own.c"), "w") as f:
        f.write('const char *own(void) { return "the package\'s library"; }\n')
    with open(os.path.join(src, "sys.c"), "w") as f:
        f.write('const char *sys_lib(void) { return "a library from the build\'s folder"; }\n')
    with open(os.path.join(src, "main.c"), "w") as f:
        f.write('#include <stdio.h>\nconst char *own(void);\nint main(void) { puts(own()); return 0; }\n')
    with open(os.path.join(src, "main2.c"), "w") as f:
        f.write('#include <stdio.h>\nconst char *own(void);\nconst char *sys_lib(void);\n'
                'int main(void) { puts(own()); puts(sys_lib()); return 0; }\n')

    def build(out, sources, *flags):
        r = run(cc, "-o", out, *sources, *flags)
        if r.returncode:
            print(r.stderr)
            sys.exit(f"{cc} can't build the test's programs")

    # the package's own library, and one the package doesn't bring (as libz: the system's on a player's PC)
    build(os.path.join(pkg, "lib", "libown.so"), [os.path.join(src, "own.c")], "-shared", "-fPIC", f"-Wl,-rpath,{deps}", "-Wl,--enable-new-dtags")
    build(os.path.join(deps, "libsys.so"), [os.path.join(src, "sys.c")], "-shared", "-fPIC")
    link = [f"-L{os.path.join(pkg, 'lib')}", f"-L{deps}"]
    as_built = f"$ORIGIN/lib:{deps}"
    prog, prog2, old = os.path.join(pkg, "prog"), os.path.join(pkg, "prog2"), os.path.join(pkg, "prog-rpath")
    build(prog, [os.path.join(src, "main.c")], *link, "-lown", f"-Wl,-rpath,{as_built}", "-Wl,--enable-new-dtags")
    build(prog2, [os.path.join(src, "main2.c")], *link, "-lown", "-lsys", f"-Wl,-rpath,{as_built}", "-Wl,--enable-new-dtags")
    build(old, [os.path.join(src, "main.c")], *link, "-lown", f"-Wl,-rpath,{as_built}", "-Wl,--disable-new-dtags")
    check(run_path(prog) == "RUNPATH " + as_built and run_path(old) == "RPATH " + as_built, "as built: $ORIGIN/lib, then the build's folder")
    check("build's folder" in run(prog2).stdout, "... and a library the package doesn't bring is taken from the build's folder")

    r = run(*TOOL, "check", os.path.dirname(pkg))
    check(r.returncode == 1 and "prog2" in r.stderr and "libown.so" in r.stderr and deps in r.stderr, "check: says which files look outside the package, and where")

    mode = os.stat(prog).st_mode
    size = os.path.getsize(prog)
    r = run(*TOOL, "fix", prog, prog2, old)
    check(r.returncode == 0, "fix: done")
    check(run_path(prog) == "RUNPATH $ORIGIN/lib", "... the run path is $ORIGIN/lib alone")
    check(run_path(old) == "RPATH $ORIGIN/lib", "... an RPATH stays an RPATH")
    check(os.stat(prog).st_mode == mode and os.path.getsize(prog) == size, "... the file's size and permissions as they were")
    r = run(prog)
    check(r.returncode == 0 and "package's library" in r.stdout, "... the program runs, with the package's library")
    r = run(prog2)
    check(r.returncode != 0 and "libsys.so" in r.stderr, "... and no longer finds a library in the build's folder")

    lib = os.path.join(pkg, "lib", "libown.so")
    r = run(*TOOL, "fix", "--else", "$ORIGIN", lib)
    check(r.returncode == 0 and run_path(lib) == "RUNPATH $ORIGIN", "a library whose run path was the build's folder alone: its own folder (--else)")
    r = run(*TOOL, "check", os.path.dirname(pkg))
    check(r.returncode == 0, "check: nothing looks outside the package now")
    before = open(prog, "rb").read()
    r = run(*TOOL, "fix", prog, os.path.join(src, "main.c"), os.path.join(tmp, "not-there"))
    check(r.returncode == 0 and open(prog, "rb").read() == before, "fix again, and on files that aren't programs: nothing changes")

    # entries in other places: the middle, the front; the current folder
    for path, want in ((f"{deps}:$ORIGIN/lib:/opt/x", "$ORIGIN/lib"), (f"$ORIGIN/lib:{deps}:$ORIGIN", "$ORIGIN/lib:$ORIGIN"), ("$ORIGIN/lib::lib", "$ORIGIN/lib")):
        p = os.path.join(pkg, "prog3")
        build(p, [os.path.join(src, "main.c")], *link, "-lown", f"-Wl,-rpath,{path}", "-Wl,--enable-new-dtags")
        r = run(*TOOL, "fix", p)
        check(r.returncode == 0 and run_path(p) == "RUNPATH " + want and run(p).returncode == 0, f"{path.replace(deps, 'BUILD')}: {want} is left")
    if shutil.which("strip"):
        build(prog, [os.path.join(src, "main.c")], *link, "-lown", f"-Wl,-rpath,{as_built}", "-Wl,--enable-new-dtags")
        run("strip", prog)
        r = run(*TOOL, "fix", prog)
        check(r.returncode == 0 and run_path(prog) == "RUNPATH $ORIGIN/lib" and run(prog).returncode == 0, "a stripped program, as a package's are: the same")

print(f"{failed} failed" if failed else "all hold")
sys.exit(1 if failed else 0)
