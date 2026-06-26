#!/usr/bin/env python3
import argparse
import json
import os
import shutil
import statistics
import subprocess
import sys
import tarfile
import tempfile
import time
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
PERF = ROOT / "tests" / "perf"


CASES = [
    {
        "name": "startup",
        "summary": "process startup + reader/expander/compiler minimum",
        "files": {
            "idiom": "startup.id",
            "python": "startup.py",
            "ruby": "startup.rb",
            "bash": "startup.sh",
            "elixir": "startup.exs",
            "elisp": "startup.el",
        },
    },
    {
        "name": "arith_tail",
        "summary": "low-level integer arithmetic in a hot tail loop",
        "default": False,
        "files": {
            "idiom": "arith_tail.id",
            "python": "arith_tail.py",
            "ruby": "arith_tail.rb",
            "bash": "arith_tail.sh",
            "elixir": "arith_tail.exs",
            "elisp": "arith_tail.el",
        },
    },
    {
        "name": "arith_idiomatic",
        "summary": "integer arithmetic through each language's reduce/range idiom",
        "base_ref_default": False,
        "base_ref_skip": "requires current std/range",
        "files": {
            "idiom": "arith_idiomatic.id",
            "python": "arith_idiomatic.py",
            "ruby": "arith_idiomatic.rb",
            "bash": "arith_idiomatic.sh",
            "elixir": "arith_idiomatic.exs",
            "elisp": "arith_idiomatic.el",
        },
    },
    {
        "name": "list_sum",
        "summary": "low-level list allocation and reduction",
        "default": False,
        "files": {
            "idiom": "list_sum.id",
            "python": "list_sum.py",
            "ruby": "list_sum.rb",
            "bash": "list_sum.sh",
            "elixir": "list_sum.exs",
            "elisp": "list_sum.el",
        },
    },
    {
        "name": "list_sum_idiomatic",
        "summary": "build and reduce a sequence through each language's collection idiom",
        "base_ref_default": False,
        "base_ref_skip": "requires current std/range and std/list",
        "files": {
            "idiom": "list_sum_idiomatic.id",
            "python": "list_sum_idiomatic.py",
            "ruby": "list_sum_idiomatic.rb",
            "bash": "list_sum_idiomatic.sh",
            "elixir": "list_sum_idiomatic.exs",
            "elisp": "list_sum_idiomatic.el",
        },
    },
    {
        "name": "range_size",
        "summary": "constant-time Range size/last/count over large integer spans",
        "base_ref_default": False,
        "base_ref_skip": "requires current std/range O(1) sizing contract",
        "files": {
            "idiom": "range_size.id",
        },
    },
    {
        "name": "pattern_matrix",
        "summary": "64-clause tuple pattern matrix dispatch",
        "base_ref_default": False,
        "base_ref_skip": "requires current syntax-case/pattern surface",
        "files": {
            "idiom": "pattern_matrix.id",
            "python": "pattern_matrix.py",
            "ruby": "pattern_matrix.rb",
            "bash": "pattern_matrix.sh",
            "elixir": "pattern_matrix.exs",
            "elisp": "pattern_matrix.el",
        },
    },
    {
        "name": "regex_scan",
        "summary": "compiled regex boolean match in a hot loop",
        "base_ref_default": False,
        "base_ref_skip": "requires current regex package/surface",
        "files": {
            "idiom": "regex_scan.id",
            "python": "regex_scan.py",
            "ruby": "regex_scan.rb",
            "bash": "regex_scan.sh",
            "elixir": "regex_scan.exs",
            "elisp": "regex_scan.el",
        },
    },
    {
        "name": "regex_capture",
        "summary": "compiled regex result/capture in a hot loop",
        "base_ref_default": False,
        "base_ref_skip": "requires current regex package/surface",
        "files": {
            "idiom": "regex_capture.id",
            "python": "regex_capture.py",
            "ruby": "regex_capture.rb",
            "bash": "regex_capture.sh",
            "elixir": "regex_capture.exs",
            "elisp": "regex_capture.el",
        },
    },
    {
        "name": "actor_spawn",
        "summary": "spawn one lightweight process and receive one message per iteration",
        "base_ref_default": False,
        "base_ref_skip": "requires current actor/runtime surface",
        "files": {
            "idiom": "actor_spawn.id",
            "elixir": "actor_spawn.exs",
        },
    },
    {
        "name": "editor_keys",
        "summary": "layered editor keymap lookup and command dispatch",
        "base_ref_default": False,
        "base_ref_skip": "editor substrate benchmark; requires current std/runtime surface",
        "files": {
            "idiom": "editor_keys.id",
            "elisp": "editor_keys.el",
        },
    },
    {
        "name": "editor_line",
        "summary": "line editor state updates over a bounded input trace",
        "base_ref_default": False,
        "base_ref_skip": "requires current app/ish editor package",
        "files": {
            "idiom": "editor_line.id",
            "elisp": "editor_line.el",
        },
    },
    {
        "name": "editor_buffer",
        "summary": "point-oriented text buffer edits over a realistic mixed trace",
        "base_ref_default": False,
        "base_ref_skip": "editor substrate benchmark; requires current runtime surface",
        "files": {
            "idiom": "editor_buffer.id",
            "elisp": "editor_buffer.el",
        },
    },
    {
        "name": "editor_markers",
        "summary": "marker/span adjustment across insert and delete edits",
        "base_ref_default": False,
        "base_ref_skip": "editor substrate benchmark; requires current runtime surface",
        "files": {
            "idiom": "editor_markers.id",
            "elisp": "editor_markers.el",
        },
    },
    {
        "name": "editor_syntax",
        "summary": "regex-driven syntax span extraction over source-like text",
        "base_ref_default": False,
        "base_ref_skip": "requires current std/regex package",
        "files": {
            "idiom": "editor_syntax.id",
            "elisp": "editor_syntax.el",
        },
    },
    {
        "name": "editor_render",
        "summary": "window state updates plus visible-region render checksum",
        "base_ref_default": False,
        "base_ref_skip": "editor substrate benchmark; requires current runtime surface",
        "files": {
            "idiom": "editor_render.id",
            "elisp": "editor_render.el",
        },
    },
]


