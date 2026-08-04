#!/usr/bin/env python3
"""Combine independent Range Analysis A/B comparison reports."""

from __future__ import annotations

import argparse
import json
import statistics
from pathlib import Path
from typing import Any, Dict, List, Sequence


CONFIGURATIONS = ("candidate", "compiled-defaults")


def summarize(runs: Sequence[Dict[str, Any]]) -> Dict[str, Any]:
    ra_times = [float(run["internal_ra_time_ms"]) for run in runs]
    wall_times = [float(run["compiler_wall_time_ms"]) for run in runs]
    rss_values = [float(run["max_rss_kb"]) for run in runs]
    return {
        "runs": len(runs),
        "all_hard_constraints_passed": all(
            run["hard_constraints_passed"] for run in runs
        ),
        "minimum_exact_queries": min(
            int(run["exact_queries"]) for run in runs
        ),
        "minimum_sound_queries": min(
            int(run["sound_queries"]) for run in runs
        ),
        "mean_internal_ra_time_ms": statistics.fmean(ra_times),
        "ra_time_pvariance_ms2": statistics.pvariance(ra_times),
        "ra_time_variance_ms2": statistics.variance(ra_times),
        "ra_time_pstdev_ms": statistics.pstdev(ra_times),
        "mean_compiler_wall_time_ms": statistics.fmean(wall_times),
        "compiler_wall_pvariance_ms2": statistics.pvariance(wall_times),
        "compiler_wall_variance_ms2": statistics.variance(wall_times),
        "compiler_wall_pstdev_ms": statistics.pstdev(wall_times),
        "mean_max_rss_kb": statistics.fmean(rss_values),
        "maximum_rss_kb": int(max(rss_values)),
    }


def improvement(candidate: float, defaults: float) -> float:
    return (1.0 - candidate / defaults) * 100.0


def render_markdown(report: Dict[str, Any]) -> str:
    lines = [
        "# Range Analysis Cumulative A/B Comparison",
        "",
        "This report combines independent alternating A/B batches.",
        (
            f"Each configuration has {report['runs_per_configuration']} "
            f"complete runs over {report['cases']} cases and "
            f"{report['queries']} queries."
        ),
        "",
        "## Cumulative Statistics",
        "",
        (
            "| Configuration | Runs | Min exact | Min sound | Mean RA ms | "
            "RA population variance ms^2 | RA sample variance ms^2 | "
            "RA stddev ms | Mean wall s | Wall population variance s^2 | "
            "Wall sample variance s^2 | Wall stddev s | Mean RSS KB | "
            "Max RSS KB |"
        ),
        (
            "|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|"
            "---:|---:|---:|"
        ),
    ]
    for name in CONFIGURATIONS:
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
                    (
                        f"{summary['mean_compiler_wall_time_ms'] / 1000.0:.3f}"
                    ),
                    (
                        f"{summary['compiler_wall_pvariance_ms2'] / 1_000_000.0:.6f}"
                    ),
                    (
                        f"{summary['compiler_wall_variance_ms2'] / 1_000_000.0:.6f}"
                    ),
                    (
                        f"{summary['compiler_wall_pstdev_ms'] / 1000.0:.3f}"
                    ),
                    f"{summary['mean_max_rss_kb']:.1f}",
                    str(summary["maximum_rss_kb"]),
                ]
            )
            + " |"
        )
    comparison = report["comparison"]
    lines.extend(
        [
            "",
            "## Candidate Versus Defaults",
            "",
            (
                "- Mean internal-RA improvement: "
                f"`{comparison['ra_improvement_percent']:.2f}%`"
            ),
            (
                "- Mean internal-RA delta: "
                f"`{comparison['mean_ra_delta_ms']:.3f} ms`"
            ),
            (
                "- Mean compiler-wall improvement: "
                f"`{comparison['compiler_wall_improvement_percent']:.2f}%`"
            ),
            (
                "- Mean compiler-wall delta: "
                f"`{comparison['mean_compiler_wall_delta_ms'] / 1000.0:.3f} s`"
            ),
            "",
            (
                "Population variance treats the observed runs as the full "
                "population. Sample variance uses the n-1 denominator."
            ),
        ]
    )
    return "\n".join(lines) + "\n"


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--inputs", type=Path, nargs="+", required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--markdown", type=Path, required=True)
    args = parser.parse_args()

    reports = [json.loads(path.read_text()) for path in args.inputs]
    first = reports[0]
    if any(
        report["cases"] != first["cases"]
        or report["queries"] != first["queries"]
        or report["params"] != first["params"]
        for report in reports[1:]
    ):
        raise ValueError("comparison inputs use different corpora or parameters")

    runs: List[Dict[str, Any]] = []
    for batch_index, report in enumerate(reports, start=1):
        for run in report["runs"]:
            combined = dict(run)
            combined["batch"] = batch_index
            runs.append(combined)

    grouped = {
        name: [run for run in runs if run["configuration"] == name]
        for name in CONFIGURATIONS
    }
    summaries = {
        name: summarize(grouped[name]) for name in CONFIGURATIONS
    }
    candidate = summaries["candidate"]
    defaults = summaries["compiled-defaults"]
    comparison = {
        "ra_improvement_percent": improvement(
            candidate["mean_internal_ra_time_ms"],
            defaults["mean_internal_ra_time_ms"],
        ),
        "compiler_wall_improvement_percent": improvement(
            candidate["mean_compiler_wall_time_ms"],
            defaults["mean_compiler_wall_time_ms"],
        ),
        "mean_ra_delta_ms": (
            candidate["mean_internal_ra_time_ms"]
            - defaults["mean_internal_ra_time_ms"]
        ),
        "mean_compiler_wall_delta_ms": (
            candidate["mean_compiler_wall_time_ms"]
            - defaults["mean_compiler_wall_time_ms"]
        ),
    }
    result = {
        "schema": 1,
        "batches": [str(path) for path in args.inputs],
        "runs_per_configuration": len(grouped["candidate"]),
        "cases": first["cases"],
        "queries": first["queries"],
        "params": first["params"],
        "runs": runs,
        "summaries": summaries,
        "comparison": comparison,
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(
        json.dumps(result, indent=2, sort_keys=True) + "\n"
    )
    args.markdown.parent.mkdir(parents=True, exist_ok=True)
    args.markdown.write_text(render_markdown(result))
    print(json.dumps(
        {"summaries": summaries, "comparison": comparison},
        indent=2,
        sort_keys=True,
    ))
    print(f"REPORT {args.output.resolve()}")
    print(f"MARKDOWN {args.markdown.resolve()}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
