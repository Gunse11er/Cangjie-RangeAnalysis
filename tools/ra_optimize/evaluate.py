#!/usr/bin/env python3
"""Evaluate Range Analysis resource configurations on the WSL regression corpus."""

from __future__ import annotations

import argparse
import glob
import json
import math
import os
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import time
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Any, Dict, Iterable, List, Optional, Sequence, Set, Tuple

try:
    import psutil  # type: ignore
except ImportError:
    psutil = None


REPO_ROOT = Path(__file__).resolve().parents[2]
TOOL_ROOT = Path(__file__).resolve().parent
DEFAULT_CJC = REPO_ROOT / "output/bin/cjc"
SEMANTIC_ROOT = Path("/home/gunseller/project/ra_hidden_shape_tests_20260726")
VH_ROOT = Path("/home/gunseller/project/ra_converted_tests/vhscampos_cj")

PARAM_SPECS = {
    "widen_start": ("CJ_RA_WIDEN_START", 4, 2, 8),
    "max_inqueue": ("CJ_RA_MAX_INQUEUE", 32, 16, 64),
    "context_per_function": ("CJ_RA_CONTEXT_PER_FUNCTION", 32, 8, 64),
    "total_context_summaries": ("CJ_RA_TOTAL_CONTEXT_SUMMARIES", 512, 128, 1024),
    "context_depth": ("CJ_RA_CONTEXT_DEPTH", 16, 4, 24),
    "exact_set_size": ("CJ_RA_EXACT_SET_SIZE", 64, 8, 128),
    "loop_observations": ("CJ_RA_LOOP_OBSERVATIONS", 4096, 512, 8192),
    "recorded_contexts": ("CJ_RA_RECORDED_CONTEXTS", 4096, 1024, 8192),
}

INTERVAL_RE = re.compile(
    r"^\[\s*(-?\d+)\s*,\s*(-?\d+)\s*:\s*(\d+)\s*\]$"
)
SYMBOLIC_ENUM_LIMIT = 200_000
SYMBOLIC_PERIOD_LIMIT = 200_000


