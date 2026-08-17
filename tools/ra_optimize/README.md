# Range Analysis Bayesian Tuning

This directory contains an offline Optuna TPE workflow for the eight bounded
resource parameters in `ValueRangeAnalysis.cpp`. It does not participate in a
normal compiler invocation and is not required in a submitted SDK.

## Environment

Run everything in WSL:

```bash
cd <repository>/tools/ra_optimize
python3 -m venv .venv
.venv/bin/pip install -r requirements.txt
```

The compiler accepts the candidate configuration through:

```text
CJ_RA_WIDEN_START
CJ_RA_MAX_INQUEUE
CJ_RA_CONTEXT_PER_FUNCTION
CJ_RA_TOTAL_CONTEXT_SUMMARIES
CJ_RA_CONTEXT_DEPTH
CJ_RA_EXACT_SET_SIZE
CJ_RA_LOOP_OBSERVATIONS
CJ_RA_RECORDED_CONTEXTS
```

Invalid or absent values use the compiled defaults.

## Baseline

The training/regression corpus contains 97 semantic cases and 18 converted
vhscampos cases, for 115 cases and 311 queries:

```bash
.venv/bin/python evaluate.py \
  --suite all \
  --result ../../.ra_optimize/baseline/result.json \
  --log-dir ../../.ra_optimize/baseline/logs
```

The held-out manifest contains 19 parameter-sensitive loop, context, global,
memory, and virtual-dispatch cases. Validate it separately:

```bash
.venv/bin/python evaluate.py \
  --suite stress \
  --result ../../.ra_optimize/stress-baseline/result.json \
  --log-dir ../../.ra_optimize/stress-baseline/logs
```

Result comparison is symbolic: enumerations and `[lower, upper:step]`
progressions are compared as sets without expanding huge integer ranges.
Peak RSS is sampled over the complete compiler process tree.

## Staged TPE Search

Each candidate must preserve every baseline-exact query, remain sound for all
queries, compile successfully, avoid timeouts, and remain below 2 GiB. Valid
candidates minimize a normalized score consisting of internal RA time,
compiler wall time, and peak RSS with default weights 70/25/5.

```bash
.venv/bin/python optimize.py --stage loop --trials 20 \
  --study-name ra-loop

.venv/bin/python optimize.py --stage context --trials 20 \
  --study-name ra-context \
  --fixed-params ../../.ra_optimize/studies/ra-loop/summary.json

.venv/bin/python optimize.py --stage domain --trials 10 \
  --study-name ra-domain \
  --fixed-params ../../.ra_optimize/studies/ra-context/summary.json

.venv/bin/python optimize.py --stage joint --trials 30 \
  --study-name ra-joint \
  --fixed-params ../../.ra_optimize/studies/ra-domain/summary.json
```

Studies are persisted in `.ra_optimize/optuna.db` and can be resumed by using
the same study name. After every trial, the complete effective eight-parameter
configuration and its exactness, soundness, timing, RSS, and rejection reason
are atomically written to `.ra_optimize/studies/<study>/trials.md`.

## Repeated Verification

Re-run the top candidates five times on both training and held-out suites:

```bash
.venv/bin/python verify.py \
  --study-name ra-joint \
  --top 3 \
  --repeats 5 \
  --output ../../.ra_optimize/final-verification.json
```

Ranking uses the held-out exact count, median internal RA time, P90 compiler
wall time, and maximum RSS. Only independently validated parameters should be
made the compiled defaults. Optuna and this virtual environment must not be
included in the competition package.

Combine all staged trials and repeated verification into one Markdown report:

```bash
.venv/bin/python export_studies.py \
  --storage ../../.ra_optimize/optuna.db \
  --study ra-loop \
  --study ra-context \
  --study ra-domain \
  --study ra-joint \
  --verification ../../.ra_optimize/final-verification.json \
  --output ../../.ra_optimize/parameter-search-results.md
```

## Alternating A/B Comparison

Compare a recommended configuration with the compiled defaults over ten
complete training-corpus runs. Odd and even rounds reverse execution order to
reduce order and thermal bias:

```bash
.venv/bin/python compare_configs.py \
  --candidate-params /path/to/candidate-params.json \
  --repeats 10 \
  --output ../../.ra_optimize/config-comparison/result.json \
  --markdown ../../.ra_optimize/config-comparison/report.md
```

The JSON and Markdown reports retain every run plus arithmetic means,
population standard deviations, exactness, soundness, compiler wall time, and
RSS.