EXTERNAL_RUNTIME_COMMANDS = {
    "python": ("python3",),
    "ruby": ("ruby", "--disable-gems"),
    "bash": ("bash",),
    "elixir": ("elixir",),
    "elisp": ("emacs", "--quick", "--batch", "--script"),
}

IDIOM_MODES = ("source", "build", "sealed")
CACHE_MODES = ("hot", "cold", "off")
CACHE_ALIASES = {
    "hit": "hot",
    "fill": "cold",
    "nocache": "off",
    "no-cache": "off",
}


def fail(message):
    print(f"perf: {message}", file=sys.stderr)
    raise SystemExit(1)


def child_env(extra=None):
    env = {**os.environ, "LC_ALL": "C", "LANG": "C"}
    if not env.get("IDIOMROOT"):
        env["IDIOMROOT"] = str(ROOT / "std")
    path = str(ROOT)
    env["IDIOMPATH"] = path if not env.get("IDIOMPATH") else f"{path}:{env['IDIOMPATH']}"
    if extra:
        for key, value in extra.items():
            if value is None:
                env.pop(key, None)
            else:
                env[key] = value
    return env


def run_checked(cmd, cwd=None, timeout=None, capture=True, extra_env=None):
    kwargs = {
        "cwd": cwd,
        "timeout": timeout,
        "text": True,
        "env": child_env(extra_env),
    }
    if capture:
        kwargs.update({"stdout": subprocess.PIPE, "stderr": subprocess.PIPE})
    proc = subprocess.run(cmd, **kwargs)
    if proc.returncode != 0:
        out = getattr(proc, "stdout", "") or ""
        err = getattr(proc, "stderr", "") or ""
        fail(f"command failed ({proc.returncode}): {' '.join(map(str, cmd))}\nstdout:\n{out}\nstderr:\n{err}")
    return proc


def safe_extract(tar_path, dst):
    root = dst.resolve()
    with tarfile.open(tar_path) as tf:
        for member in tf.getmembers():
            target = (dst / member.name).resolve()
            if root != target and root not in target.parents:
                fail(f"refusing unsafe archive path: {member.name}")
        if sys.version_info >= (3, 12):
            tf.extractall(dst, filter="data")
        else:
            tf.extractall(dst)


