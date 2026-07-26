# Range Analysis CHIR Refactor Baseline

## Revision

- Branch: `refactor_stage3_20260724`
- Commit: `b5014e3a4e5468911f7366f330ca4d21da685e1a`
- Compiler: Cangjie 1.0.0, x86_64 cjnative
- Binary SHA-256: `3264d884ee0280d31a72f2d4984c9eef3e163dd863178da16dcd665a87ddb81e`
- Date: 2026-07-25

The baseline was compiled with `-O2`. `E` means exact and sound, while `S`
means sound but not exact. Expected values without an existing `expected.txt`
were derived from the query-point semantics and are marked as manual oracles.

## Summary

| Suite | Queries | Exact | Sound | Max RSS |
| --- | ---: | ---: | ---: | ---: |
| Loop and public | 22 | 21 | 22 | 133444 KB |
| Calls | 25 | 19 | 25 | 133536 KB |
| Ref/load/store | 13 | 13 | 13 | 133248 KB |
| Globals | 15 | 8 | 15 | 133928 KB |
| Converted vhscampos | 51 | 51 | 51 | 132896 KB |
| Total | 126 | 112 | 126 | 133928 KB |

Result origins across all queries:

- `CHIRAnalysis`: 103
- `ContextSummary`: 22
- `SourceFallback`: 1 (`global_range_superset`)

The converted-suite runner completed in 12.80 seconds with a process-tree peak
RSS of 132832 KB.

## Loop And Public Cases

| Case | Expected | Actual | Status | Time | RSS |
| --- | --- | --- | --- | ---: | ---: |
| acc_for | `0,1,3 ; 6` | `0,1,3 ; 6` | E | 0.82s | 132608 KB |
| enum_match_filter | `0` | `Int64 Top` | S | 0.60s | 132480 KB |
| if_else_sim | `0,10,21` | `0,10,21` | E | 0.68s | 132480 KB |
| multi_loop_bound | `0..6 ; 0,1,3` | `0..6 ; 0,1,3` | E | 0.73s | 132736 KB |
| multi_loop_like | `0..3 ; 0,1,3` | `0..3 ; 0,1,3` | E | 0.63s | 133444 KB |
| pair_loop | `0,1,2 ; 4,5,6 ; 3 ; 3` | same | E | 0.60s | 131712 KB |
| public | `13,24,40 ; 0,5` | same | E | 0.72s | 132992 KB |
| while_acc_pair | `0..4 ; 0,1,3 ; 0..3 ; 0,1,3,6 ; 4 ; 6` | same | E | 0.60s | 131712 KB |
| while_iv | `-10..-5 ; -5` | same | E | 0.77s | 131968 KB |

## Call Cases

| Case | Expected | Actual | Status | Time | RSS |
| --- | --- | --- | --- | ---: | ---: |
| generic_direct | `7 ; 11` | same | E | 0.66s | 131968 KB |
| ip_chain_like | `1..4 ; 1..4 ; 2,4,6,8` | `1..4 ; 1..4 ; Int64 Top` | S | 0.67s | 133452 KB |
| ip_chain_multifile | `1..4 ; 1..4 ; 2,4,6,8` | `1..4 ; 1..4 ; Int64 Top` | S | 0.68s | 133536 KB |
| ip_chain_three_files | `1..4 ; 1..4 ; 2,4,6,8 ; 1..4 ; 2,4,6,8` | `1..4 ; [-inf,50] ; 2,4,6,8 ; 1..4 ; Int64 Top` | S | 0.67s | 133248 KB |
| ip_chain_while | `1..4 ; 1..4 ; 2,4,6,8` | same | E | 0.67s | 132096 KB |
| lambda_direct | `11 ; 22` | same | E | 0.59s | 132096 KB |
| lambda_mutable_capture | `11 ; 13 ; 13` | same | E | 0.74s | 132224 KB |
| virtual_multi_context | `13 ; 16` | `Int64 Top ; 16` | S | 0.73s | 132224 KB |
| virtual_override | `12 ; 14` | `Int64 Top ; 14` | S | 0.66s | 132528 KB |

## Ref, Load And Store Cases

All 13 queries were exact and sound.

