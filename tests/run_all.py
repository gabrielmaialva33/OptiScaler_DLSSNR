#!/usr/bin/env python3
"""Run the repository's test suites and report one summary.

Every suite under tests/ stays a standalone `python3 tests/<name>/run.py`; this
driver only discovers them, decides which can run here, and collects results.

    python3 tests/run_all.py                 # host tier, the default
    python3 tests/run_all.py --list          # registry with tiers, run nothing
    python3 tests/run_all.py --tier all      # host and wine
    python3 tests/run_all.py --only nr-menu,nr-multipass
    python3 tests/run_all.py --skip nr-localization -v

Exit status is 0 when nothing failed. Skips are not failures: a suite whose
toolchain is absent is reported as skipped with the reason. A directory that is
not registered in suites.toml, or a registry entry with no directory, is a
failure, because that is how the suite list silently drifts out of date.
"""

from __future__ import annotations

import argparse
import concurrent.futures
import dataclasses
import os
import pathlib
import shutil
import subprocess
import sys
import time
import tomllib

ROOT = pathlib.Path(__file__).resolve().parents[1]
TESTS = ROOT / "tests"
MANIFEST = TESTS / "suites.toml"
TIERS = ("host", "wine", "wip")


@dataclasses.dataclass
class Result:
    name: str
    tier: str
    status: str  # PASS FAIL SKIP TIMEOUT
    seconds: float
    note: str = ""
    log: pathlib.Path | None = None

    @property
    def failed(self) -> bool:
        return self.status in ("FAIL", "TIMEOUT")


def load_manifest() -> dict[str, dict]:
    with MANIFEST.open("rb") as handle:
        manifest = tomllib.load(handle)
    for name, entry in manifest.items():
        tier = entry.get("tier")
        if tier not in TIERS:
            raise SystemExit(f"suites.toml: {name} has tier {tier!r}, expected one of {TIERS}")
    return manifest


def check_registry(manifest: dict[str, dict]) -> list[str]:
    """Registry and filesystem must agree. Returns human-readable problems."""
    on_disk = {p.name for p in TESTS.iterdir() if p.is_dir() and not p.name.startswith(("_", "."))}
    problems = []
    for missing in sorted(on_disk - manifest.keys()):
        problems.append(f"tests/{missing}/ exists but is not registered in tests/suites.toml")
    for stale in sorted(manifest.keys() - on_disk):
        problems.append(f"tests/suites.toml lists {stale} but tests/{stale}/ does not exist")
    for name, entry in sorted(manifest.items()):
        runner = TESTS / name / "run.py"
        if entry["tier"] == "wip":
            if runner.exists():
                problems.append(f"{name} is tier wip but tests/{name}/run.py exists; promote it")
        elif not runner.exists():
            problems.append(f"{name} is tier {entry['tier']} but has no tests/{name}/run.py")
    return problems


def tier_blocker(tier: str) -> str:
    """Why this tier cannot run here, or an empty string if it can."""
    if tier == "wip":
        return "no runner yet"
    if tier == "host":
        if not (shutil.which("g++") and shutil.which("clang++")):
            return "needs g++ and clang++ on PATH"
        return ""
    prefix = pathlib.Path(os.environ.get("WINEPREFIX", "~/.local/opt/msvc-wineprefix")).expanduser()
    msvc = pathlib.Path(os.environ.get("MSVC_BIN", "~/.local/opt/msvc/bin/x64")).expanduser()
    if not shutil.which("wine"):
        return "needs wine on PATH"
    if not prefix.is_dir():
        return f"no Wine prefix at {prefix}"
    if not msvc.is_dir():
        return f"no msvc-wine toolchain at {msvc}"
    return ""


def run_suite(name: str, entry: dict, log_dir: pathlib.Path, stream: bool) -> Result:
    tier = entry["tier"]
    blocker = tier_blocker(tier)
    if blocker:
        return Result(name, tier, "SKIP", 0.0, blocker)

    log_path = log_dir / f"{name}.log"
    started = time.monotonic()
    try:
        with log_path.open("wb") as log:
            completed = subprocess.run(
                [sys.executable, str(TESTS / name / "run.py")],
                cwd=ROOT,
                stdout=None if stream else log,
                stderr=subprocess.STDOUT if not stream else None,
                timeout=entry["timeout"],
            )
    except subprocess.TimeoutExpired:
        return Result(name, tier, "TIMEOUT", time.monotonic() - started,
                      f"exceeded {entry['timeout']}s", log_path)
    elapsed = time.monotonic() - started
    if completed.returncode == 0:
        return Result(name, tier, "PASS", elapsed, log=log_path)
    return Result(name, tier, "FAIL", elapsed, f"exit {completed.returncode}", log_path)


