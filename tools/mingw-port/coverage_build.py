"""Build, test and measure line coverage in one step.

The workspace path contains non-ASCII characters ("翠花工具包") and both the
gcov runtime and gcov itself use narrow file APIs on Windows: .gcda files
are never written and sources cannot be read back. So the coverage run
mirrors the source tree to an ASCII path, builds there with --coverage,
runs ctest and reports through coverage.py.

Usage:
    python tools/mingw-port/coverage_build.py [--jobs N] [--logic-only] [--skip-tests]

Exit code is non-zero when a test fails (coverage is still reported).
"""

import os
import shutil
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
MIRROR = r"C:\Users\sch\tinta-cov\src"
BUILD = r"C:\Users\sch\tinta-cov\build"
TOOLCHAIN = r"D:\Qt\Tools\mingw1310_64"
CMAKE = r"D:\Qt\Tools\CMake_64\bin\cmake.exe"
CTEST = r"D:\Qt\Tools\CMake_64\bin\ctest.exe"
NINJA = r"D:\Qt\Tools\Ninja\ninja.exe"

SKIP_DIRS = {"build", "build-test", ".git", ".workbuddy", "third_party/md4c/.git"}


def log(message):
    print(message, flush=True)


def mirror():
    # No rmtree here: the mirror is refreshed in place so the run does not
    # trip bulk-delete guards. Obsolete files are harmless because CMake
    # only compiles what the current CMakeLists.txt lists.
    os.makedirs(MIRROR, exist_ok=True)
    copied = 0
    for dirpath, dirnames, filenames in os.walk(ROOT):
        rel = os.path.relpath(dirpath, ROOT)
        parts = [] if rel == "." else rel.split(os.sep)
        if parts and parts[0] in SKIP_DIRS:
            dirnames[:] = []
            continue
        if any(p in SKIP_DIRS for p in parts):
            continue
        target = os.path.join(MIRROR, *parts) if parts else MIRROR
        os.makedirs(target, exist_ok=True)
        # Skip generated artefacts and diagnostics that only exist for the
        # MinGW port
        for name in filenames:
            if name.endswith((".png", ".gcda", ".gcno", ".obj", ".exe")):
                continue
            src = os.path.join(dirpath, name)
            dst = os.path.join(target, name)
            try:
                shutil.copy2(src, dst)
                copied += 1
            except OSError:
                pass
    log(f"mirrored {copied} files to {MIRROR}")


def run(cmd, cwd=None, env=None, capture=False):
    return subprocess.run(cmd, cwd=cwd, env=env, text=True,
                          capture_output=capture, errors="replace")


def configure(jobs):
    env = dict(os.environ)
    env["PATH"] = TOOLCHAIN + r"\bin;" + env.get("PATH", "")
    # A stale cache keeps the previous compiler paths and the previous
    # coverage flags, which produces silently wrong numbers. Dropping just
    # the cache forces a full reconfigure (and therefore a rebuild with the
    # new command lines) without deleting the object tree.
    if "--reuse" not in sys.argv:
        for name in ("CMakeCache.txt",):
            cache = os.path.join(BUILD, name)
            if os.path.isfile(cache):
                os.remove(cache)
    args = [
        CMAKE, "-S", MIRROR, "-B", BUILD, "-G", "Ninja",
        "-DCMAKE_BUILD_TYPE=Debug",
        f"-DCMAKE_C_COMPILER={TOOLCHAIN}/bin/gcc.exe",
        f"-DCMAKE_CXX_COMPILER={TOOLCHAIN}/bin/g++.exe",
        f"-DCMAKE_MAKE_PROGRAM={NINJA}",
        "-DCMAKE_RC_COMPILER=D:/Qt/Tools/mingw1310_64/bin/windres.exe",
        "-DBUILD_TESTING=ON",
        f"-DFETCHCONTENT_SOURCE_DIR_MD4C={MIRROR}/third_party/md4c",
        "-DCMAKE_CXX_FLAGS=--coverage -O0 -g",
        "-DCMAKE_C_FLAGS=--coverage -O0 -g",
        "-DCMAKE_EXE_LINKER_FLAGS=--coverage",
    ]
    result = run(args, env=env)
    if result.returncode:
        log("configure failed")
        sys.exit(result.returncode)


def build(jobs, target=None):
    env = dict(os.environ)
    env["PATH"] = TOOLCHAIN + r"\bin;" + env.get("PATH", "")
    args = [NINJA, "-C", BUILD, "-k", "0", "-j", str(jobs)]
    if target:
        args.append(target)
    result = run(args, env=env, capture=True)
    if result.returncode:
        errors = [l for l in result.stdout.splitlines() if "error:" in l or "FAILED" in l]
        log("build failed:")
        for line in errors[:10]:
            log("  " + line)
        sys.exit(result.returncode)
    log("build ok")


def runTarget(target):
    """Fast loop: run one test executable instead of the whole ctest suite."""
    exe = os.path.join(BUILD, target + ".exe")
    env = dict(os.environ)
    env["PATH"] = TOOLCHAIN + r"\bin;" + env.get("PATH", "")
    started = time.time()
    result = run([exe], cwd=BUILD, env=env, capture=True)
    log(f"{target} ({time.time() - started:.0f}s) exit={result.returncode}")
    for line in (result.stdout + result.stderr).splitlines():
        if "FAIL" in line or "error" in line.lower():
            log("  " + line)
    return result.returncode


def test():
    env = dict(os.environ)
    env["PATH"] = TOOLCHAIN + r"\bin;" + env.get("PATH", "")
    started = time.time()
    # -j 2, not more: several tests copy the 120 MB test binary into their own
    # directory, and four at once makes the slower ones time out.
    result = run([CTEST, "-j", "2", "--output-on-failure"], cwd=BUILD, env=env,
                 capture=True)
    tail = [l for l in result.stdout.splitlines()
            if "tests passed" in l or "tests failed" in l]
    log(f"ctest ({time.time() - started:.0f}s): " + (tail[0] if tail else "?"))
    if result.returncode:
        for line in result.stdout.splitlines():
            if line.strip().startswith(("FAIL", "FAILED", "  ->")) or "Failed" in line:
                log("  " + line.strip())
    return result.returncode


def report(logic_only):
    args = [sys.executable, os.path.join(ROOT, "tools", "mingw-port", "coverage.py"),
            BUILD, f"--root={MIRROR}"]
    if logic_only:
        args.append("--logic-only")
    run(args, cwd=ROOT)


def main():
    jobs = 6
    target = None
    for i, arg in enumerate(sys.argv):
        if arg == "--jobs" and i + 1 < len(sys.argv):
            jobs = int(sys.argv[i + 1])
        if arg == "--target" and i + 1 < len(sys.argv):
            target = sys.argv[i + 1]
    mirror()
    configure(jobs)
    build(jobs, target)
    failed = 0
    if target:
        failed = runTarget(target)
    elif "--skip-tests" not in sys.argv:
        failed = test()
    report("--logic-only" in sys.argv)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