| Case | Expected And Actual | Time | RSS |
| --- | --- | ---: | ---: |
| bool_branch_store | `false,true` | 0.78s | 133248 KB |
| branch_store | `7,11 ; 11` | 0.53s | 132608 KB |
| field_call_read_modify | `5` | 0.60s | 132776 KB |
| field_call_store | `9` | 0.60s | 132608 KB |
| field_store | `9` | 0.80s | 132224 KB |
| loop_store | `2,5,8,11 ; 11` | 0.67s | 132480 KB |
| multiline_bool_ref | `true` | 0.58s | 132096 KB |
| multiline_ref | `37` | 0.69s | 131968 KB |
| shadowed_bool_ref | `false` | 0.66s | 132732 KB |
| straight_store | `10 ; 42` | 0.54s | 131968 KB |

## Global Cases

| Case | Expected | Actual | Status | Time | RSS |
| --- | --- | --- | --- | ---: | ---: |
| global_call | `5` | `5` | E | 0.68s | 132224 KB |
| global_declaration_range | `50,52,54,56,58` | same | E | 0.69s | 133376 KB |
| global_declaration_unreachable | `50,52,54` | same | E | 0.61s | 132864 KB |
| global_direct_store | `5` | `5` | E | 0.63s | 132096 KB |
| global_local_ap | `0,52,54,56` | `0,52,54,56,58` | S | 0.75s | 132608 KB |
| global_local_shadow | `1,2,3,4` | same | E | 0.68s | 132992 KB |
| global_non_declaration_query | `50,52,54,56,58` | same | E | 0.61s | 132992 KB |
| global_param_loop | `0,4` | `0,4,9` | S | 0.63s | 133120 KB |
| global_pre_update | `50,52,54,56` | `50,52,54,56,58` | S | 0.63s | 133928 KB |
| global_pre_update_local | `58` | `58` | E | 0.68s | 133368 KB |
| global_range_superset | `0,52,54,56,58` | `Int64 Top` | S | 0.59s | 133120 KB |
| global_read | `7` | `7` | E | 0.68s | 131968 KB |
| global_read_after_update | `52,54,56,58,60` | `50,52,54,56,58,60` | S | 0.79s | 133120 KB |
| global_read_before_update | `50,52,54,56,58` | `50,52,54,56,58,60` | S | 0.77s | 132992 KB |
| global_return_loop | `0,52,54,56` | `0,52,54,56,58` | S | 0.64s | 132992 KB |

## Converted Vhscampos Cases

All 51 queries were exact and sound.

| Case | Expected And Actual | Time | RSS |
| --- | --- | ---: | ---: |
| goubault_01_simple_inc | `100` | 0.64s | 131840 KB |
| goubault_02_crossing | `9 ; 6` | 0.70s | 132224 KB |
| goubault_03_nonterminating_counter | `2 ; 2` | 0.59s | 132096 KB |
| goubault_04_sequential_loops | `1000 ; 1100 ; -100 ; 1000 ; 0` | 0.64s | 132896 KB |
| goubault_05_nested_decrement | `100 ; 1` | 0.67s | 132096 KB |
| goubault_06_branch_nested | `5 ; 4` | 0.81s | 132352 KB |
| goubault_07_inner_j_growth | `101 ; 20 ; 4` | 0.74s | 132096 KB |
| goubault_08_three_nested | `100 ; 100 ; 100` | 0.60s | 131968 KB |
| goubault_09_outer_nonterminating | `100 ; 1 ; -1 ; 100 ; 1 ; -1` | 0.73s | 131968 KB |
| goubault_10_nested_outer_l | `101 ; 20 ; 4 ; 1020` | 0.73s | 131968 KB |
| vh_t10_two_entry_phi | `0 ; true ; 0` | 0.60s | 131968 KB |
| vh_t1_basic_nested_loop | `100 ; 100` | 0.84s | 132676 KB |
| vh_t2_const_arg | `100 ; 100` | 0.82s | 132480 KB |
| vh_t3_symbolic_arg_as_local | `100 ; 0 ; 100` | 0.65s | 132224 KB |
| vh_t4_mixed_int8_compare | `100 ; 100` | 0.69s | 132096 KB |
| vh_t5_two_args | `100 ; 100` | 0.72s | 132224 KB |
| vh_t6_context_sensitive_calls | `10,100 ; 10 ; 100` | 0.78s | 132224 KB |
| vh_t7_two_deep_call | `0..50 ; 0..49 ; 100 ; 100` | 1.00s | 132096 KB |

## Baseline Risks

1. The query resolver still has one real `SourceFallback` result.
2. `enum_match_filter` loses the value that exists immediately before the
   `match` assignment, indicating a query-to-program-point mapping gap.
3. Cross-file call chains lose return summaries at the final caller query.
4. Resolved virtual calls are context-sensitive only for some call sites.
5. Several global and loop-carried results aggregate one post-update value too
   far, which is sound but violates precise program-point semantics.