def build_base_ref(ref):
    tmp = tempfile.TemporaryDirectory(prefix="idiom-perf-base-")
    tmp_path = Path(tmp.name)
    tar_path = tmp_path / "base.tar"
    src_path = tmp_path / "src"
    src_path.mkdir()
    print(f"building baseline {ref} in {src_path}", file=sys.stderr)
    run_checked(["git", "archive", "--format=tar", "--output", str(tar_path), ref], cwd=ROOT)
    safe_extract(tar_path, src_path)
    base_ldflags = os.environ.get("LDFLAGS", "-lpthread -lm")
    run_checked(["make", "release", f"LDFLAGS={base_ldflags} -flto=auto"], cwd=src_path, capture=False)
    bin_path = src_path / "build" / "release" / "idiomc"
    if not bin_path.exists():
        fail(f"baseline build did not produce {bin_path}")
    return tmp, bin_path, src_path


def runtime_file(kind, filename):
    return PERF / kind / filename


def parse_csv(value, default, allowed, name, aliases=None):
    raw = default if value is None or value == "" else value
    items = []
    for part in raw.split(","):
        item = part.strip()
        if not item:
            continue
        if aliases:
            item = aliases.get(item, item)
        items.append(item)
    unknown = sorted(set(items).difference(allowed))
    if unknown:
        fail(f"unknown {name}: {', '.join(unknown)}")
    return items


def parse_runtime_targets(value):
    allowed = {"idiom", "base", *EXTERNAL_RUNTIME_COMMANDS.keys()}
    return parse_csv(value, "", allowed, "runtime(s)") if value else None


def runtime_key(runtime):
    if runtime["kind"] != "idiom":
        return runtime["target"]
    return f"{runtime['target']}:{runtime['mode']}:{runtime['cache']}"


def runtime_dir(runtime):
    if runtime["kind"] != "idiom":
        return runtime["target"]
    return f"{runtime['target']}/{runtime['mode']}-{runtime['cache']}"


def add_idiom_runtimes(runtimes, target, binary, cwd, modes, caches, artifact_root):
    for mode in modes:
        for cache in caches:
            if mode == "sealed" and cache == "cold":
                continue
            info = {
                "kind": "idiom",
                "target": target,
                "cmd": [str(binary)],
                "cwd": cwd,
                "available": True,
                "mode": mode,
                "cache": cache,
                "env": {"IDIOMROOT": str(Path(cwd) / "std")},
            }
            if cache == "off":
                info["env"]["IDIOMCACHE"] = "0"
            if mode in ("build", "sealed"):
                info["artifact_dir"] = artifact_root / target / f"{mode}-{cache}"
            if mode == "sealed":
                info["sealed_cache"] = {}
            runtimes[runtime_key(info)] = info


def wants(targets, name):
    return targets is None or name in targets


def discover_runtimes(args):
    runtimes = {}
    targets = args.runtime_targets
    if args.idiom_current and wants(targets, "idiom"):
        current = Path(args.idiom_current)
        if not current.exists():
            fail(f"current idiom binary does not exist: {current}")
        add_idiom_runtimes(runtimes, "idiom", current, ROOT, args.modes, args.cache, args.artifact_dir)
    if args.idiom_base and wants(targets, "base"):
        base = Path(args.idiom_base)
        if not base.exists():
            fail(f"base idiom binary does not exist: {base}")
        base_cwd = Path(args.idiom_base_cwd) if args.idiom_base_cwd else ROOT
        add_idiom_runtimes(runtimes, "base", base, base_cwd, args.modes, args.cache, args.artifact_dir)
    elif targets and "base" in targets:
        fail("runtime 'base' requires --base-ref or --idiom-base")
    if not args.no_external:
        for name, command in EXTERNAL_RUNTIME_COMMANDS.items():
            if not wants(targets, name):
                continue
            exe = shutil.which(command[0])
            info = {"kind": name, "target": name, "mode": "run", "cache": None, "cwd": ROOT}
            if exe:
                info.update({"cmd": [exe, *command[1:]], "available": True})
            else:
                info.update({"cmd": list(command), "available": False})
            runtimes[runtime_key(info)] = info
    return runtimes


def selected_cases(args):
    if not args.cases:
        selected = [case for case in CASES if case.get("default", True)]
        if args.base_ref:
            skipped = [case for case in selected if not case.get("base_ref_default", True)]
            selected = [case for case in selected if case.get("base_ref_default", True)]
            if skipped:
                names = ", ".join(f"{case['name']} ({case.get('base_ref_skip', 'not baseline-compatible')})" for case in skipped)
                print(f"perf: skipping baseline-incompatible default case(s): {names}", file=sys.stderr)
        return selected
    wanted = set(part.strip() for part in args.cases.split(",") if part.strip())
    selected = [case for case in CASES if case["name"] in wanted]
    found = {case["name"] for case in selected}
    missing = wanted.difference(found)
    if missing:
        fail(f"unknown benchmark case(s): {', '.join(sorted(missing))}")
    return selected