def selected(manifest: dict[str, dict], args) -> list[str]:
    names = sorted(manifest)
    if args.tier != "all":
        names = [n for n in names if manifest[n]["tier"] == args.tier]
    if args.only:
        wanted = {s.strip() for s in args.only.split(",") if s.strip()}
        unknown = wanted - manifest.keys()
        if unknown:
            raise SystemExit(f"--only names no such suite: {', '.join(sorted(unknown))}")
        names = [n for n in sorted(wanted)]
    if args.skip:
        dropped = {s.strip() for s in args.skip.split(",") if s.strip()}
        unknown = dropped - manifest.keys()
        if unknown:
            raise SystemExit(f"--skip names no such suite: {', '.join(sorted(unknown))}")
        names = [n for n in names if n not in dropped]
    return names


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--tier", choices=(*TIERS, "all"), default="host",
                        help="which tier to run (default: host)")
    parser.add_argument("--only", help="comma-separated suite names, overrides --tier")
    parser.add_argument("--skip", help="comma-separated suite names to leave out")
    parser.add_argument("--jobs", type=int, default=4,
                        help="parallel host suites (default: 4); the wine tier is always serial")
    parser.add_argument("--list", action="store_true", help="print the registry and exit")
    parser.add_argument("-v", "--verbose", action="store_true",
                        help="stream suite output instead of capturing it to a log")
    parser.add_argument("--log-dir", type=pathlib.Path,
                        help="where to write per-suite logs (default: a temp dir under $TMPDIR)")
    args = parser.parse_args()

    manifest = load_manifest()
    problems = check_registry(manifest)

    if args.list:
        for name in sorted(manifest, key=lambda n: (TIERS.index(manifest[n]["tier"]), n)):
            entry = manifest[name]
            blocker = tier_blocker(entry["tier"])
            state = "ready" if not blocker else f"unavailable: {blocker}"
            print(f"{name:<22} {entry['tier']:<5} {state}")
            print(f"{'':<22} {'':<5} {entry['summary']}")
        for problem in problems:
            print(f"registry: {problem}", file=sys.stderr)
        return 1 if problems else 0

    if problems:
        for problem in problems:
            print(f"registry: {problem}", file=sys.stderr)
        print("\nFix tests/suites.toml before running the suites.", file=sys.stderr)
        return 1

    names = selected(manifest, args)
    if not names:
        print("nothing selected")
        return 0

    log_dir = args.log_dir or pathlib.Path(
        os.environ.get("TMPDIR", "/tmp")) / f"optiscaler-tests-{os.getpid()}"
    log_dir.mkdir(parents=True, exist_ok=True)

    parallel = [n for n in names if manifest[n]["tier"] == "host"]
    serial = [n for n in names if manifest[n]["tier"] != "host"]
    results: list[Result] = []

    print(f"running {len(names)} suite(s); logs in {log_dir}\n")

    if parallel:
        workers = 1 if args.verbose else max(1, min(args.jobs, len(parallel)))
        with concurrent.futures.ThreadPoolExecutor(max_workers=workers) as pool:
            futures = {pool.submit(run_suite, n, manifest[n], log_dir, args.verbose): n
                       for n in parallel}
            for future in concurrent.futures.as_completed(futures):
                result = future.result()
                results.append(result)
                print(f"  {result.status:<7} {result.name}")
    for name in serial:
        result = run_suite(name, manifest[name], log_dir, args.verbose)
        results.append(result)
        print(f"  {result.status:<7} {result.name}")

    results.sort(key=lambda r: (TIERS.index(r.tier), r.name))
    width = max(len(r.name) for r in results)
    print("\n" + "-" * (width + 30))
    for result in results:
        note = f"  {result.note}" if result.note else ""
        print(f"{result.status:<7} {result.name:<{width}}  {result.seconds:6.1f}s{note}")
    print("-" * (width + 30))

    counts = {status: sum(1 for r in results if r.status == status)
              for status in ("PASS", "FAIL", "TIMEOUT", "SKIP")}
    print(" ".join(f"{status.lower()}={count}" for status, count in counts.items() if count))

    failures = [r for r in results if r.failed]
    if failures:
        print("\nfailing suites:")
        for result in failures:
            print(f"  {result.name}: {result.note}"
                  + (f" (log: {result.log})" if result.log and not args.verbose else ""))
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