6. The core implementation is CHIR-based but relies heavily on independent
   loop-idiom provers and a bounded concrete loop evaluator instead of one
   uniform CFG fixpoint model.

## Verified Changes

### Query Point Versus Same-Line Store

- Root cause: a correct CHIR state observed before a source line was later
  overwritten by the plain `Store` executed on that line.
- Design: ignore a same-line plain Store after a before-point observation, but
  retain CHIR-proven read-modify-write observations.
- Implementation:
  `src/CHIR/Optimization/RangePropagation.cpp:1863`.
- Exact improvements:
  - `enum_match_filter`: `Int64 Top` -> `0`
  - `global_local_ap`: `0,52,54,56,58` -> `0,52,54,56`
  - `global_param_loop`: `0,4,9` -> `0,4`
  - `global_return_loop`: `0,52,54,56,58` -> `0,52,54,56`
- Preserved RMW behavior:
  `global_local_shadow` remains `1,2,3,4`.
- Regression:
  - converted suite: 51/51 exact and sound
  - Ref suite: 13/13 exact and sound
  - public sample unchanged
  - all loop, call, and global cases compiled
- Resource comparison for the converted suite:
  - baseline: 12.80s, 132832 KB Max RSS
  - changed: 11.55s, 133776 KB Max RSS
  - RSS delta: +944 KB (measurement noise scale, still far below 2 GB)

### Unambiguous Global Query Binding

- Root cause: a query on a source line without a CHIR expression beginning on
  that exact line could not bind to its GlobalVar and fell through to
  `SourceFallback`.
- Design: bind only a unique same-file GlobalVar with the requested source
  identifier, and reject the fallback binding when an earlier same-name local
  Debug binding exists. Values still come exclusively from CHIR initializers,
  stores, and context-sensitive call states.
- Implementation:
  `src/CHIR/Optimization/RangePropagation.cpp:972` and package setup in
  `RangePropagation::EmitContestOutput`.
- Improvement:
  `global_range_superset` changed from `SourceFallback / Int64 Top` to
  `ContextSummary / 0,52,54,56,58`.
- No shadowing regression:
  `global_local_shadow` remains `CHIRAnalysis / 1,2,3,4`.
- Stage-2 totals:
  - exact: 117/126 (baseline 112/126)
  - sound: 126/126
  - origins: 104 CHIRAnalysis, 22 ContextSummary, 0 SourceFallback
- Full regression:
  - converted suite: 51/51 exact and sound
  - Ref suite: 13/13 exact and sound
  - all loop, call, and global cases compiled with their prior exact results
    preserved
- Converted-suite resources after both query-mapping changes:
  10.60s, 134500 KB Max RSS.

### Bounded Unsigned Induction And Accumulators

- Root cause: the counted-loop recognizer, small-value extraction, affine
  accumulator builders, and range constructors rejected every unsigned integer
  type before checking whether the concrete values were representable.
- Design: model only UInt values that can be represented losslessly by the
  existing bounded `int64_t` loop formulas, then validate every generated value
  against the actual unsigned bit width. Any wrap, underflow, large UInt64
  value, or non-enumerable bound returns unknown.
- Implementation:
  - modeled constants and width checks at
    `src/CHIR/Analysis/ValueRangeAnalysis.cpp:4828` and `:5104`
  - UInt-aware small exact values at `:6247`
  - signed/unsigned domain construction at `:6914` and `:7124`
  - counted update/body/exit summaries at `:8422`, `:8478`, and `:8619`
- New directed tests:
  - `unsigned_while`: exact `i=0..4`, body `sum=0,1,3`, exit `i=4`,
    `sum=6`
  - `unsigned_overflow_guard`: UInt8 overflow remains the sound full
    `[0,255]` range
- Regression:
  - converted suite: 51/51 exact and sound
  - Ref suite: 13/13 exact and sound
  - every prior loop, call, global, and public output preserved
- Converted-suite resources: 11.99s, 133376 KB Max RSS.

### Stage 3 CFG And Loop Gate

- Added directed CHIR loop coverage for:
  - `break_continue`: exact values across both a continue backedge and a break
    exit;
  - `unsigned_large_exit`: exact body `[0,19999:1]` and exit `20000` after
    exceeding the bounded concrete evaluator's 16384-step budget;
  - `unsigned_overflow_guard`: retained full UInt8 range when wraparound makes
    narrowing unsafe.
