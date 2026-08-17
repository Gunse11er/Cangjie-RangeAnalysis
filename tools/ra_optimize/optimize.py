#!/usr/bin/env python3
"""Run staged Optuna TPE search for Range Analysis resource parameters."""

from __future__ import annotations

import argparse
import json
import re
import time
from pathlib import Path
from typing import Any, Dict, List, Optional, Sequence

import optuna

from evaluate import (
    DEFAULT_CJC,
    PARAM_SPECS,
    REPO_ROOT,
    RegressionCase,
    discover_semantic_cases,
    discover_stress_cases,
    discover_vh_cases,
    evaluate,
    load_params,
)


SEARCH_SPACES = {
    "widen_start": [2, 3, 4, 5, 6, 8],
    "max_inqueue": [16, 24, 32, 48, 64],
    "context_per_function": [8, 16, 24, 32, 48, 64],
    "total_context_summaries": [128, 256, 512, 768, 1024],
    "context_depth": [4, 8, 12, 16, 20, 24],
    "exact_set_size": [8, 16, 32, 64, 96, 128],
    "loop_observations": [512, 1024, 2048, 4096, 8192],
    "recorded_contexts": [1024, 2048, 4096, 8192],
}

STAGE_PARAMS = {
    "loop": ["widen_start", "max_inqueue", "loop_observations"],
    "context": [
        "context_per_function",
        "total_context_summaries",
        "context_depth",
        "recorded_contexts",
    ],
    "domain": ["exact_set_size"],
    "joint": list(SEARCH_SPACES),
}

PARAMETER_ORDER = tuple(SEARCH_SPACES)


def read_json(path: Path) -> Dict[str, Any]:
    return json.loads(path.read_text())


def load_fixed_params(raw: Optional[str]) -> Dict[str, int]:
    if raw is None:
        return load_params(None)
    path = Path(raw)
    data = json.loads(path.read_text() if path.is_file() else raw)
    if isinstance(data, dict) and data.get("best") is not None:
        data = data["best"]["params"]
    return load_params(json.dumps(data))


def sqlite_storage_url(path: Path) -> str:
    return f"sqlite:///{path.resolve()}"


def selected_cases(
    suite: str,
    stress_manifest: Path,
    case_regex: Optional[str],
) -> List[RegressionCase]:
    if suite == "semantic":
        cases = discover_semantic_cases()
    elif suite == "vhscampos":
        cases = discover_vh_cases()
    elif suite == "stress":
        cases = discover_stress_cases(stress_manifest)
    else:
        cases = [*discover_semantic_cases(), *discover_vh_cases()]
    if case_regex:
        pattern = re.compile(case_regex)
        cases = [
            case for case in cases if pattern.search(case.qualified_name)
        ]
    return cases


def metrics_for_cases(
    baseline: Dict[str, Any], cases: Sequence[RegressionCase]
) -> Dict[str, float]:
    wanted = {case.qualified_name for case in cases}
    entries = [
        entry
        for entry in baseline["case_results"]
        if entry["name"] in wanted
    ]
    if len(entries) != len(cases):
        missing = sorted(wanted - {entry["name"] for entry in entries})
        raise ValueError(
            "baseline result does not contain selected cases: "
            + ", ".join(missing)
        )
    return {
        "compiler_wall_time_ms": max(
            1.0, sum(float(entry["wall_time_ms"]) for entry in entries)
        ),
        "internal_ra_time_ms": max(
            1.0,
            sum(float(entry["internal_ra_time_ms"]) for entry in entries),
        ),
        "max_rss_kb": max(
            1.0, max(float(entry["max_rss_kb"]) for entry in entries)
        ),
    }


def penalty(payload: Dict[str, Any], memory_limit_kb: int) -> float:
    missing_sound = payload["total_queries"] - payload["sound_queries"]
    lost_exact = max(
        0, payload["baseline_exact_queries"] - payload["exact_queries"]
    )
    memory_excess = max(0, payload["max_rss_kb"] - memory_limit_kb)
    incomplete_cases = payload["cases_requested"] - payload["cases_completed"]
    return (
        1_000_000.0
        + missing_sound * 100_000.0
        + len(payload["regressed_exact_queries"]) * 50_000.0
        + lost_exact * 10_000.0
        + payload["compile_failures"] * 100_000.0
        + payload["timeouts"] * 100_000.0
        + incomplete_cases * 100_000.0
        + memory_excess
    )


