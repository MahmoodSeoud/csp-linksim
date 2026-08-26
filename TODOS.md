# TODOS

## Port the Gilbert-Elliott burst model into ci_inject_bridge

**What:** The bridge injects i.i.d. loss with mode-4 clustering; the calibrated
Gilbert-Elliott model (marginal `-L`, mean burst `-B`, seed `-S`) exists only in
the csp_loss APM, which drops in-process and therefore cannot produce an
independent per-packet drop log. Add a GE mode to the bridge via `ci_ge` (already
in lib/, already linked by both instruments) so burst-shaped sweeps get the
external drop record.

**Why:** Closes the last gap between the two loss instruments: replayable
measured-shape channels (mined real-pass priors: marginal ~0.78 RT, mean burst
7.8) WITH the drop-log evidence chain the thesis's three-check design prefers.

**Pros:** `ci_ge` reuse means likely <100 lines; hook point is the bridge's
per-frame drop decision (where `$LOSS`/seed are consulted today).
**Cons:** Instrument change after the campaign — sweeps run under it are a new
instrument version and must be labeled as such.

**Context:** Identified 2026-08-07 while moving the RDP board arm from the
bridge to csp_loss; the thesis instrument chapter notes the gap. Deferred by
eng-review D12 (2026-08-10): no remaining thesis experiment needs it, and
instrument churn before submission is pure risk.

**Depends on / blocked by:** Nothing technical; post-thesis.

## Impair the reverse channel in the loss injector

**What:** Add loss and pacing to the reverse direction so acknowledgements and repair
requests are dropped and charged airtime the same way forward data is.

**Why:** `csp_loss` hooks the TX path only (`loss_nexthop`, and every run log says so:
"loss on CAN0 (TX from this node)"). RDP's acknowledgement stream and satdeploy's repair
requests therefore travel a perfect, instant link. The mined pass statistics put real loss
near 47% round-trip. Every airtime figure for the two recovering systems is a lower bound
on a channel that does not exist, and R4/R5 rest on an 11% margin (594.75 s against a
534 s contact). R6, recovery under correlated loss, cannot be evaluated at all while the
feedback path is perfect.

**Pros:** makes the cost figures defensible; makes R6 measurable; turns the
reverse-channel cost argument from assumption into evidence.
**Cons:** an injector change after the campaign, so every existing run becomes a prior
instrument version. It will probably make satdeploy look worse, since repair requests are
exactly what a lossy reverse channel destroys.

**Context:** identified 2026-08-26 during the eng review of `docs/experiment-matrix.md`,
by the outside voice. A reverse hook needs either a second `csp_loss` instance on the
board side or a move to `ci_inject_bridge`, which already sits mid-path and could impair
both directions. That overlaps the existing deferred item above (port Gilbert-Elliott into
the bridge) and both are blocked by the same eng-review D12 position: instrument churn
before submission is pure risk.

**Depends on / blocked by:** nothing technical. Blocked by the submission date. Stated as
a threat to validity in the meantime.

## zmqproxy-lossy aborts under -M with loss (P0)

**Priority:** P0

**What:** `proxy/zmqproxy-lossy` core-dumps intermittently when started with an
identity-keyed drop filter and a nonzero loss rate.

**Repro:** `meson test -C build --print-errorlogs`, test 10 of 14
(`tests/e2e/determinism.sh`). Observed 2026-08-26:

```
zmqproxy-lossy -s tcp://127.0.0.1:<front> -p tcp://127.0.0.1:<back> \
  -M 13 -L 0.3 -S 1 -o log1.csv
-> line 16: Aborted (core dumped)
stdout: 500 / 353 / log1 rows: 0 / log2 rows: 500
```

Run 1 aborted with zero rows written; run 2 of the same invocation completed with
500 rows. Intermittent, so a startup race is more likely than a logic error. The
test's own header already notes the ZMQ slow-joiner drops early frames, so the
startup window is known-fragile.

**Why it matters:** this is `ci_inject_bridge`'s sibling and the injector behind
the committed `board_external_full` campaign, which also ran with `-M`. A crash is
loud, so it cannot have produced wrong data silently, but a reproducibility gate
that crashes is undocumented in the instrument chapter and an examiner running the
suite will hit it.

**Pros:** removes a red test from the suite; closes a crash in a cited instrument.
**Cons:** an afternoon on a code path the 2026-08-26 experiments never used, close
to submission.

**Context:** found by `/ship` on 2026-08-26 on branch
`experiments/2026-08-rebaseline`. The branch changed no C, build or test files, so
the failure is pre-existing. The build already sets ASAN/UBSAN options in the meson
test environment, so a sanitiser rebuild should localise it quickly.

**Depends on / blocked by:** nothing. Not on the path to the T1-T11 tasks from the
eng review.