- These tests confirm that the abstract CFG/induction path, rather than only
  bounded concrete execution, handles the large loop.
- All directed loop cases are exact; the overflow guard remains sound.

### Reachable Context Result Selection

- Root cause: context aggregation marked a query unknown before checking
  whether root CHIR analysis already had a complete result. This allowed an
  auxiliary Top observation to overwrite an exact root value. Conversely, a
  complete reachable-context result could not refine a coarse root interval.
- Design:
  - a non-global unknown or auxiliary context cannot invalidate a complete
    root CHIR result;
  - a complete reachable-context candidate may refine the root result only
    when its range is provably a subset of the root range;
  - globals continue to join all lifetime/context observations.
- Implementation:
  `src/CHIR/Optimization/RangePropagation.cpp`, helper
  `IsContestRangeSubset` and `ApplyContestContextCandidates`.
- Exact improvements:
  - `ip_chain_multifile`: final `b` changed from Int64 Top to `2,4,6,8`;
  - `ip_chain_three_files`: `y` changed from `[-2^63,50:1]` to `1,2,3,4`,
    and the final result changed from Top to `2,4,6,8`.
- Regression:
  - direct/generic/lambda/call-loop tests remain exact;
  - converted suite: 51/51 exact and sound;
  - current Ref suite: 10/10 exact;
  - converted-suite resources: 10.56s, 134172 KB Max RSS.

### Declaration Point Versus Later Loads

- Root cause: after a declaration Store had produced an exact CHIR result, the
  query mapper continued to aggregate later loads of that binding. A later Top
  load therefore erased an exact value at the requested declaration point.
- Design: once the same-line declaration Store is complete, it is authoritative
  for that source program point. Repeated loop declarations remain aggregated
  by the Store's fixpoint/bounded CHIR observation; unrelated later loads no
  longer participate.
- Implementation:
  `ContestQuery::hasDeclarationStoreResult` and
  `ResolveQueryAtLoadResult` in
  `src/CHIR/Optimization/RangePropagation.cpp`.
- Exact improvements:
  - `virtual_override`: `direct` changed from Top to `12`;
  - `virtual_multi_context`: first direct result changed from Top to `13`.
- The exploratory allocation-site extension was removed after an ablation run
  proved it was not needed.
- Regression:
  - call suite: 8/8 exact (previously 6/8);
  - converted suite: 51/51 exact and sound;
  - Ref suite: 10/10 exact;
  - loop gate: all exact, with the overflow guard sound;
  - converted-suite resources: 10.33s, 133996 KB Max RSS.

### Aliased Reference Arguments In Context Summaries

- Root cause: function contexts captured the abstract contents of each Ref
  argument but not the alias relation between arguments. Two parameters
  receiving the same object were initialized with independent abstract
  objects, and their summaries were then written back sequentially to the same
  caller object. This was unsound and made the answer depend on parameter
  order.
- Directed failure before the fix:
  - the same `Box` was passed as both arguments;
  - the callee stored through the second parameter and then through the first;
  - runtime final field value: `7`;
  - RA result: `1`.
- Design:
  - detect equal caller-side `AbstractObject*` values among Ref arguments;
  - assign a bounded, call-local alias group only to repeated objects;
  - include the alias group in the context cache key;
  - rebind parameters in the same group to one abstract object at callee entry.
  Values and alias relations are derived only from the CHIR abstract state.
- Implementation:
  `ContextAbstractValue::aliasGroup`,
  `RangeAnalysis::HandleContextSensitiveCall`, and
  `RangeAnalysis::InitializeFuncEntryState` in
  `src/CHIR/Analysis/ValueRangeAnalysis.cpp`.
- Directed result after the fix:
  - aliased call: exact final value `7`;
  - same-valued non-aliased call in the same program: exact final values
    `7` and `1`, proving that the two contexts do not collide.
- Regression:
  - call suite including generic static dispatch: 9/9 exact;
  - existing Ref suite: 10/10 exact;
  - converted suite: 51/51 exact and 51/51 sound;
  - global oracle gate: 5/5 exact;
  - loop gate: 4/4;
  - converted-suite resources: 10.19s, 133640 KB Max RSS.

### Compound Mutable Global Side Effects

- Root cause:
  - mutable-global summaries accepted only Bool and integer root types;
  - a global class value has CHIR type `Class&&`, while its Load result is
    `Class&`;
  - treating an unknown compound Load result as a non-reference Top violated
    the abstract-state Ref invariant and caused a later field access to
    dereference an invalid domain.
