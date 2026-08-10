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