def performance_score(
    payload: Dict[str, Any],
    baseline_metrics: Dict[str, float],
    weights: Sequence[float],
) -> float:
    ra_ratio = (
        payload["internal_ra_time_ms"]
        / baseline_metrics["internal_ra_time_ms"]
    )
    wall_ratio = (
        payload["compiler_wall_time_ms"]
        / baseline_metrics["compiler_wall_time_ms"]
    )
    rss_ratio = payload["max_rss_kb"] / baseline_metrics["max_rss_kb"]
    return (
        weights[0] * ra_ratio
        + weights[1] * wall_ratio
        + weights[2] * rss_ratio
    )


def parse_weights(raw: str) -> List[float]:
    weights = [float(value) for value in raw.split(",")]
    if len(weights) != 3 or any(value < 0 for value in weights):
        raise ValueError("weights must be three non-negative numbers")
    total = sum(weights)
    if total <= 0:
        raise ValueError("at least one score weight must be positive")
    return [value / total for value in weights]


def markdown_escape(value: Any) -> str:
    return str(value).replace("|", r"\|").replace("\n", "<br>")


def rejection_reason(
    payload: Dict[str, Any], memory_limit_kb: int
) -> str:
    reasons: List[str] = []
    regressions = payload.get("regressed_exact_queries", [])
    if regressions:
        reasons.append(
            "exact regression: " + ", ".join(str(item) for item in regressions)
        )
    missing_sound = (
        int(payload.get("total_queries", 0))
        - int(payload.get("sound_queries", 0))
    )
    if missing_sound > 0:
        reasons.append(f"{missing_sound} unsound queries")
    compile_failures = int(payload.get("compile_failures", 0))
    if compile_failures:
        reasons.append(f"{compile_failures} compile failures")
    timeouts = int(payload.get("timeouts", 0))
    if timeouts:
        reasons.append(f"{timeouts} timeouts")
    incomplete = (
        int(payload.get("cases_requested", 0))
        - int(payload.get("cases_completed", 0))
    )
    if incomplete > 0:
        reasons.append(f"{incomplete} incomplete cases")
    max_rss_kb = int(payload.get("max_rss_kb", 0))
    if max_rss_kb > memory_limit_kb:
        reasons.append(
            f"RSS {max_rss_kb} KB exceeds {memory_limit_kb} KB"
        )
    if not reasons and not payload.get("hard_constraints_passed", False):
        reasons.append("hard constraint failed")
    return "; ".join(reasons) if reasons else "-"


def full_trial_params(
    trial: optuna.trial.FrozenTrial, base_params: Dict[str, int]
) -> Dict[str, int]:
    stored = trial.user_attrs.get("full_params")
    if isinstance(stored, dict):
        return {
            name: int(stored.get(name, base_params[name]))
            for name in PARAMETER_ORDER
        }
    params = dict(base_params)
    params.update({name: int(value) for name, value in trial.params.items()})
    return params