- Directed failure before the fix:
  a callee stored `9` into `globalBox.value`, but the caller observed Int64
  Top.
- Design:
  - track Bool, integer, Class, Struct, and Tuple roots via
    `RefType::GetRootBaseType`;
  - use the mutable global's existing abstract object as the compound root;
  - bind a compound global Load result to that root object;
  - capture Store inputs, call inputs, and exit summaries through the top-level
    CHIR value so nested fields are retained;
  - return TopRef, rather than scalar Top, for an unknown reference result.
- An initial experiment that created a synthetic two-level Ref was rejected
  after it caused SIGSEGV. The final implementation does not synthesize an
  extra reference and passed the crash regression.
- Implementation:
  `GetTrackedMutableGlobalBaseType`,
  `RangeAnalysis::HandleContextSensitiveCall`,
  `RangeAnalysis::SummarizeGlobalValues`, and the global Load/Store cases in
  `RangeAnalysis::HandleNormalExpressionEffect`, all in
  `src/CHIR/Analysis/ValueRangeAnalysis.cpp`.
- Directed result after the fix:
  `globalBox.value` is exactly `9`.
- Regression:
  - global oracle gate including the compound global: 6/6 exact;
  - call suite: 9/9 exact;
  - aliased argument test: 3/3 exact;
  - existing Ref suite: 10/10 exact;
  - converted suite: 51/51 exact and 51/51 sound;
  - loop gate: 4/4;
  - converted-suite resources: 10.17s, 133672 KB Max RSS.

### Mutable Compound Global Initializers

- Root cause:
  `AnalysisWrapper` runs global initializer functions automatically only for
  `READONLY` globals. A mutable compound global such as `Box(3)` therefore
  entered every RangeAnalysis root as a Top object even though its
  `GlobalVar::GetInitFunc()` contains complete CHIR allocation, constructor,
  and global Store operations.
- Directed failure before the fix:
  reading `globalBox.value` before any ordinary function call produced Int64
  Top instead of the initializer value `3`.
- Design:
  - initialize tracked mutable globals in package initialization order;
  - analyze each `GetInitFunc()` with the existing bounded context-sensitive
    CHIR engine;
  - use the current abstract global state as the initializer context, so an
    initializer can depend on an earlier global;
  - apply every exit global summary, including side effects on globals other
    than the one owning the initializer;
  - do not seed initializers recursively from context analyses or from a global
    initializer root;
  - reuse the existing per-function, global-context, and total-summary budgets
    instead of adding an unbounded cache.
- Implementation:
  `RangeAnalysis::SeedMutableGlobalInitializers` and
  `RangeAnalysis::InitializeFuncEntryState` in
  `src/CHIR/Analysis/ValueRangeAnalysis.cpp`.
- Directed results after the fix:
  - initial `globalBox.value`: exact `3`;
  - callee-updated `globalBox.value`: exact `9`;
  - a `Box(base + 1)` initializer depending on an earlier mutable global:
    exact `5`.
- Regression:
  - global oracle gate: 8/8 exact;
  - call suite: 9/9 exact;
  - Ref suite including aliased arguments: 11/11 exact;
  - loop and public gate: 5/5;
  - converted suite: 51/51 exact and 51/51 sound;
  - result origins: 24 `CHIRAnalysis`, 27 `ContextSummary`, and zero
    `SourceFallback`;
  - converted-suite resources: 9.36s, 134224 KB Max RSS.

### Compound Lambda Arguments And Captures

- Root causes:
  - lambda context eligibility and summaries accepted only scalar parameters
    and captures;
  - a Ref parameter's binding could disappear from the joined exit state even
    though its underlying abstract object remained valid;
  - compound-object capture through the four-argument
    `CaptureContextValue` overload handled only Bool and integer roots;
  - lambda summaries applied the return value before conservative Ref side
    effects, so a later havoc could erase a precise return.
- Design:
  - admit Class, Struct, and Tuple roots into bounded lambda contexts;
  - retain the entry-bound `AbstractObject*` for each Ref parameter and use it
    to build the exit side-effect summary;
  - serialize compound fields in the common object-based context capture path,
    with at most 64 tracked fields;
  - apply Ref, capture, and global side effects before the return summary.
  All values and object identities come from the CHIR abstract state.
- Directed results:
  - object parameter read: exact `5`;
  - object capture read: exact `11`;
  - object parameter mutation return: exact `8`;
  - caller observation after the mutation: exact `7`.
