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

## Deferred from the August 2026 experiment matrix

Deferred from `docs/experiment-matrix.md` by `/ship` on 2026-08-26. The core matrix
(three systems, three loss levels, three seeds, 25 valid runs of 28) is complete and
landed; these are the edges.

### Median-module runs never attempted (P1)

**Priority:** P1

**What:** run DTP and satdeploy against `libcolor.so` (365 688 B, the median flight
module) at iid 0.30, seed 1. The plan's Spot runs section lists all three systems;
only the reliable path was attempted.

**Why:** tuned RDP fits inside one 8.9-minute contact at the 67 KB artifact
(365.6 s against 534 s), so R4 and R5 do not bite at the small artifact. The median
module is where the contact boundary is actually crossed, which makes these runs
load-bearing for two requirements rather than the "optional" the plan calls them.

**Depends on / blocked by:** the satdeploy run is blocked by T5 from the eng review
(determine whether the `-m 256` pin, against satdeploy's own default of 1024, is
what caused the three 0.30 failures). Running it at the wrong MTU would repeat the
same confound at a larger size. DTP is unblocked, ~12 min of bench.

### RDP median-module control failed (P0)

**Priority:** P0

**What:** the zero-loss control for the `libcolor.so` runs came back FAILED with a
loud abort, and the 0.30 run beneath it was still recorded OK.

**Why:** `docs/experiment-matrix.md` line 97 says a control must pass before any
lossy run in that experiment counts. Keeping both the rule and the row is worse than
keeping either alone.

**Context:** root-caused during the eng review. `scripts/rdp-board-sweep` hardcodes
`-t 10000` on the pre-fill and the read-back download while `UP_TO` scales with
artifact size, so at 365 688 B the pre-fill timed out at 190464 bytes, left a
half-open RDP connection, and the payload upload then died at 18048 bytes at zero
loss. One-line fix, tracked as T1.

**Depends on / blocked by:** T1. After the fix, re-run control + stock + tuned,
about 90 min of bench.

### Gilbert-Elliott runs not started (P1)

**Priority:** P1

**What:** two burst-channel runs at seed 1, for the reliable path and satdeploy,
replaying the measured link shape (`-B 7.8`).

**Why:** R6 (recovery must work under correlated loss, not only isolated drops) has
no evidence at all without them, and they are what ties the Link Characterisation
chapter to the loss-injection chapter.

**Depends on / blocked by:** the uplink one-way loss marginal from Link
Characterisation, which does not exist yet and is marked `[VERIFY]` in the plan. The
round-trip figure (~47%) is the wrong number for a forward-only injector. Also note
the eng review's finding that a lossless reverse channel makes R6 unevaluable
regardless until the reverse-channel item above is addressed.

## Harness hardening: the long tail from the ship coverage audit (P2)

**Priority:** P2

**What:** the smaller unguarded paths the 2026-08-26 coverage audit found, none of
which produced a wrong number yet but each of which can.

- `run-dtp-experiment` entry parse: an argument without a colon sets `seed:=loss`
  silently, producing a label like `dtp_L0.30_s0.30`.
- `gs_count` skips processes whose `/proc/PID/exe` is unreadable (a gs-server owned by
  another user), so the "exactly one server" verdict can undercount.
- An all-SKIP sweep writes zero CSV rows and still exits 0.
- `add-dtp-ground-claim` couples to the `/tmp/cell_*` prefix left by the
  `csh-loss-cell` -> `dtp-experiment` rename. Correct today (9 logs matched) but a
  silent `claim=none` on every row if it drifts. Its `head -1` over the claim patterns
  also makes R2's evidence ordering-dependent.
- `add-wallclock` treats dots in labels as regex wildcards, and gives duplicate labels
  the same START/DONE pair.
- The `drops` regex in `rdp-board-sweep` survives only because `csp_loss_apm.c:435`
  prints a comma between `offered` and `dropped`. Without it the leftmost alternation
  captures **offered** as the drop count.
- Every post-processor appends duplicate columns when re-run. `fix-dtp-verdicts` and
  `fill-satdeploy-verdicts` were made idempotent; `add-*` and `classify-*` were not.