def write_study_markdown(
    study: optuna.Study,
    path: Path,
    *,
    stage: str,
    seed: int,
    base_params: Dict[str, int],
    baseline_metrics: Dict[str, float],
    case_count: int,
    query_count: int,
) -> None:
    lines = [
        f"# Range Analysis Parameter Search: {study.study_name}",
        "",
        "This file is rewritten atomically after every completed trial.",
        "",
        f"- Stage: `{stage}`",
        f"- Seed: `{seed}`",
        f"- Cases: `{case_count}`",
        f"- Queries: `{query_count}`",
        (
            "- Baseline: "
            f"RA `{baseline_metrics['internal_ra_time_ms']:.3f} ms`, "
            f"compiler wall "
            f"`{baseline_metrics['compiler_wall_time_ms'] / 1000.0:.3f} s`, "
            f"RSS `{int(baseline_metrics['max_rss_kb'])} KB`"
        ),
        "",
        (
            "| Trial | Status | Score | Exact | Sound | RA ms | Wall s | "
            "RSS KB | Widen | Inqueue | Context/Func | Total Summaries | "
            "Depth | Exact Set | Loop Obs | Recorded Contexts | "
            "Reason | Result |"
        ),
        (
            "|---:|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|"
            "---:|---:|---:|---:|---:|---|---|"
        ),
    ]
    for trial in sorted(study.trials, key=lambda item: item.number):
        attrs = trial.user_attrs
        params = full_trial_params(trial, base_params)
        state = trial.state.name
        if state == "COMPLETE":
            status = "VALID" if attrs.get("valid") else "REJECTED"
        else:
            status = state
        score = "" if trial.value is None else f"{float(trial.value):.6f}"
        queries = int(attrs.get("queries", 0))
        exact = (
            f"{int(attrs.get('exact', 0))}/{queries}" if queries else ""
        )
        sound = (
            f"{int(attrs.get('sound', 0))}/{queries}" if queries else ""
        )
        ra_ms = attrs.get("internal_ra_time_ms")
        wall_ms = attrs.get("compiler_wall_time_ms")
        rss_kb = attrs.get("max_rss_kb")
        row = [
            trial.number,
            status,
            score,
            exact,
            sound,
            "" if ra_ms is None else f"{float(ra_ms):.3f}",
            "" if wall_ms is None else f"{float(wall_ms) / 1000.0:.3f}",
            "" if rss_kb is None else int(rss_kb),
            params["widen_start"],
            params["max_inqueue"],
            params["context_per_function"],
            params["total_context_summaries"],
            params["context_depth"],
            params["exact_set_size"],
            params["loop_observations"],
            params["recorded_contexts"],
            attrs.get("rejection_reason", "-"),
            attrs.get("result", ""),
        ]
        lines.append(
            "| " + " | ".join(markdown_escape(value) for value in row) + " |"
        )
    lines.extend(
        [
            "",
            "## Base Parameters",
            "",
            "```json",
            json.dumps(base_params, indent=2, sort_keys=True),
            "```",
            "",
        ]
    )
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text("\n".join(lines))
    temporary.replace(path)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--stage", choices=tuple(STAGE_PARAMS), default="joint"
    )
    parser.add_argument("--trials", type=int, default=20)
    parser.add_argument("--seed", type=int, default=20260729)
    parser.add_argument("--study-name")
    parser.add_argument(
        "--storage",
        type=Path,
        default=REPO_ROOT / ".ra_optimize/optuna.db",
    )
    parser.add_argument(
        "--baseline-result",
        type=Path,
        default=REPO_ROOT / ".ra_optimize/baseline-v2/result.json",
    )
    parser.add_argument("--fixed-params")
    parser.add_argument("--cjc", type=Path, default=DEFAULT_CJC)
    parser.add_argument(
        "--suite",
        choices=("all", "semantic", "vhscampos", "stress"),
        default="all",
    )
    parser.add_argument(
        "--stress-manifest",
        type=Path,
        default=Path(__file__).resolve().parent / "stress_manifest.json",
    )
    parser.add_argument("--case-regex")
    parser.add_argument("--case-timeout", type=float, default=30.0)
    parser.add_argument("--overall-timeout", type=float, default=180.0)
    parser.add_argument("--timeout-hours", type=float)
    parser.add_argument("--memory-limit-kb", type=int, default=2 * 1024 * 1024)
    parser.add_argument(
        "--weights",
        default="0.70,0.25,0.05",
        help="normalized weights for RA time, compiler wall time, and RSS",
    )
    parser.add_argument(
        "--markdown",
        type=Path,
        help="per-trial Markdown report (default: study directory/trials.md)",
    )
    parser.add_argument("--fail-fast", action="store_true")
    args = parser.parse_args()

    if args.trials <= 0:
        parser.error("--trials must be positive")
    baseline = read_json(args.baseline_result)
    base_params = load_fixed_params(args.fixed_params)
    cases = selected_cases(
        args.suite, args.stress_manifest, args.case_regex
    )
    if not cases:
        parser.error("no cases selected")
    baseline_metrics = metrics_for_cases(baseline, cases)
    weights = parse_weights(args.weights)
    study_name = args.study_name or f"ra-{args.stage}"
    args.storage.parent.mkdir(parents=True, exist_ok=True)
    run_root = REPO_ROOT / ".ra_optimize/studies" / study_name
    run_root.mkdir(parents=True, exist_ok=True)
    markdown_path = args.markdown or run_root / "trials.md"

    sampler = optuna.samplers.TPESampler(
        seed=args.seed,
        n_startup_trials=min(10, max(3, args.trials // 3)),
        multivariate=True,
        group=True,
    )
    study = optuna.create_study(
        study_name=study_name,
        storage=sqlite_storage_url(args.storage),
        load_if_exists=True,
        direction="minimize",
        sampler=sampler,
    )
    if not study.trials:
        study.enqueue_trial(
            {name: base_params[name] for name in STAGE_PARAMS[args.stage]}
        )

    def objective(trial: optuna.Trial) -> float:
        params = dict(base_params)
        for name in STAGE_PARAMS[args.stage]:
            params[name] = trial.suggest_categorical(
                name, SEARCH_SPACES[name]
            )
        trial.set_user_attr("full_params", params)
        trial.set_user_attr("stage", args.stage)
        if params["max_inqueue"] <= params["widen_start"]:
            trial.set_user_attr("valid", False)
            trial.set_user_attr(
                "rejection_reason", "max_inqueue must exceed widen_start"
            )
            return 10_000_000.0

        trial_root = run_root / f"trial-{trial.number:04d}"
        result_path = trial_root / "result.json"
        logs = trial_root / "logs"
        started = time.monotonic()
        payload = evaluate(
            cases=cases,
            cjc=args.cjc,
            params=params,
            result_path=result_path,
            log_root=logs,
            case_timeout=args.case_timeout,
            overall_timeout=args.overall_timeout,
            fail_fast=args.fail_fast,
            baseline=baseline,
            progress=False,
        )
        duration = time.monotonic() - started
        valid = (
            payload["hard_constraints_passed"]
            and payload["max_rss_kb"] <= args.memory_limit_kb
        )
        score = (
            performance_score(payload, baseline_metrics, weights)
            if valid
            else penalty(payload, args.memory_limit_kb)
        )
        trial.set_user_attr("valid", valid)
        trial.set_user_attr("result", str(result_path))
        trial.set_user_attr("exact", payload["exact_queries"])
        trial.set_user_attr("sound", payload["sound_queries"])
        trial.set_user_attr("queries", payload["total_queries"])
        trial.set_user_attr(
            "compiler_wall_time_ms", payload["compiler_wall_time_ms"]
        )
        trial.set_user_attr(
            "internal_ra_time_ms", payload["internal_ra_time_ms"]
        )
        trial.set_user_attr("max_rss_kb", payload["max_rss_kb"])
        trial.set_user_attr("duration_s", duration)
        trial.set_user_attr(
            "hard_constraints_passed",
            payload["hard_constraints_passed"],
        )
        trial.set_user_attr(
            "compile_failures", payload["compile_failures"]
        )
        trial.set_user_attr("timeouts", payload["timeouts"])
        trial.set_user_attr(
            "cases_requested", payload["cases_requested"]
        )
        trial.set_user_attr(
            "cases_completed", payload["cases_completed"]
        )
        trial.set_user_attr(
            "regressed_exact_queries",
            payload["regressed_exact_queries"],
        )
        trial.set_user_attr(
            "rejection_reason",
            "-" if valid else rejection_reason(payload, args.memory_limit_kb),
        )
        print(
            f"TRIAL {trial.number} valid={valid} score={score:.6f} "
            f"exact={payload['exact_queries']}/{payload['total_queries']} "
            f"sound={payload['sound_queries']}/{payload['total_queries']} "
            f"ra_ms={payload['internal_ra_time_ms']:.3f} "
            f"wall_s={payload['compiler_wall_time_ms'] / 1000.0:.3f} "
            f"rss_kb={payload['max_rss_kb']} params={params}",
            flush=True,
        )
        return score

    def persist_trial(
        current_study: optuna.Study,
        _trial: optuna.trial.FrozenTrial,
    ) -> None:
        write_study_markdown(
            current_study,
            markdown_path,
            stage=args.stage,
            seed=args.seed,
            base_params=base_params,
            baseline_metrics=baseline_metrics,
            case_count=len(cases),
            query_count=sum(len(case.expected) for case in cases),
        )

    timeout = (
        None
        if args.timeout_hours is None
        else max(0.0, args.timeout_hours * 3600.0)
    )
    study.optimize(
        objective,
        n_trials=args.trials,
        timeout=timeout,
        gc_after_trial=True,
        show_progress_bar=False,
        callbacks=[persist_trial],
    )
    persist_trial(study, study.trials[-1])

    complete = [
        trial
        for trial in study.trials
        if trial.state == optuna.trial.TrialState.COMPLETE
    ]
    valid = [trial for trial in complete if trial.user_attrs.get("valid")]
    ranked = sorted(valid, key=lambda trial: float(trial.value))
    report = {
        "schema": 1,
        "study": study.study_name,
        "stage": args.stage,
        "storage": str(args.storage.resolve()),
        "cases": len(cases),
        "queries": sum(len(case.expected) for case in cases),
        "base_params": base_params,
        "baseline_metrics": baseline_metrics,
        "weights": weights,
        "trials_complete": len(complete),
        "trials_valid": len(valid),
        "trials_markdown": str(markdown_path.resolve()),
        "best": None,
        "top_trials": [],
    }
    for trial in ranked[:10]:
        params = dict(base_params)
        params.update(trial.params)
        entry = {
            "number": trial.number,
            "score": trial.value,
            "params": params,
            "metrics": trial.user_attrs,
        }
        report["top_trials"].append(entry)
    if report["top_trials"]:
        report["best"] = report["top_trials"][0]
    report_path = run_root / "summary.json"
    report_path.write_text(
        json.dumps(report, indent=2, sort_keys=True) + "\n"
    )
    if not ranked:
        print("No candidate satisfied all hard constraints.")
        return 1
    best = report["best"]
    improvement = (1.0 - float(best["score"])) * 100.0
    print(
        f"BEST trial={best['number']} score={best['score']:.6f} "
        f"weighted_improvement={improvement:.2f}% "
        f"params={best['params']}"
    )
    print(f"REPORT {report_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
