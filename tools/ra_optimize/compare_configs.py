#!/usr/bin/env python3
"""Run an alternating A/B comparison of two RA parameter configurations."""

from __future__ import annotations

import argparse
import json
import statistics
from pathlib import Path
from typing import Any, Dict, List, Sequence

from evaluate import (
    DEFAULT_CJC,
    REPO_ROOT,
    RegressionCase,
    discover_semantic_cases,
    discover_vh_cases,
    evaluate,
    load_params,
)


def read_json(path: Path) -> Dict[str, Any]:
    return json.loads(path.read_text())


def arithmetic_mean(runs: Sequence[Dict[str, Any]], key: str) -> float:
    return statistics.fmean(float(run[key]) for run in runs)


def summarize(runs: Sequence[Dict[str, Any]]) -> Dict[str, Any]:
    ra_times = [float(run["internal_ra_time_ms"]) for run in runs]
    compiler_wall_times = [
        float(run["compiler_wall_time_ms"]) for run in runs
    ]
    return {
        "runs": len(runs),
        "all_hard_constraints_passed": all(
            run["hard_constraints_passed"] for run in runs
        ),
        "minimum_exact_queries": min(
            run["exact_queries"] for run in runs
        ),
        "minimum_sound_queries": min(
            run["sound_queries"] for run in runs
        ),
        "mean_internal_ra_time_ms": arithmetic_mean(
            runs, "internal_ra_time_ms"
        ),
        "mean_compiler_wall_time_ms": arithmetic_mean(
            runs, "compiler_wall_time_ms"
        ),
        "mean_max_rss_kb": arithmetic_mean(runs, "max_rss_kb"),
        "maximum_rss_kb": max(int(run["max_rss_kb"]) for run in runs),
        "ra_time_pvariance_ms2": statistics.pvariance(ra_times),
        "ra_time_variance_ms2": (
            statistics.variance(ra_times) if len(ra_times) > 1 else 0.0
        ),
        "ra_time_pstdev_ms": statistics.pstdev(ra_times),
        "compiler_wall_pvariance_ms2": statistics.pvariance(
            compiler_wall_times
        ),
        "compiler_wall_variance_ms2": (
            statistics.variance(compiler_wall_times)
            if len(compiler_wall_times) > 1
            else 0.0
        ),
        "compiler_wall_pstdev_ms": statistics.pstdev(
            compiler_wall_times
        ),
    }


def improvement(candidate: float, defaults: float) -> float:
    return (1.0 - candidate / defaults) * 100.0