def command_for(runtime, case):
    kind = runtime["kind"]
    filename = case["files"].get(kind)
    if not filename:
        return None
    path = runtime_file(kind, filename)
    if not path.exists():
        fail(f"benchmark file does not exist: {path}")
    if runtime.get("mode") == "build":
        artifact_dir = runtime["artifact_dir"]
        artifact_dir.mkdir(parents=True, exist_ok=True)
        return [*runtime["cmd"], "build", str(path), "-o", str(artifact_dir / case["name"])]
    if runtime.get("mode") == "sealed":
        cache = runtime["sealed_cache"]
        if case["name"] not in cache:
            artifact_dir = runtime["artifact_dir"]
            artifact_dir.mkdir(parents=True, exist_ok=True)
            artifact = artifact_dir / case["name"]
            run_checked([*runtime["cmd"], "build", str(path), "-o", str(artifact)], cwd=runtime.get("cwd"), extra_env=runtime.get("env"))
            cache[case["name"]] = artifact
        return [*runtime["cmd"], str(cache[case["name"]])]
    return [*runtime["cmd"], str(path)]


def purge_ic(root):
    for path in Path(root).rglob("*.ic"):
        try:
            path.unlink()
        except FileNotFoundError:
            pass


def timed_run(cmd, timeout, cwd=None, extra_env=None, before=None):
    if before:
        before()
    start = time.perf_counter()
    proc = run_checked(cmd, cwd=cwd, timeout=timeout, extra_env=extra_env)
    elapsed = time.perf_counter() - start
    return elapsed, proc.stdout.strip()


def measure(cmd, cwd, runs, warmups, timeout, expected, runtime):
    before = None
    if runtime.get("cache") == "cold":
        before = lambda: purge_ic(runtime.get("cwd") or ROOT)
    extra_env = runtime.get("env")
    for _ in range(warmups):
        _, output = timed_run(cmd, timeout, cwd, extra_env, before)
        if expected is not None and output != expected:
            fail(f"unexpected warmup output for {' '.join(cmd)}: got {output!r}, expected {expected!r}")
    samples = []
    for _ in range(runs):
        elapsed, output = timed_run(cmd, timeout, cwd, extra_env, before)
        if expected is not None and output != expected:
            fail(f"unexpected output for {' '.join(cmd)}: got {output!r}, expected {expected!r}")
        samples.append(elapsed)
    return {
        "samples_ms": [s * 1000.0 for s in samples],
        "median_ms": statistics.median(samples) * 1000.0,
        "min_ms": min(samples) * 1000.0,
        "mean_ms": statistics.mean(samples) * 1000.0,
        "stdev_ms": (statistics.stdev(samples) * 1000.0) if len(samples) > 1 else 0.0,
    }


def expected_for_case(case, runtimes, timeout):
    for runtime in runtimes.values():
        if not runtime.get("available"):
            continue
        if runtime.get("mode") == "build":
            continue
        cmd = command_for(runtime, case)
        if not cmd:
            continue
        _, output = timed_run(cmd, timeout, runtime.get("cwd"), runtime.get("env"))
        return output
    for runtime in runtimes.values():
        if runtime.get("available") and runtime.get("mode") == "build":
            return ""
    fail(f"no available runtime can establish expected output for {case['name']}")


def emit_idiom_dumps(cases, runtimes, dump_dir, timeout):
    if not dump_dir:
        return
    dump_dir.mkdir(parents=True, exist_ok=True)
    for runtime in runtimes.values():
        if runtime["kind"] != "idiom" or not runtime.get("available") or runtime.get("mode") != "source":
            continue
        label_dir = dump_dir / runtime_dir(runtime)
        label_dir.mkdir(parents=True, exist_ok=True)
        for case in cases:
            filename = case["files"].get("idiom")
            if not filename:
                continue
            path = runtime_file("idiom", filename)
            core = run_checked([*runtime["cmd"], "dump", "core", str(path)], cwd=runtime.get("cwd"), timeout=timeout)
            bytecode = run_checked([*runtime["cmd"], "dump", "bytecode", str(path)], cwd=runtime.get("cwd"), timeout=timeout)
            (label_dir / f"{case['name']}.core").write_text(core.stdout)
            (label_dir / f"{case['name']}.bytecode").write_text(bytecode.stdout)


