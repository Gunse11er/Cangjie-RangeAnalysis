#!/usr/bin/env python3
"""Export complete parameter-level Optuna results to one Markdown report."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
from typing import Any, Dict, List, Optional

import optuna

from evaluate import REPO_ROOT, load_params
from optimize import PARAMETER_ORDER, markdown_escape, sqlite_storage_url


def read_json(path: Optional[Path]) -> Optional[Dict[str, Any]]:
    if path is None or not path.exists():
        return None
    return json.loads(path.read_text())


def trial_params(
    trial: optuna.trial.FrozenTrial, defaults: Dict[str, int]
) -> Dict[str, int]:
    stored = trial.user_attrs.get("full_params")
    if isinstance(stored, dict):
        return {
            name: int(stored.get(name, defaults[name]))
            for name in PARAMETER_ORDER
        }
    result_path = trial.user_attrs.get("result")
    if result_path:
        payload = read_json(Path(result_path))
        if payload and isinstance(payload.get("params"), dict):
            return {
                name: int(payload["params"].get(name, defaults[name]))
                for name in PARAMETER_ORDER
            }
    params = dict(defaults)
    params.update({name: int(value) for name, value in trial.params.items()})
    return params


def metric(attrs: Dict[str, Any], name: str, digits: int = 3) -> str:
    value = attrs.get(name)
    return "" if value is None else f"{float(value):.{digits}f}"


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def robust_verification_score(
    candidate: Dict[str, Any], defaults: Dict[str, Any]
) -> float:
    candidate_training = candidate["training"]
    default_training = defaults["training"]
    return (
        0.50
        * candidate_training["median_internal_ra_time_ms"]
        / default_training["median_internal_ra_time_ms"]
        + 0.20
        * candidate_training["p90_internal_ra_time_ms"]
        / default_training["p90_internal_ra_time_ms"]
        + 0.15
        * candidate_training["median_compiler_wall_time_ms"]
        / default_training["median_compiler_wall_time_ms"]
        + 0.10
        * candidate_training["p90_compiler_wall_time_ms"]
        / default_training["p90_compiler_wall_time_ms"]
        + 0.05
        * candidate_training["max_rss_kb"]
        / default_training["max_rss_kb"]
    )


def render_verification(report: Dict[str, Any]) -> List[str]:
    lines = [
        "## Repeated Verification",
        "",
        (
            "| Rank | Source Trial | Valid | Train Exact | Train Sound | "
            "Stress Exact | Stress Sound | Median RA ms | P90 RA ms | "
            "Median Wall s | P90 Wall s | Max RSS KB | Parameters |"
        ),
        (
            "|---:|---|---|---:|---:|---:|---:|---:|---:|---:|---:|"
            "---:|---|"
        ),
    ]
    for rank, candidate in enumerate(report.get("candidates", []), start=1):
        training = candidate["training"]
        stress = candidate.get("stress") or {}
        params = ", ".join(
            f"{name}={candidate['params'][name]}" for name in PARAMETER_ORDER
        )
        row = [
            rank,
            candidate["trial"],
            candidate["valid"],
            training["minimum_exact_queries"],
            training["minimum_sound_queries"],
            stress.get("minimum_exact_queries", ""),
            stress.get("minimum_sound_queries", ""),
            f"{training['median_internal_ra_time_ms']:.3f}",
            f"{training['p90_internal_ra_time_ms']:.3f}",
            f"{training['median_compiler_wall_time_ms'] / 1000.0:.3f}",
            f"{training['p90_compiler_wall_time_ms'] / 1000.0:.3f}",
            training["max_rss_kb"],
            params,
        ]
        lines.append(
            "| " + " | ".join(markdown_escape(value) for value in row) + " |"
        )
    lines.append("")
    candidates = report.get("candidates", [])
    defaults = next(
        (
            candidate
            for candidate in candidates
            if candidate.get("trial") == "compiled-defaults"
        ),
        None,
    )
    if defaults is not None:
        comparable = [
            candidate
            for candidate in candidates
            if candidate.get("valid")
            and candidate["training"]["minimum_exact_queries"]
            >= defaults["training"]["minimum_exact_queries"]
            and candidate["training"]["minimum_sound_queries"]
            >= defaults["training"]["minimum_sound_queries"]
            and (candidate.get("stress") or {}).get(
                "minimum_exact_queries", 0
            )
            >= (defaults.get("stress") or {}).get(
                "minimum_exact_queries", 0
            )
            and (candidate.get("stress") or {}).get(
                "minimum_sound_queries", 0
            )
            >= (defaults.get("stress") or {}).get(
                "minimum_sound_queries", 0
            )
        ]
        for candidate in comparable:
            candidate["_robust_score"] = robust_verification_score(
                candidate, defaults
            )
        robust = min(
            comparable,
            key=lambda candidate: float(candidate["_robust_score"]),
            default=None,
        )
        if robust is not None:
            training = robust["training"]
            default_training = defaults["training"]
            median_ra_improvement = (
                1.0
                - training["median_internal_ra_time_ms"]
                / default_training["median_internal_ra_time_ms"]
            ) * 100.0
            p90_ra_improvement = (
                1.0
                - training["p90_internal_ra_time_ms"]
                / default_training["p90_internal_ra_time_ms"]
            ) * 100.0
            median_wall_improvement = (
                1.0
                - training["median_compiler_wall_time_ms"]
                / default_training["median_compiler_wall_time_ms"]
            ) * 100.0
            p90_wall_improvement = (
                1.0
                - training["p90_compiler_wall_time_ms"]
                / default_training["p90_compiler_wall_time_ms"]
            ) * 100.0
            lines.extend(
                [
                    "### Stable Recommendation",
                    "",
                    (
                        "The robust score uses normalized training metrics "
                        "with weights 50% median RA, 20% P90 RA, 15% median "
                        "compiler wall, 10% P90 compiler wall, and 5% max RSS. "
                        "Correctness is a hard gate before scoring."
                    ),
                    "",
                    f"- Recommended source trial: `{robust['trial']}`",
                    f"- Robust score versus defaults: "
                    f"`{robust['_robust_score']:.6f}`",
                    f"- Median RA improvement: "
                    f"`{median_ra_improvement:.2f}%`",
                    f"- P90 RA improvement: `{p90_ra_improvement:.2f}%`",
                    f"- Median compiler-wall improvement: "
                    f"`{median_wall_improvement:.2f}%`",
                    f"- P90 compiler-wall improvement: "
                    f"`{p90_wall_improvement:.2f}%`",
                    f"- Max RSS: `{training['max_rss_kb']} KB` "
                    f"(defaults `{default_training['max_rss_kb']} KB`)",
                    "",
                    "```json",
                    json.dumps(robust["params"], indent=2, sort_keys=True),
                    "```",
                    "",
                ]
            )
    return lines


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--storage",
        type=Path,
        default=REPO_ROOT / ".ra_optimize/optuna.db",
    )
    parser.add_argument(
        "--study",
        action="append",
        required=True,
        help="study name; repeat in desired report order",
    )
    parser.add_argument(
        "--defaults",
        type=Path,
        default=REPO_ROOT / ".ra_optimize/baseline-v2/result.json",
    )
    parser.add_argument("--verification", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--title", default="Range Analysis Parameter Search")
    parser.add_argument("--seed", type=int)
    parser.add_argument(
        "--cjc",
        type=Path,
        default=REPO_ROOT / "output/bin/cjc.real",
    )
    args = parser.parse_args()

    baseline = json.loads(args.defaults.read_text())
    defaults = load_params(json.dumps(baseline["params"]))
    lines = [
        f"# {args.title}",
        "",
        "Every row below is one completed or attempted Optuna trial. "
        "All eight effective parameters are shown, including fixed parameters "
        "in staged searches.",
        "",
        "## Experiment",
        "",
        f"- Optuna storage: `{args.storage.resolve()}`",
        f"- Compiler: `{args.cjc.resolve()}`",
        f"- Compiler SHA-256: `{sha256(args.cjc)}`",
        *([f"- TPE seed: `{args.seed}`"] if args.seed is not None else []),
        "- Training/regression corpus: `115 cases / 311 queries`",
        "- Held-out stress corpus: `19 cases / 53 queries`",
        "- Hard limits: `2 GiB RSS`, `180 s overall evaluation timeout`",
        "",
        "## Compiled Defaults",
        "",
        "```json",
        json.dumps(defaults, indent=2, sort_keys=True),
        "```",
        "",
    ]
    total_trials = 0
    total_valid = 0
    for study_name in args.study:
        study = optuna.load_study(
            study_name=study_name,
            storage=sqlite_storage_url(args.storage),
        )
        trials = sorted(study.trials, key=lambda item: item.number)
        total_trials += len(trials)
        valid_count = sum(
            trial.state == optuna.trial.TrialState.COMPLETE
            and bool(trial.user_attrs.get("valid"))
            for trial in trials
        )
        total_valid += valid_count
        stage = next(
            (
                str(trial.user_attrs["stage"])
                for trial in trials
                if "stage" in trial.user_attrs
            ),
            "unknown",
        )
        lines.extend(
            [
                f"## {study_name}",
                "",
                f"- Stage: `{stage}`",
                f"- Trials: `{len(trials)}`",
                f"- Valid: `{valid_count}`",
                "",
                (
                    "| Trial | Status | Score | Exact | Sound | RA ms | "
                    "Wall s | RSS KB | Widen | Inqueue | Context/Func | "
                    "Total Summaries | Depth | Exact Set | Loop Obs | "
                    "Recorded Contexts | Reason | Result |"
                ),
                (
                    "|---:|---|---:|---:|---:|---:|---:|---:|---:|---:|"
                    "---:|---:|---:|---:|---:|---:|---|---|"
                ),
            ]
        )
        for trial in trials:
            attrs = trial.user_attrs
            params = trial_params(trial, defaults)
            if trial.state == optuna.trial.TrialState.COMPLETE:
                status = "VALID" if attrs.get("valid") else "REJECTED"
            else:
                status = trial.state.name
            queries = int(attrs.get("queries", 0))
            score = (
                "" if trial.value is None else f"{float(trial.value):.6f}"
            )
            row = [
                trial.number,
                status,
                score,
                (
                    f"{int(attrs.get('exact', 0))}/{queries}"
                    if queries
                    else ""
                ),
                (
                    f"{int(attrs.get('sound', 0))}/{queries}"
                    if queries
                    else ""
                ),
                metric(attrs, "internal_ra_time_ms"),
                (
                    ""
                    if attrs.get("compiler_wall_time_ms") is None
                    else f"{float(attrs['compiler_wall_time_ms']) / 1000.0:.3f}"
                ),
                attrs.get("max_rss_kb", ""),
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
                "| "
                + " | ".join(markdown_escape(value) for value in row)
                + " |"
            )
        valid_trials = [
            trial
            for trial in trials
            if trial.state == optuna.trial.TrialState.COMPLETE
            and trial.user_attrs.get("valid")
        ]
        if valid_trials:
            best = min(valid_trials, key=lambda trial: float(trial.value))
            params = trial_params(best, defaults)
            lines.extend(
                [
                    "",
                    f"Best valid trial: `{best.number}` with score "
                    f"`{float(best.value):.6f}`.",
                    "",
                    "```json",
                    json.dumps(params, indent=2, sort_keys=True),
                    "```",
                ]
            )
        lines.append("")

    verification = read_json(args.verification)
    if verification is not None:
        lines.extend(render_verification(verification))
    lines.extend(
        [
            "## Search Totals",
            "",
            f"- Trials: `{total_trials}`",
            f"- Valid trials: `{total_valid}`",
            f"- Rejected or incomplete trials: `{total_trials - total_valid}`",
            "",
        ]
    )
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text("\n".join(lines))
    print(f"REPORT {args.output.resolve()}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