- Regression:
  - call suite including the two new lambda object cases: 11/11 exact;
  - Ref suite: 11/11 exact;
  - global suite: 8/8 exact;
  - loop and public gate: 5/5;
  - converted suite: 51/51 exact and 51/51 sound;
  - result origins: 24 `CHIRAnalysis`, 27 `ContextSummary`, and zero
    `SourceFallback`;
  - converted-suite resources: 8.90s, 134488 KB Max RSS.

### Object-Local Unknown Call Havoc

- Root cause:
  `ForgetReferenceArgument` called `ClearState()` for every Class, Struct, or
  Tuple Ref even when CHIR had already resolved that Ref to a concrete
  `AbstractObject*`. An unresolved virtual call could therefore erase exact
  unrelated locals and loop state.
- Design:
  - if a Ref resolves to an abstract object, set only that object and its
    tracked children to Top;
  - aliases remain sound because they share the same abstract object;
  - if the Ref is TopRef, its object is missing, or its root type is invalid,
    retain the full-state conservative havoc.
- Directed result:
  an unresolved virtual call mutating a `Box` now preserves an unrelated
  `stable=42` local as exact `42`, with origin `CHIRAnalysis`.
- Regression:
  - call suite: 12/12 exact;
  - Ref suite: 11/11 exact;
  - global suite: 8/8 exact;
  - loop and public gate: 5/5;
  - converted suite: 51/51 exact and 51/51 sound;
  - result origins: 24 `CHIRAnalysis`, 27 `ContextSummary`, and zero
    `SourceFallback`;
  - converted-suite resources: 8.62s, 133304 KB Max RSS.

### Bounded Nested Object Contexts

- Root causes:
  - compound context capture serialized only first-level Bool and integer
    fields;
  - a nested Ref field was therefore recorded as Top even when it pointed to a
    tracked Class, Struct, or Tuple object;
  - parent-object reset changed nested Ref fields to TopRef before summary
    writeback, losing their existing abstract-object bindings;
  - unconditionally restoring the old binding was also unsound when the
    callee rebound the field to a newly allocated object.
- Design:
  - recursively capture compound fields from the CHIR object graph;
  - bound traversal to depth 4, 128 object nodes, and 64 fields per object;
  - detect repeated objects and cycles, degrading that path to Top rather than
    duplicating an alias unsafely;
  - attach a bounded object-identity token to each object summary and carry
    that token through reconstructed callee states;
  - join object summaries only when their identities agree; identity
    disagreement across exits degrades the object path to Top;
  - derive child types from CHIR Class/Struct/Tuple types during writeback;
  - preserve a nested Ref object only when its identity matches the summary,
    otherwise rebind the Ref to a fresh bounded abstract object;
  - apply mutable-global summaries through the global Ref itself, so a global
    object initializer or callee can rebind the global without mutating the
    old referent.
- Directed results:
  - a two-level `Outer.inner.value` update returns exact `9`;
  - the caller observes the updated nested field as exact `9`;
  - after rebinding `outer.inner` from a shared object containing `1` to a new
    object containing `9`, the return, old alias, and new field are exactly
    `9`, `1`, and `9`;
  - a pair of fields sharing one nested object degrades to Int64 Top, which
    contains the runtime value `9` and preserves soundness.
- Regression:
  - exact call suite: 14/14;
  - Ref suite: 11/11 exact;
  - global suite: 8/8 exact;
  - loop and public gate: 5/5;
  - converted suite: 51/51 exact and 51/51 sound;
  - result origins: 24 `CHIRAnalysis`, 27 `ContextSummary`, and zero
    `SourceFallback`;
  - converted-suite resources: 8.44s, 134700 KB Max RSS.

### Source Fallback Retirement

- The output layer no longer contains a `SourceFallback` result origin or a
  source-derived value path.
- An unresolved query is represented explicitly as `UnresolvedCHIRTop`; its
  full Bool or integer range is constructed from the mapped CHIR `Type*`.
- `input.txt` file name, line, and variable name remain query-location keys
  only. The implementation does not open `.cj` files or simulate source
  statements.
- Final regression after this cleanup:
  - call suite: 14/14 exact;
  - Ref suite: 11/11 exact;
  - global suite: 8/8 exact;
  - loop and public gate: 5/5;
  - converted suite: 51/51 exact and 51/51 sound;
  - direct `UnresolvedCHIRTop` observations: zero;
  - converted origins: 24 `CHIRAnalysis`, 27 `ContextSummary`;
  - converted-suite resources: 8.24s, 134048 KB Max RSS.