def run_callgrind(case, runtime, expected, out_dir, timeout):
    if not out_dir or runtime["kind"] != "idiom" or runtime.get("mode") != "source" or runtime.get("cache") != "hot" or not runtime.get("available"):
        return None
    valgrind = shutil.which("valgrind")
    if not valgrind:
        fail("--callgrind-dir requested but valgrind is not available")
    cmd = command_for(runtime, case)
    if not cmd:
        return None
    label_dir = out_dir / runtime_dir(runtime)
    label_dir.mkdir(parents=True, exist_ok=True)
    out_file = label_dir / f"{case['name']}.callgrind"
    proc = run_checked(
        [valgrind, "--tool=callgrind", f"--callgrind-out-file={out_file}", *cmd],
        cwd=runtime.get("cwd"),
        timeout=timeout,
    )
    output = proc.stdout.strip()
    if output != expected:
        fail(f"unexpected callgrind output for {case['name']} {runtime_key(runtime)}: got {output!r}, expected {expected!r}")
    return str(out_file)


def runtime_display(runtime):
    return {
        "runtime": runtime["target"],
        "mode": runtime.get("mode") or "-",
        "cache": runtime.get("cache") or "-",
    }


def find_measurement(case_result, runtime, mode=None, cache=None):
    for measurement in case_result["measurements"]:
        if measurement["runtime"] != runtime:
            continue
        if mode is not None and measurement["mode"] != mode:
            continue
        if cache is not None and measurement["cache"] != cache:
            continue
        return measurement
    return None


def print_table(results, runtimes):
    rows = [runtime_display(info) for info in runtimes.values() if info.get("available")]
    skipped = [runtime_display(info)["runtime"] for info in runtimes.values() if not info.get("available")]
    runtime_width = max(7, *(len(row["runtime"]) for row in rows), len("runtime"), len("base/idiom"))
    mode_width = max(4, *(len(row["mode"]) for row in rows), len("mode"))
    cache_width = max(5, *(len(row["cache"]) for row in rows), len("cache"))
    has_base = any(find_measurement(case_result, "base") for case_result in results)
    if skipped:
        print("skipped missing runtimes: " + ", ".join(skipped))
    print("")
    if has_base:
        header = (
            f"{'case':<18} {'runtime':<{runtime_width}} {'mode':<{mode_width}} {'cache':<{cache_width}} "
            f"{'median ms':>10} {'min ms':>10} {'mean ms':>10} {'x idiom':>8} {'x base':>8}"
        )
    else:
        header = (
            f"{'case':<18} {'runtime':<{runtime_width}} {'mode':<{mode_width}} {'cache':<{cache_width}} "
            f"{'median ms':>10} {'min ms':>10} {'mean ms':>10} {'x idiom':>8}"
        )
    print(header)
    print("-" * len(header))
    for case_result in results:
        idiom = find_measurement(case_result, "idiom")
        base = find_measurement(case_result, "base")
        idiom_median = idiom.get("median_ms") if idiom else None
        base_median = base.get("median_ms") if base else None
        first = True
        for stats in case_result["measurements"]:
            x_idiom = stats["median_ms"] / idiom_median if idiom_median else None
            x_base = stats["median_ms"] / base_median if base_median else None
            case_name = case_result["name"] if first else ""
            idiom_text = f"{x_idiom:.2f}" if x_idiom is not None else "-"
            base_text = f"{x_base:.2f}" if x_base is not None else "-"
            row = (
                f"{case_name:<18} {stats['runtime']:<{runtime_width}} {stats['mode']:<{mode_width}} {stats['cache']:<{cache_width}} "
                f"{stats['median_ms']:>10.3f} {stats['min_ms']:>10.3f} {stats['mean_ms']:>10.3f} "
                f"{idiom_text:>8}"
            )
            if has_base:
                row = f"{row} {base_text:>8}"
            print(row)
            first = False
        if idiom_median and base_median:
            speedup = base_median / idiom_median
            if has_base:
                print(
                    f"{'':<18} {'base/idiom':<{runtime_width}} {'':<{mode_width}} {'':<{cache_width}} "
                    f"{'':>10} {'':>10} {'':>10} {speedup:>8.2f} {'':>8}"
                )


