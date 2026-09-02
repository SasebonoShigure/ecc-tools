# Stage-0 full-STA baseline

This directory owns the hermetic CPU/full-STA correctness oracle that later
incremental and GPU implementations must match.  It is intentionally a
test-only harness: it does not add a runtime mode and it does not claim any
incremental execution in stage 0.

## Contract

- `StaFullBaselineProbe.cpp` links the current source tree and runs the
  production `DataManager` and `STAInterface` pipeline.  A previously published
  Python wheel is not an oracle for native changes.
- Each case runs in at least three independent operating-system processes and
  in a distinct output directory.  A successful exit without a complete atomic
  snapshot is a failure.
- `manifest.json` names every LEF, Liberty, Verilog, and SDC input consumed by
  each case. Canonical timing values and SDC numeric values are interpreted as
  nanoseconds.
- Stable semantic names, graph shape, enum dimensions, typed sentinels, and
  configuration fields compare directly. Finite timing and power values compare
  field by field with the manifest's absolute-plus-relative tolerance. Missing
  values and positive/negative infinity are distinct typed sentinels; raw JSON
  NaN/Infinity is forbidden.
- Text artifacts normalize timestamps and run-directory paths, then preserve
  exact text/integer tokens while comparing finite decimal tokens with the same
  tolerance as the snapshot.  This keeps SDF delays, timescales, identifiers,
  bus indices, and report contents in the correctness gate.  Structured JSON is
  compared field by field and non-text binary artifacts use exact SHA-256.
- Ordinary verification is read-only.  Checked-in references change only when
  a maintainer deliberately passes `--record-golden` after fresh-process
  consistency succeeds.

The fixtures cover a serial chain and a reconvergent diamond with positive- and
negative-unate cell arcs.  They exercise max/min, rise/fall, input-constrained
paths, reports, power, and SDF without an external PDK or SPEF.  They do not
cover sequential timing checks, propagated clocks, multi-corner analysis, or
parasitic extraction; those gaps are recorded in the stage-0 report.

## Run locally

The controller requires Python 3.11 or newer.  Both CMake discovery branches
enforce that minimum before registering the tests.

Configure with tests enabled, then build the three explicit stage-0 targets:

```sh
cmake -S . -B build-ista-stage0 -G Ninja \
  -DCMD_BUILD=ON -DBUILD_TESTING=ON -DCMAKE_BUILD_TYPE=Release \
  -DCOMPATIBILITY_MODE=ON
cmake --build build-ista-stage0 --target \
  ista_arc_timing_arc_delay_test \
  ista_graph_builder_state_test \
  ista_full_baseline_probe --parallel 2
ctest --test-dir build-ista-stage0 --no-tests=error \
  --output-on-failure -L '^ista_unit$'
ctest --test-dir build-ista-stage0 --no-tests=error \
  --output-on-failure -L '^ista_smoke$'
```

For the stronger five-process local check:

```sh
python3 src/operation/iSTA/test/baseline/run_baseline.py \
  --probe build-ista-stage0/bin/ista_full_baseline_probe \
  --repeats 5 --output-root build-ista-stage0/stage0-baseline-runs
```

The exact executable location is generator-dependent; use
`find build-ista-stage0 -name ista_full_baseline_probe` if needed.

To intentionally bootstrap or replace the references, use the same current
source executable and add `--record-golden`.  All selected cases finish their
fresh-process checks before any golden is published. Review the complete
snapshot and artifact diff, including structured timing values, tokenized
text, and opaque binary hashes, before accepting a golden update.
