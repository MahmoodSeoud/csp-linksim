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