def main(argv):
    parser = argparse.ArgumentParser(description="Run Idiom performance comparisons.")
    parser.add_argument("--idiom-current", default=str(ROOT / "build" / "release" / "idiomc"))
    parser.add_argument("--idiom-base")
    parser.add_argument("--idiom-base-cwd")
    parser.add_argument("--base-ref", help="Build and compare an idiomc from this git ref.")
    parser.add_argument("--runs", type=int, default=5)
    parser.add_argument("--warmups", type=int, default=1)
    parser.add_argument("--quick", action="store_true", help="Use 3 measured runs and 0 warmups.")
    parser.add_argument("--timeout", type=float, default=60.0)
    parser.add_argument("--cases", help="Comma-separated benchmark case names.")
    parser.add_argument("--runtimes", help="Comma-separated runtime targets: idiom, base, python, ruby, bash, elixir, elisp.")
    parser.add_argument("--no-external", action="store_true")
    parser.add_argument("--modes", default="source", help="Comma-separated Idiom modes: source, build, sealed.")
    parser.add_argument("--cache", default="hot", help="Comma-separated Idiom cache modes: hot, cold, off.")
    parser.add_argument("--with-sealed", action="store_true", help="Also benchmark Idiom sealed bytecode artifacts.")
    parser.add_argument("--json-out", type=Path)
    parser.add_argument("--dump-dir", type=Path, help="Write 'idiomc dump core' and 'idiomc dump bytecode' outputs for Idiom cases.")
    parser.add_argument("--callgrind-dir", type=Path, help="Run Idiom source cases under valgrind callgrind and store profiles.")
    parser.add_argument("--list", action="store_true")
    args = parser.parse_args(argv)

    if args.quick:
        args.runs = 3
        args.warmups = 0
    if args.runs < 1:
        fail("--runs must be >= 1")
    if args.warmups < 0:
        fail("--warmups must be >= 0")
    args.modes = parse_csv(args.modes, "source", IDIOM_MODES, "mode(s)")
    if args.with_sealed and "sealed" not in args.modes:
        args.modes.append("sealed")
    args.cache = parse_csv(args.cache, "hot", CACHE_MODES, "cache mode(s)", CACHE_ALIASES)
    args.runtime_targets = parse_runtime_targets(args.runtimes)

    cases = selected_cases(args)
    if args.list:
        for case in cases:
            print(f"{case['name']}: {case['summary']}")
        return 0

    base_tmp = None
    artifact_tmp = None
    if args.base_ref:
        base_tmp, base_bin, base_cwd = build_base_ref(args.base_ref)
        args.idiom_base = str(base_bin)
        args.idiom_base_cwd = str(base_cwd)
    artifact_tmp = tempfile.TemporaryDirectory(prefix="idiom-perf-artifacts-")
    args.artifact_dir = Path(artifact_tmp.name)

    runtimes = discover_runtimes(args)
    if not any(info.get("available") for info in runtimes.values()):
        fail("no runtimes available")
    emit_idiom_dumps(cases, runtimes, args.dump_dir, args.timeout)
    if args.callgrind_dir:
        args.callgrind_dir.mkdir(parents=True, exist_ok=True)

    results = []
    try:
        for case in cases:
            expected = expected_for_case(case, runtimes, args.timeout)
            case_result = {
                "name": case["name"],
                "summary": case["summary"],
                "expected": expected,
                "measurements": [],
            }
            print(f"running {case['name']}: {case['summary']}", file=sys.stderr)
            for runtime in runtimes.values():
                if not runtime.get("available"):
                    continue
                cmd = command_for(runtime, case)
                if not cmd:
                    continue
                runtime_expected = None if runtime.get("mode") == "build" else expected
                stats = measure(cmd, runtime.get("cwd"), args.runs, args.warmups, args.timeout, runtime_expected, runtime)
                stats.update(runtime_display(runtime))
                profile = run_callgrind(case, runtime, expected, args.callgrind_dir, args.timeout)
                if profile:
                    stats["callgrind"] = profile
                case_result["measurements"].append(stats)
            results.append(case_result)
    finally:
        if base_tmp is not None:
            base_tmp.cleanup()
        if artifact_tmp is not None:
            artifact_tmp.cleanup()

    payload = {
        "runs": args.runs,
        "warmups": args.warmups,
        "base_ref": args.base_ref,
        "idiom_modes": args.modes,
        "cache": args.cache,
        "dump_dir": str(args.dump_dir) if args.dump_dir else None,
        "callgrind_dir": str(args.callgrind_dir) if args.callgrind_dir else None,
        "results": results,
    }
    if args.json_out:
        args.json_out.parent.mkdir(parents=True, exist_ok=True)
        args.json_out.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n")
    print_table(results, runtimes)
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