def render_markdown(report: Dict[str, Any]) -> str:
    candidate_summary = report["summaries"]["candidate"]
    default_summary = report["summaries"]["compiled-defaults"]
    lines = [
        "# Range Analysis 10-Run A/B Comparison",
        "",
        "The candidate and compiled defaults were run sequentially on the same "
        "115-case/311-query training corpus. Execution order was reversed on "
        "even rounds to reduce order and thermal bias.",
        "",
        "## Configurations",
        "",
        "### Candidate",
        "",
        "```json",
        json.dumps(report["params"]["candidate"], indent=2, sort_keys=True),
        "```",
        "",
        "### Compiled Defaults",
        "",
        "```json",
        json.dumps(
            report["params"]["compiled-defaults"],
            indent=2,
            sort_keys=True,
        ),
        "```",
        "",
        "## Per-Run Results",
        "",
        (
            "| Round | Order | Configuration | Exact | Sound | RA ms | "
            "Compiler wall s | Max RSS KB | Result |"
        ),
        "|---:|---:|---|---:|---:|---:|---:|---:|---|",
    ]
    for entry in report["runs"]:
        lines.append(
            "| "
            + " | ".join(
                [
                    str(entry["round"]),
                    str(entry["order"]),
                    entry["configuration"],
                    (
                        f"{entry['exact_queries']}/"
                        f"{entry['total_queries']}"
                    ),
                    (
                        f"{entry['sound_queries']}/"
                        f"{entry['total_queries']}"
                    ),
                    f"{entry['internal_ra_time_ms']:.3f}",
                    f"{entry['compiler_wall_time_ms'] / 1000.0:.3f}",
                    str(entry["max_rss_kb"]),
                    entry["result"],
                ]
            )
            + " |"
        )
    lines.extend(
        [
            "",
            "## Arithmetic Means",
            "",
            (
                "| Configuration | Runs | Min Exact | Min Sound | "
                "Mean RA ms | RA population variance ms^2 | "
                "RA sample variance ms^2 | RA stddev ms | Mean wall s | "
                "Wall population variance s^2 | "
                "Wall sample variance s^2 | Wall stddev s | Mean RSS KB | "
                "Max RSS KB |"
            ),
            (
                "|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|"
                "---:|---:|---:|"
            ),
        ]
    )
    for name in ("candidate", "compiled-defaults"):
        summary = report["summaries"][name]
        lines.append(
            "| "
            + " | ".join(
                [
                    name,
                    str(summary["runs"]),
                    str(summary["minimum_exact_queries"]),
                    str(summary["minimum_sound_queries"]),
                    f"{summary['mean_internal_ra_time_ms']:.3f}",
                    f"{summary['ra_time_pvariance_ms2']:.3f}",
                    f"{summary['ra_time_variance_ms2']:.3f}",
                    f"{summary['ra_time_pstdev_ms']:.3f}",
                    f"{summary['mean_compiler_wall_time_ms'] / 1000.0:.3f}",
                    (
                        f"{summary['compiler_wall_pvariance_ms2'] / 1000000.0:.6f}"
                    ),
                    (
                        f"{summary['compiler_wall_variance_ms2'] / 1000000.0:.6f}"
                    ),
                    f"{summary['compiler_wall_pstdev_ms'] / 1000.0:.3f}",
                    f"{summary['mean_max_rss_kb']:.1f}",
                    str(summary["maximum_rss_kb"]),
                ]
            )
            + " |"
        )
    lines.extend(
        [
            "",
            "## Candidate Versus Defaults",
            "",
            (
                "- Mean RA-time improvement: "
                f"`{improvement(candidate_summary['mean_internal_ra_time_ms'], default_summary['mean_internal_ra_time_ms']):.2f}%`"
            ),
            (
                "- Mean compiler-wall improvement: "
                f"`{improvement(candidate_summary['mean_compiler_wall_time_ms'], default_summary['mean_compiler_wall_time_ms']):.2f}%`"
            ),
            (
                "- Mean RSS change: "
                f"`{-improvement(candidate_summary['mean_max_rss_kb'], default_summary['mean_max_rss_kb']):+.2f}%`"
            ),
            "",
        ]
    )
    return "\n".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--candidate-params", type=Path, required=True)
    parser.add_argument("--repeats", type=int, default=10)
    parser.add_argument("--cjc", type=Path, default=DEFAULT_CJC)
    parser.add_argument(
        "--baseline-result",
        type=Path,
        default=REPO_ROOT / ".ra_optimize/baseline-v2/result.json",
    )
    parser.add_argument("--case-timeout", type=float, default=30.0)
    parser.add_argument("--overall-timeout", type=float, default=180.0)
    parser.add_argument(
        "--output",
        type=Path,
        default=REPO_ROOT / ".ra_optimize/config-comparison.json",
    )
    parser.add_argument(
        "--markdown",
        type=Path,
        default=REPO_ROOT / ".ra_optimize/config-comparison.md",
    )
    args = parser.parse_args()

    if args.repeats <= 0:
        parser.error("--repeats must be positive")
    baseline = read_json(args.baseline_result)
    candidate_params = load_params(args.candidate_params.read_text())
    default_params = load_params(json.dumps(baseline["params"]))
    cases: List[RegressionCase] = [
        *discover_semantic_cases(),
        *discover_vh_cases(),
    ]
    configurations = {
        "candidate": candidate_params,
        "compiled-defaults": default_params,
    }
    runs: Dict[str, List[Dict[str, Any]]] = {
        name: [] for name in configurations
    }
    flat_runs: List[Dict[str, Any]] = []
    output_root = args.output.resolve().parent / "config-comparison-runs"

    for round_number in range(1, args.repeats + 1):
        order = (
            ["candidate", "compiled-defaults"]
            if round_number % 2 == 1
            else ["compiled-defaults", "candidate"]
        )
        for order_number, name in enumerate(order, start=1):
            run_root = output_root / f"round-{round_number:02d}" / name
            result_path = run_root / "result.json"
            payload = evaluate(
                cases=cases,
                cjc=args.cjc,
                params=configurations[name],
                result_path=result_path,
                log_root=run_root / "logs",
                case_timeout=args.case_timeout,
                overall_timeout=args.overall_timeout,
                fail_fast=False,
                baseline=baseline,
                progress=False,
            )
            runs[name].append(payload)
            entry = {
                "round": round_number,
                "order": order_number,
                "configuration": name,
                "exact_queries": payload["exact_queries"],
                "sound_queries": payload["sound_queries"],
                "total_queries": payload["total_queries"],
                "internal_ra_time_ms": payload["internal_ra_time_ms"],
                "compiler_wall_time_ms": payload[
                    "compiler_wall_time_ms"
                ],
                "max_rss_kb": payload["max_rss_kb"],
                "hard_constraints_passed": payload[
                    "hard_constraints_passed"
                ],
                "result": str(result_path),
            }
            flat_runs.append(entry)
            print(
                f"COMPARE round={round_number}/{args.repeats} "
                f"order={order_number}/2 config={name} "
                f"exact={payload['exact_queries']}/"
                f"{payload['total_queries']} "
                f"sound={payload['sound_queries']}/"
                f"{payload['total_queries']} "
                f"ra_ms={payload['internal_ra_time_ms']:.3f} "
                f"wall_s={payload['compiler_wall_time_ms'] / 1000.0:.3f} "
                f"rss_kb={payload['max_rss_kb']}",
                flush=True,
            )

    report = {
        "schema": 1,
        "repeats": args.repeats,
        "cases": len(cases),
        "queries": sum(len(case.expected) for case in cases),
        "params": configurations,
        "runs": flat_runs,
        "summaries": {
            name: summarize(config_runs)
            for name, config_runs in runs.items()
        },
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(
        json.dumps(report, indent=2, sort_keys=True) + "\n"
    )
    args.markdown.parent.mkdir(parents=True, exist_ok=True)
    args.markdown.write_text(render_markdown(report))
    print(f"REPORT {args.output.resolve()}")
    print(f"MARKDOWN {args.markdown.resolve()}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
