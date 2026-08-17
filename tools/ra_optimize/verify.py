#!/usr/bin/env python3
"""Repeatedly verify top Optuna candidates on training and held-out suites."""

from __future__ import annotations

import argparse
import json
import statistics
from pathlib import Path
from typing import Any, Dict, List, Sequence

import optuna

from evaluate import (
    DEFAULT_CJC,
    REPO_ROOT,
    discover_semantic_cases,
    discover_stress_cases,
    discover_vh_cases,
    evaluate,
)
from optimize import sqlite_storage_url


def read_json(path: Path) -> Dict[str, Any]:
    return json.loads(path.read_text())


def percentile90(values: Sequence[float]) -> float:
    ordered = sorted(values)
    if not ordered:
        return 0.0
    index = max(0, (9 * len(ordered) + 9) // 10 - 1)
    return float(ordered[min(index, len(ordered) - 1)])


def summarize_runs(runs: Sequence[Dict[str, Any]]) -> Dict[str, Any]:
    return {
        "runs": len(runs),
        "all_hard_constraints_passed": all(
            run["hard_constraints_passed"] for run in runs
        ),
        "minimum_exact_queries": min(
            (run["exact_queries"] for run in runs), default=0
        ),
        "minimum_sound_queries": min(
            (run["sound_queries"] for run in runs), default=0
        ),
        "median_internal_ra_time_ms": statistics.median(
            run["internal_ra_time_ms"] for run in runs
        ),
        "p90_internal_ra_time_ms": percentile90(
            [run["internal_ra_time_ms"] for run in runs]
        ),
        "median_compiler_wall_time_ms": statistics.median(
            run["compiler_wall_time_ms"] for run in runs
        ),
        "p90_compiler_wall_time_ms": percentile90(
            [run["compiler_wall_time_ms"] for run in runs]
        ),
        "max_rss_kb": max((run["max_rss_kb"] for run in runs), default=0),
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--study-name", required=True)
    parser.add_argument(
        "--storage",
        type=Path,
        default=REPO_ROOT / ".ra_optimize/optuna.db",
    )
    parser.add_argument("--top", type=int, default=3)
    parser.add_argument("--repeats", type=int, default=5)
    parser.add_argument("--cjc", type=Path, default=DEFAULT_CJC)
    parser.add_argument(
        "--baseline-result",
        type=Path,
        default=REPO_ROOT / ".ra_optimize/baseline-v2/result.json",
    )
    parser.add_argument(
        "--stress-baseline-result",
        type=Path,
        default=REPO_ROOT / ".ra_optimize/stress-baseline/result.json",
    )
    parser.add_argument(
        "--stress-manifest",
        type=Path,
        default=Path(__file__).resolve().parent / "stress_manifest.json",
    )
    parser.add_argument("--case-timeout", type=float, default=30.0)
    parser.add_argument("--overall-timeout", type=float, default=180.0)
    parser.add_argument("--skip-stress", action="store_true")
    parser.add_argument("--no-defaults", action="store_true")
    parser.add_argument(
        "--output",
        type=Path,
        default=REPO_ROOT / ".ra_optimize/verification.json",
    )
    args = parser.parse_args()

    if args.top <= 0 or args.repeats <= 0:
        parser.error("--top and --repeats must be positive")
    study = optuna.load_study(
        study_name=args.study_name,
        storage=sqlite_storage_url(args.storage),
    )
    trials = sorted(
        (
            trial
            for trial in study.trials
            if trial.state == optuna.trial.TrialState.COMPLETE
            and trial.user_attrs.get("valid")
        ),
        key=lambda trial: float(trial.value),
    )[: args.top]
    if not trials:
        print("No valid completed trials to verify.")
        return 1

    training_cases = [*discover_semantic_cases(), *discover_vh_cases()]
    stress_cases = discover_stress_cases(args.stress_manifest)
    training_baseline = read_json(args.baseline_result)
    stress_baseline = read_json(args.stress_baseline_result)
    output_root = args.output.resolve().parent / "verification-runs"
    candidates: List[Dict[str, Any]] = []
    candidate_specs: List[Dict[str, Any]] = []
    seen_params = set()
    for trial in trials:
        source_result = read_json(Path(trial.user_attrs["result"]))
        params = source_result["params"]
        identity = json.dumps(params, sort_keys=True)
        if identity in seen_params:
            continue
        seen_params.add(identity)
        candidate_specs.append(
            {
                "trial": trial.number,
                "study_score": trial.value,
                "params": params,
            }
        )
    if not args.no_defaults:
        default_params = training_baseline["params"]
        identity = json.dumps(default_params, sort_keys=True)
        if identity not in seen_params:
            candidate_specs.append(
                {
                    "trial": "compiled-defaults",
                    "study_score": None,
                    "params": default_params,
                }
            )

    for rank, candidate_spec in enumerate(candidate_specs, start=1):
        params = candidate_spec["params"]
        training_runs: List[Dict[str, Any]] = []
        stress_runs: List[Dict[str, Any]] = []
        for repeat in range(1, args.repeats + 1):
            run_root = output_root / f"candidate-{rank}" / f"run-{repeat}"
            training = evaluate(
                cases=training_cases,
                cjc=args.cjc,
                params=params,
                result_path=run_root / "training.json",
                log_root=run_root / "training-logs",
                case_timeout=args.case_timeout,
                overall_timeout=args.overall_timeout,
                fail_fast=False,
                baseline=training_baseline,
                progress=False,
            )
            training_runs.append(training)
            if not args.skip_stress:
                stress = evaluate(
                    cases=stress_cases,
                    cjc=args.cjc,
                    params=params,
                    result_path=run_root / "stress.json",
                    log_root=run_root / "stress-logs",
                    case_timeout=args.case_timeout,
                    overall_timeout=args.overall_timeout,
                    fail_fast=False,
                    baseline=stress_baseline,
                    progress=False,
                )
                stress_runs.append(stress)
            print(
                f"VERIFY candidate={rank}/{len(candidate_specs)} "
                f"trial={candidate_spec['trial']} "
                f"repeat={repeat}/{args.repeats} "
                f"train_exact={training['exact_queries']}/"
                f"{training['total_queries']} "
                f"train_ra_ms={training['internal_ra_time_ms']:.3f} "
                f"train_wall_s={training['compiler_wall_time_ms'] / 1000.0:.3f}"
                + (
                    f" stress_exact={stress_runs[-1]['exact_queries']}/"
                    f"{stress_runs[-1]['total_queries']}"
                    if stress_runs
                    else ""
                ),
                flush=True,
            )
            if not training["hard_constraints_passed"] or (
                stress_runs
                and not stress_runs[-1]["hard_constraints_passed"]
            ):
                print(
                    f"REJECT candidate={rank} trial={candidate_spec['trial']} "
                    "after first hard-constraint failure",
                    flush=True,
                )
                break
        training_summary = summarize_runs(training_runs)
        stress_summary = summarize_runs(stress_runs) if stress_runs else None
        valid = training_summary["all_hard_constraints_passed"] and (
            stress_summary is None
            or stress_summary["all_hard_constraints_passed"]
        )
        candidates.append(
            {
                "rank_from_study": rank,
                "trial": candidate_spec["trial"],
                "study_score": candidate_spec["study_score"],
                "params": params,
                "valid": valid,
                "training": training_summary,
                "stress": stress_summary,
            }
        )

    def candidate_key(candidate: Dict[str, Any]):
        stress = candidate["stress"]
        stress_exact = (
            stress["minimum_exact_queries"] if stress is not None else 0
        )
        training = candidate["training"]
        return (
            not candidate["valid"],
            -stress_exact,
            training["median_internal_ra_time_ms"],
            training["p90_compiler_wall_time_ms"],
            training["max_rss_kb"],
        )

    ranked = sorted(candidates, key=candidate_key)
    report = {
        "schema": 1,
        "study": args.study_name,
        "repeats": args.repeats,
        "candidates": ranked,
        "best": ranked[0] if ranked and ranked[0]["valid"] else None,
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(
        json.dumps(report, indent=2, sort_keys=True) + "\n"
    )
    if report["best"] is None:
        print("No candidate passed repeated training and stress validation.")
        return 1
    best_params_path = args.output.with_name("best-params.json")
    best_params_path.write_text(
        json.dumps(report["best"]["params"], indent=2, sort_keys=True) + "\n"
    )
    print(
        f"BEST trial={report['best']['trial']} "
        f"params={report['best']['params']}"
    )
    print(f"REPORT {args.output}")
    print(f"PARAMS {best_params_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