- `run-satdeploy-experiment` leaks its `mktemp` init file on timeout (no trap) and
  discards the agent-relaunch sentinel to `/dev/null` without checking it.

**Why:** each is the same shape as the six faults that cost a day: a setting that is
silently wrong, producing output that looks like a result. None is urgent on its own.

**Pros:** removes the remaining ways the harness can lie quietly.
**Cons:** eight small edits with no single failing case driving them, close to
submission.

**Context:** found by the `/ship` coverage audit on 2026-08-26, which measured 0%
automated coverage of the harness and 34 untested branch clusters. The audit's five
highest-value test groups are tracked as T8 in the eng review; only the differential
calibrator test was built (`tests/calibrator_diff.sh`, 60 configurations).

**Depends on / blocked by:** nothing. Independent of T1-T11.

## Harness: remaining adversarial-review findings (P1)

**Priority:** P1

Found by the `/ship` adversarial pass on 2026-08-26, after the coverage audit's ten were
fixed. These are the ones not yet addressed.

- **Console canary has no per-run nonce, and `head -1` prefers stale output.**
  `board-cmd` drains the serial buffer for only 0.2 s and then prints every line up to its
  own sentinel, so leftover output from the previous probe comes FIRST. All three callers
  take `head -1`. A stale canary is a well-formed 32-hex hash from the PREVIOUS
  destination and would be recorded as this run's external verdict. `dtp-experiment`'s own
  comment already documents that the third consecutive lossy run loses the console. Fix:
  embed the run label inside the canary and match on it, or use `tail -1`.
- **`crc32` is collected and then ignored.** The sweep header promises "clean crc32 AND
  download-back sha256", but the verdict ladder branches only on claimed/sha. So
  `crc=SUCCESS, sha=MISMATCH` records SILENT_CORRUPTION even though the board's own CRC
  says the region is correct, and an empty `got_sha` from a failed download does the same.
  Add crc to the ladder and size-check `$GOT`.
- **The pre-fill gate does not detect a failed pre-fill.** It accepts any `Failure` on the
  first crc32, and an aborted pre-fill yields CRC 0, which is a Failure. The libcolor
  control passed this gate with the region half-written, so the run's initial state was
  unknown rather than established.
- **`run-satdeploy-experiment` discards the agent-relaunch confirmation.** The command
  ends in `echo AGT_$(pidof ...)_END` but the whole call is `>/dev/null 2>&1` and
  `board-cmd`'s exit status is unchecked. That is how `recovery_L0.30_s3` was produced,
  leaving 0.30 at n=2.
- **`invocations` is empty in all nine satdeploy rows.** The `(round|pass|invocation)`
  regex never matched, so satdeploy's recovery-round count -- its distinguishing mechanism
  and the RQ3 number -- is absent from the capture. Recover it by counting
  `svu-server: served` lines.
- **`add-wallclock` can pair a late START with an early DONE.** It takes `tail -1` of each
  independently from a log that accumulates across invocations, with no ordering or
  non-negativity check, and interpolates the label into a regex unescaped.
- **`fill-satdeploy-verdicts` derives the reference hash from an untracked payload copy**
  rather than from the `source_md5` already in each row. Regenerate the copy and every
  verdict silently flips against a different artifact.
- **Two remaining non-idempotent post-processors:** `add-wallclock` and
  `add-dtp-ground-claim` still append columns unconditionally.
- **`captures/evidence/**/*.bin` is still ignored** outside the two negated directories, so
  the corrupt-download forensics `rdp-board-sweep` deliberately preserves can never be
  committed. Silently, with no error.
- **The calibrator test cannot see the flow-index path.** `calibrator_host` feeds
  `ci_rule_decide` a loop counter; the injector derives its index through
  `ci_dtp_fragment_index_ovh(off, mtu, overhead)`, which is exactly where the discarded
  set went wrong. The `-M 8 -m 256 -O 4` runtime arguments are pinned nowhere.
- **`python3` absent makes the calibrator test a meson SKIP** while the suite still
  reports success and the README claims 15/15 green.

**Context:** the adversarial pass verified three claims that turned out to be real and are
now fixed (missing pacing guard in the sweep, prefill time attributed to failed runs, and
two false statements in documents). These are what remains.