@dataclass(frozen=True)
class Progression:
    start: int
    end: int
    step: int

    def __post_init__(self) -> None:
        if self.step <= 0 or self.start > self.end:
            raise ValueError(f"invalid progression {self}")
        reachable_end = self.start + ((self.end - self.start) // self.step) * self.step
        object.__setattr__(self, "end", reachable_end)

    @property
    def count(self) -> int:
        return (self.end - self.start) // self.step + 1

    def contains(self, value: int) -> bool:
        return (
            self.start <= value <= self.end
            and (value - self.start) % self.step == 0
        )


@dataclass(frozen=True)
class AbstractSet:
    booleans: frozenset
    integers: Tuple[Progression, ...]


@dataclass(frozen=True)
class RegressionCase:
    suite: str
    name: str
    directory: Path
    expected: Tuple[str, ...]

    @property
    def qualified_name(self) -> str:
        return f"{self.suite}/{self.name}"


@dataclass
class CaseResult:
    name: str
    expected_lines: int
    actual_lines: int
    exact_lines: int
    sound_lines: int
    returncode: Optional[int]
    timed_out: bool
    wall_time_ms: float
    internal_ra_time_ms: float
    max_rss_kb: int
    stats_records: int
    stderr_tail: str
    exact_mask: List[bool]
    sound_mask: List[bool]
    mismatches: List[Dict[str, Any]]


def split_items(line: str) -> List[str]:
    items: List[str] = []
    start = 0
    depth = 0
    for index, char in enumerate(line):
        if char == "[":
            depth += 1
        elif char == "]":
            depth -= 1
        elif char == "," and depth == 0:
            items.append(line[start:index].strip())
            start = index + 1
    tail = line[start:].strip()
    if tail:
        items.append(tail)
    return items


def parse_abstract_set(line: str) -> AbstractSet:
    booleans: Set[str] = set()
    integers: List[Progression] = []
    for item in split_items(line):
        if item in {"true", "false"}:
            booleans.add(item)
            continue
        match = INTERVAL_RE.fullmatch(item)
        if match is not None:
            lower, upper, step = map(int, match.groups())
            integers.append(Progression(lower, upper, step))
            continue
        value = int(item)
        integers.append(Progression(value, value, 1))
    return AbstractSet(frozenset(booleans), tuple(integers))


def _ceil_div(dividend: int, divisor: int) -> int:
    return -((-dividend) // divisor)


def _least_common_multiple(left: int, right: int) -> int:
    return left // math.gcd(left, right) * right


def _intersection_in_expected_indices(
    expected: Progression, actual: Progression
) -> Optional[Tuple[int, int, int, int]]:
    """Return (lo, hi, residue, modulus) for expected indices covered by actual."""
    lower = max(expected.start, actual.start)
    upper = min(expected.end, actual.end)
    if lower > upper:
        return None

    lo = max(0, _ceil_div(lower - expected.start, expected.step))
    hi = min(expected.count - 1, (upper - expected.start) // expected.step)
    if lo > hi:
        return None

    gcd = math.gcd(expected.step, actual.step)
    difference = actual.start - expected.start
    if difference % gcd != 0:
        return None

    modulus = actual.step // gcd
    if modulus == 1:
        residue = 0
    else:
        coefficient = expected.step // gcd
        residue = (
            (difference // gcd) * pow(coefficient, -1, modulus)
        ) % modulus
    first = lo + ((residue - lo) % modulus)
    if first > hi:
        return None
    return lo, hi, residue, modulus


def _segment_is_covered(
    lower: int,
    upper: int,
    active: Sequence[Tuple[int, int]],
) -> bool:
    if lower > upper:
        return True
    if any(modulus == 1 for _, modulus in active):
        return True

    length = upper - lower + 1
    if length <= SYMBOLIC_ENUM_LIMIT:
        return all(
            any(index % modulus == residue for residue, modulus in active)
            for index in range(lower, upper + 1)
        )

    period = 1
    for _, modulus in active:
        period = _least_common_multiple(period, modulus)
        if period > SYMBOLIC_PERIOD_LIMIT:
            return False
    sample_end = lower + min(length, period) - 1
    return all(
        any(index % modulus == residue for residue, modulus in active)
        for index in range(lower, sample_end + 1)
    )


def progression_is_subset(
    expected: Progression, actual: Sequence[Progression]
) -> bool:
    if expected.count <= SYMBOLIC_ENUM_LIMIT:
        return all(
            any(candidate.contains(value) for candidate in actual)
            for value in range(expected.start, expected.end + 1, expected.step)
        )

    intersections = [
        intersection
        for candidate in actual
        if (
            intersection := _intersection_in_expected_indices(
                expected, candidate
            )
        )
        is not None
    ]
    if not intersections:
        return False

    boundaries = {0, expected.count}
    for lower, upper, _, _ in intersections:
        boundaries.add(lower)
        boundaries.add(upper + 1)
    ordered = sorted(boundaries)
    for segment_start, segment_end_exclusive in zip(ordered, ordered[1:]):
        segment_end = segment_end_exclusive - 1
        active = [
            (residue, modulus)
            for lower, upper, residue, modulus in intersections
            if lower <= segment_start and segment_end <= upper
        ]
        if not active or not _segment_is_covered(
            segment_start, segment_end, active
        ):
            return False
    return True


def abstract_set_is_subset(expected: AbstractSet, actual: AbstractSet) -> bool:
    if not expected.booleans.issubset(actual.booleans):
        return False
    return all(
        progression_is_subset(progression, actual.integers)
        for progression in expected.integers
    )


def compare_line(expected_text: str, actual_text: str) -> Tuple[bool, bool]:
    expected = parse_abstract_set(expected_text)
    actual = parse_abstract_set(actual_text)
    sound = abstract_set_is_subset(expected, actual)
    exact = sound and abstract_set_is_subset(actual, expected)
    return exact, sound


def discover_semantic_cases() -> List[RegressionCase]:
    patterns = [
        "/home/gunseller/project/ra_*_regression/*/expected.txt",
        "/home/gunseller/project/ra_loop_targeted_20260726/*/expected.txt",
        "/home/gunseller/project/ra_hidden_shape_tests_20260726/*/expected.txt",
    ]
    directories = {
        Path(path).parent
        for pattern in patterns
        for path in glob.glob(pattern)
        if "fix1_full_baseline" not in path
    }
    return [
        RegressionCase(
            suite="semantic",
            name=directory.name,
            directory=directory,
            expected=tuple(
                (directory / "expected.txt").read_text().splitlines()
            ),
        )
        for directory in sorted(directories)
    ]


def discover_vh_cases() -> List[RegressionCase]:
    expected_path = VH_ROOT / "expected_results.json"
    expected = json.loads(expected_path.read_text())
    return [
        RegressionCase(
            suite="vhscampos",
            name=name,
            directory=VH_ROOT / name,
            expected=tuple(lines),
        )
        for name, lines in sorted(expected.items())
    ]


def discover_stress_cases(manifest: Optional[Path]) -> List[RegressionCase]:
    if manifest is None or not manifest.is_file():
        return []
    entries = json.loads(manifest.read_text())
    cases: List[RegressionCase] = []
    for entry in entries:
        directory = Path(entry["directory"])
        expected_path = directory / entry.get("expected", "expected.txt")
        cases.append(
            RegressionCase(
                suite="stress",
                name=entry.get("name", directory.name),
                directory=directory,
                expected=tuple(expected_path.read_text().splitlines()),
            )
        )
    return sorted(cases, key=lambda case: case.name)


def sources_for(case_dir: Path) -> List[Path]:
    source_root = case_dir / "src"
    if source_root.is_dir():
        return sorted(source_root.glob("*.cj"))
    return sorted(case_dir.glob("*.cj"))


def prepare_workspace(case: RegressionCase, root: Path) -> Tuple[Path, List[Path]]:
    workspace = root / case.suite / case.name
    workspace.mkdir(parents=True, exist_ok=True)
    for filename in ("input.txt", "cjpm.toml"):
        source = case.directory / filename
        if source.is_file():
            shutil.copy2(source, workspace / filename)
    copied_sources: List[Path] = []
    for source in sources_for(case.directory):
        relative = source.relative_to(case.directory)
        destination = workspace / relative
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, destination)
        copied_sources.append(destination)
    return workspace, copied_sources


def load_params(raw: Optional[str]) -> Dict[str, int]:
    params = {name: spec[1] for name, spec in PARAM_SPECS.items()}
    if raw:
        path = Path(raw)
        updates = json.loads(path.read_text() if path.is_file() else raw)
        unknown = sorted(set(updates) - set(PARAM_SPECS))
        if unknown:
            raise ValueError(f"unknown parameters: {', '.join(unknown)}")
        params.update({name: int(value) for name, value in updates.items()})
    for name, value in params.items():
        _, _, minimum, maximum = PARAM_SPECS[name]
        if not minimum <= value <= maximum:
            raise ValueError(
                f"{name}={value} is outside [{minimum}, {maximum}]"
            )
    if params["max_inqueue"] <= params["widen_start"]:
        raise ValueError("max_inqueue must be greater than widen_start")
    return params


def params_environment(params: Dict[str, int]) -> Dict[str, str]:
    return {
        PARAM_SPECS[name][0]: str(value)
        for name, value in params.items()
    }


def _procfs_descendants(pid: int) -> Set[int]:
    pending = [pid]
    result: Set[int] = set()
    while pending:
        current = pending.pop()
        if current in result:
            continue
        result.add(current)
        children_path = Path(f"/proc/{current}/task/{current}/children")
        try:
            pending.extend(
                int(child) for child in children_path.read_text().split()
            )
        except (FileNotFoundError, ProcessLookupError, PermissionError):
            pass
    return result


def process_tree_rss_kb(pid: int) -> int:
    if psutil is not None:
        try:
            process = psutil.Process(pid)
            processes = [process, *process.children(recursive=True)]
            return sum(
                item.memory_info().rss
                for item in processes
                if item.is_running()
            ) // 1024
        except (psutil.NoSuchProcess, psutil.AccessDenied):
            return 0

    rss = 0
    for descendant in _procfs_descendants(pid):
        try:
            for line in Path(f"/proc/{descendant}/status").read_text().splitlines():
                if line.startswith("VmRSS:"):
                    rss += int(line.split()[1])
                    break
        except (FileNotFoundError, ProcessLookupError, PermissionError):
            pass
    return rss


def terminate_process_group(process: subprocess.Popen) -> None:
    if process.poll() is not None:
        return
    try:
        os.killpg(process.pid, signal.SIGTERM)
        process.wait(timeout=1.0)
    except (ProcessLookupError, subprocess.TimeoutExpired):
        try:
            os.killpg(process.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        process.wait()


def run_process(
    command: Sequence[str],
    cwd: Path,
    env: Dict[str, str],
    stderr_path: Path,
    timeout_seconds: float,
) -> Tuple[int, bool, float, int]:
    started = time.monotonic()
    peak_rss_kb = 0
    timed_out = False
    with stderr_path.open("wb") as stderr:
        process = subprocess.Popen(
            command,
            cwd=cwd,
            env=env,
            stdin=subprocess.DEVNULL,
            stdout=subprocess.DEVNULL,
            stderr=stderr,
            start_new_session=True,
        )
        while process.poll() is None:
            peak_rss_kb = max(peak_rss_kb, process_tree_rss_kb(process.pid))
            if time.monotonic() - started >= timeout_seconds:
                timed_out = True
                terminate_process_group(process)
                break
            time.sleep(0.005)
        peak_rss_kb = max(peak_rss_kb, process_tree_rss_kb(process.pid))
    elapsed_ms = (time.monotonic() - started) * 1000.0
    returncode = process.returncode if process.returncode is not None else -1
    return returncode, timed_out, elapsed_ms, peak_rss_kb


def read_jsonl(path: Path) -> List[Dict[str, Any]]:
    if not path.is_file():
        return []
    records = []
    for line in path.read_text().splitlines():
        if line.strip():
            records.append(json.loads(line))
    return records


def internal_time_ms(records: Sequence[Dict[str, Any]]) -> float:
    total_ns = 0
    for record in records:
        if "internal_elapsed_ns" in record:
            total_ns += int(record["internal_elapsed_ns"])
        elif "internal_ns" in record:
            total_ns += int(record["internal_ns"])
        elif "elapsed_ns" in record:
            total_ns += int(record["elapsed_ns"])
        elif "elapsed_ms" in record:
            total_ns += int(float(record["elapsed_ms"]) * 1_000_000)
    return total_ns / 1_000_000.0


def flatten_numeric_stats(
    records: Sequence[Dict[str, Any]]
) -> Dict[str, float]:
    totals: Dict[str, float] = {}
    maxima: Dict[str, float] = {}

    def visit(prefix: str, value: Any) -> None:
        if isinstance(value, bool):
            return
        if isinstance(value, (int, float)):
            totals[prefix] = totals.get(prefix, 0.0) + float(value)
            maxima[prefix] = max(maxima.get(prefix, float("-inf")), float(value))
            return
        if isinstance(value, dict):
            for key, child in value.items():
                visit(f"{prefix}.{key}" if prefix else key, child)

    for record in records:
        visit("", record)
    return {
        **{f"sum.{key}": value for key, value in sorted(totals.items())},
        **{f"max.{key}": value for key, value in sorted(maxima.items())},
    }


def evaluate_case(
    case: RegressionCase,
    cjc: Path,
    params: Dict[str, int],
    temporary_root: Path,
    log_root: Path,
    case_timeout: float,
) -> Tuple[CaseResult, List[Dict[str, Any]]]:
    workspace, sources = prepare_workspace(case, temporary_root)
    log_directory = log_root / case.suite
    log_directory.mkdir(parents=True, exist_ok=True)
    stderr_path = log_directory / f"{case.name}.stderr.log"
    stats_path = log_directory / f"{case.name}.stats.jsonl"
    stats_path.unlink(missing_ok=True)

    output_binary = temporary_root / "bin" / f"{case.suite}-{case.name}"
    output_binary.parent.mkdir(parents=True, exist_ok=True)
    command = [
        str(cjc),
        "-O2",
        *(str(path) for path in sources),
        "-o",
        str(output_binary),
    ]
    env = os.environ.copy()
    env.update(params_environment(params))
    env["CJ_RA_STATS_FILE"] = str(stats_path)
    returncode, timed_out, wall_time_ms, peak_rss_kb = run_process(
        command, workspace, env, stderr_path, case_timeout
    )
    records = read_jsonl(stats_path)

    output_path = workspace / "output.txt"
    actual = output_path.read_text().splitlines() if output_path.is_file() else []
    exact_lines = 0
    sound_lines = 0
    exact_mask: List[bool] = []
    sound_mask: List[bool] = []
    mismatches: List[Dict[str, Any]] = []
    for index, expected_text in enumerate(case.expected):
        actual_text = actual[index] if index < len(actual) else ""
        try:
            exact, sound = compare_line(expected_text, actual_text)
            error = ""
        except (TypeError, ValueError) as exception:
            exact, sound = False, False
            error = str(exception)
        exact_lines += int(exact)
        sound_lines += int(sound)
        exact_mask.append(exact)
        sound_mask.append(sound)
        if not exact or not sound:
            mismatches.append(
                {
                    "line": index + 1,
                    "expected": expected_text,
                    "actual": actual_text,
                    "exact": exact,
                    "sound": sound,
                    "error": error,
                }
            )
    if len(actual) != len(case.expected):
        mismatches.append(
            {
                "line_count": True,
                "expected": len(case.expected),
                "actual": len(actual),
            }
        )

    stderr_tail = ""
    if stderr_path.is_file():
        stderr_tail = stderr_path.read_text(errors="replace")[-4000:]
    result = CaseResult(
        name=case.qualified_name,
        expected_lines=len(case.expected),
        actual_lines=len(actual),
        exact_lines=exact_lines,
        sound_lines=sound_lines,
        returncode=returncode,
        timed_out=timed_out,
        wall_time_ms=wall_time_ms,
        internal_ra_time_ms=internal_time_ms(records),
        max_rss_kb=peak_rss_kb,
        stats_records=len(records),
        stderr_tail=stderr_tail if returncode != 0 or timed_out else "",
        exact_mask=exact_mask,
        sound_mask=sound_mask,
        mismatches=mismatches,
    )
    return result, records


def evaluate(
    cases: Sequence[RegressionCase],
    cjc: Path,
    params: Dict[str, int],
    result_path: Path,
    log_root: Path,
    case_timeout: float,
    overall_timeout: float,
    fail_fast: bool,
    baseline: Optional[Dict[str, Any]] = None,
    progress: bool = True,
) -> Dict[str, Any]:
    started = time.monotonic()
    results: List[CaseResult] = []
    all_stats: List[Dict[str, Any]] = []
    with tempfile.TemporaryDirectory(prefix="ra-opt-evaluate-") as temporary:
        temporary_root = Path(temporary)
        for index, case in enumerate(cases, start=1):
            elapsed = time.monotonic() - started
            if elapsed >= overall_timeout:
                results.append(
                    CaseResult(
                        name=case.qualified_name,
                        expected_lines=len(case.expected),
                        actual_lines=0,
                        exact_lines=0,
                        sound_lines=0,
                        returncode=None,
                        timed_out=True,
                        wall_time_ms=0.0,
                        internal_ra_time_ms=0.0,
                        max_rss_kb=0,
                        stats_records=0,
                        stderr_tail="overall evaluation timeout",
                        exact_mask=[False] * len(case.expected),
                        sound_mask=[False] * len(case.expected),
                        mismatches=[],
                    )
                )
                break
            timeout = min(case_timeout, overall_timeout - elapsed)
            result, records = evaluate_case(
                case,
                cjc,
                params,
                temporary_root,
                log_root,
                timeout,
            )
            results.append(result)
            all_stats.extend(records)
            if progress:
                print(
                    f"[{index:03d}/{len(cases):03d}] {result.name}: "
                    f"exact={result.exact_lines}/{result.expected_lines} "
                    f"sound={result.sound_lines}/{result.expected_lines} "
                    f"wall_ms={result.wall_time_ms:.1f} "
                    f"ra_ms={result.internal_ra_time_ms:.3f} "
                    f"rss_kb={result.max_rss_kb}",
                    flush=True,
                )
            failed = (
                result.returncode != 0
                or result.timed_out
                or result.sound_lines != result.expected_lines
            )
            if fail_fast and failed:
                break

    total_queries = sum(result.expected_lines for result in results)
    exact_queries = sum(result.exact_lines for result in results)
    sound_queries = sum(result.sound_lines for result in results)
    compile_failures = sum(
        result.returncode not in (0, None) for result in results
    )
    timeouts = sum(result.timed_out for result in results)
    baseline_cases = {
        entry["name"]: entry
        for entry in (baseline or {}).get("case_results", [])
    }
    regressed_exact_queries: List[str] = []
    baseline_exact_queries = 0
    for result in results:
        baseline_case = baseline_cases.get(result.name)
        if baseline_case is None:
            baseline_mask = [True] * result.expected_lines
        else:
            baseline_mask = baseline_case.get("exact_mask")
            if baseline_mask is None:
                baseline_mismatch_lines = {
                    int(mismatch["line"])
                    for mismatch in baseline_case.get("mismatches", [])
                    if "line" in mismatch
                }
                baseline_mask = [
                    index + 1 not in baseline_mismatch_lines
                    for index in range(result.expected_lines)
                ]
        baseline_exact_queries += sum(bool(value) for value in baseline_mask)
        for index, was_exact in enumerate(baseline_mask):
            remains_exact = (
                index < len(result.exact_mask) and result.exact_mask[index]
            )
            if was_exact and not remains_exact:
                regressed_exact_queries.append(f"{result.name}:{index + 1}")
    regressed_exact_cases = sorted(
        {query.rsplit(":", 1)[0] for query in regressed_exact_queries}
    )
    total_wall_time_ms = (time.monotonic() - started) * 1000.0
    payload = {
        "schema": 1,
        "timestamp": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
        "cjc": str(cjc),
        "params": params,
        "cases_requested": len(cases),
        "cases_completed": len(results),
        "total_queries": total_queries,
        "exact_queries": exact_queries,
        "sound_queries": sound_queries,
        "baseline_exact_queries": baseline_exact_queries,
        "compile_failures": compile_failures,
        "timeouts": timeouts,
        "regressed_exact_cases": regressed_exact_cases,
        "regressed_exact_queries": regressed_exact_queries,
        "wall_time_ms": total_wall_time_ms,
        "compiler_wall_time_ms": sum(
            result.wall_time_ms for result in results
        ),
        "internal_ra_time_ms": sum(
            result.internal_ra_time_ms for result in results
        ),
        "max_rss_kb": max(
            (result.max_rss_kb for result in results), default=0
        ),
        "hard_constraints_passed": (
            len(results) == len(cases)
            and exact_queries >= baseline_exact_queries
            and sound_queries == total_queries
            and compile_failures == 0
            and timeouts == 0
            and not regressed_exact_queries
        ),
        "aggregated_stats": flatten_numeric_stats(all_stats),
        "case_results": [asdict(result) for result in results],
    }
    result_path.parent.mkdir(parents=True, exist_ok=True)
    result_path.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n")
    return payload


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Evaluate one Range Analysis parameter configuration."
    )
    parser.add_argument("--cjc", type=Path, default=DEFAULT_CJC)
    parser.add_argument(
        "--suite",
        choices=("all", "semantic", "vhscampos", "stress"),
        default="all",
    )
    parser.add_argument(
        "--stress-manifest",
        type=Path,
        default=TOOL_ROOT / "stress_manifest.json",
    )
    parser.add_argument("--params", help="JSON object or path to JSON file")
    parser.add_argument(
        "--baseline-result",
        type=Path,
        help="Previous evaluation whose exact query lines must not regress.",
    )
    parser.add_argument("--case-regex")
    parser.add_argument("--case-timeout", type=float, default=30.0)
    parser.add_argument("--overall-timeout", type=float, default=180.0)
    parser.add_argument(
        "--result",
        type=Path,
        default=REPO_ROOT / ".ra_optimize/evaluation.json",
    )
    parser.add_argument(
        "--log-dir",
        type=Path,
        default=REPO_ROOT / ".ra_optimize/logs",
    )
    parser.add_argument("--fail-fast", action="store_true")
    parser.add_argument("--list-cases", action="store_true")
    return parser


def main() -> int:
    args = build_parser().parse_args()
    params = load_params(args.params)
    semantic = discover_semantic_cases()
    vhscampos = discover_vh_cases()
    stress = discover_stress_cases(args.stress_manifest)
    if args.suite == "semantic":
        cases = semantic
    elif args.suite == "vhscampos":
        cases = vhscampos
    elif args.suite == "stress":
        cases = stress
    else:
        cases = [*semantic, *vhscampos]
    if args.case_regex:
        pattern = re.compile(args.case_regex)
        cases = [
            case for case in cases if pattern.search(case.qualified_name)
        ]
    if args.list_cases:
        for case in cases:
            print(f"{case.qualified_name}\t{len(case.expected)}")
        print(
            f"SUMMARY cases={len(cases)} "
            f"queries={sum(len(case.expected) for case in cases)}"
        )
        return 0
    if not args.cjc.is_file():
        print(f"cjc not found: {args.cjc}", file=sys.stderr)
        return 2
    if not cases:
        print("no regression cases selected", file=sys.stderr)
        return 2
    baseline = None
    if args.baseline_result is not None:
        baseline = json.loads(args.baseline_result.read_text())

    payload = evaluate(
        cases=cases,
        cjc=args.cjc,
        params=params,
        result_path=args.result.resolve(),
        log_root=args.log_dir.resolve(),
        case_timeout=args.case_timeout,
        overall_timeout=args.overall_timeout,
        fail_fast=args.fail_fast,
        baseline=baseline,
    )
    print(
        "SUMMARY "
        f"cases={payload['cases_completed']}/{payload['cases_requested']} "
        f"exact={payload['exact_queries']}/{payload['total_queries']} "
        f"sound={payload['sound_queries']}/{payload['total_queries']} "
        f"compile_failures={payload['compile_failures']} "
        f"timeouts={payload['timeouts']} "
        f"wall_s={payload['wall_time_ms'] / 1000.0:.2f} "
        f"compiler_wall_s={payload['compiler_wall_time_ms'] / 1000.0:.2f} "
        f"ra_ms={payload['internal_ra_time_ms']:.3f} "
        f"max_rss_kb={payload['max_rss_kb']} "
        f"hard_constraints_passed={payload['hard_constraints_passed']}"
    )
    print(f"RESULT {args.result}")
    return 0 if payload["hard_constraints_passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
