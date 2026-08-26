# Experiment matrix, August 2026 rebaseline

**Decision (author, 2026-08-25).** The 256 KiB synthetic artifact is retired. Every
experiment is re-measured against a real DISCO-2 flight module.

Vocabulary per revision brief §12. A **run** is one transfer at one fully specified setting: build, artifact, loss level, seed, rate, target node.
An **experiment** is one build across every loss level and seed.

## The narrative

The experiments run in two parts, and the first part produces the input to the second.

**Part 1, the baselines.** DTP and RDP are measured under loss. Each failure mode they
expose becomes a design requirement for a deployment tool. The requirements are derived
from measurement, not asserted.

**Part 2, the tool.** satdeploy is built to those requirements and measured on the
identical channel, same levels and same seeds, then evaluated requirement by
requirement.

### Requirements derived from the baselines

| # | requirement | derived from |
|---|---|---|
| R1 | The receiver must verify delivered bytes against the source, independently of the transfer's own status | DTP delivers a full-size file whose hash mismatches |
| R2 | A transfer that did not deliver every byte must report failure | DTP reports success on a corrupt file |
| R3 | Lost fragments must be re-requestable; one pass over the data cannot be the only chance | DTP never retransmits, so loss is unrecoverable |
| R4 | Work completed before a contact ends must survive it, so the next contact resumes rather than restarts | RDP delivers intact at 0.30 but needs 594.75 s for a 67 KB module against an 8.9-minute (534 s) mean contact, and has no cross-contact resume |
| R5 | Completion must be reachable within a bounded number of contacts, and that number must be observable | RDP's airtime at 0.30 is 4.79x its own loss-free control (594.75 s against a measured 124.28 s) with no bound stated or observable |
| R6 | Recovery must work under correlated loss, not only isolated drops | Link Characterisation: mean burst 7.8 |

R1 to R5 come from Part 1. R6 comes from the Link Characterisation chapter, which is why
the Gilbert-Elliott runs apply to satdeploy only and DTP's absence there is structural
rather than a gap.

### How the builds map onto the requirements

| build | R1 | R2 | R3 | R4 | R5 | R6 |
|---|---|---|---|---|---|---|
| DTP | fail | fail | fail | fail | fail | n/a |
| RDP | pass | pass | pass | fail | fail | untested |

Measured 2026-08-26: RDP does NOT fail on integrity. At 0.30 it retransmitted through 197
drops, sent 703 frames to deliver a 267-fragment artifact, and delivered it bit-exact
(`crc SUCCESS`, hash MATCH, claimed DELIVERED). R4 and R5 therefore rest on cost and the
contact boundary, not on abort. That is the stronger form of the requirement and it is
what prices invocations against the 8.9-minute contact.
| satdeploy | expect pass | expect pass | expect pass | expect pass | expect pass | GE runs |

satdeploy differs from the deployed uploader by adding an integrity check and retry
rounds. The failures measured in the DTP experiment are what those two mechanisms
address, so the DTP row is the control that attributes satdeploy's result.

Expected outcomes are stated here, before the runs, and frozen with the rest of this
document. A run that contradicts its expectation is a result, not an error.

### Capture schema

The captures must record the failure mode, not a pass/fail verdict, or the requirements
cannot be derived from them. The existing columns already do this: DTP records
`client_reported_success`, `matches_original` and `corrupt_but_accepted`; RDP records
`claimed` against `sha`; satdeploy records `invocations` (`passes` in the committed
files), `result` and `ext_sha`.

## The artifact

`libjpegxl.so`, 67 256 bytes, 267 fragments at 252 B payload. 1.9 min at 4800 bit/s.

A real flight module, byte-identical on SOM1, SOM2 and the Yocto recipe that flashes
them. It is the smallest of the 13 pipeline modules, which is the point: the systems
fail on the artifact most favourable to them.

Provenance, sha256. The copies under `captures/` are untracked and regenerate from
`disco-ii-flight-checkout/SOM1/usr/share/pipeline/`:

    31b50250cc4d02ba2df37e4631d7269acc21cba6b29337c300408b1816e3db89  libjpegxl.so  67256 B
    dc443182d2fa37f34a57d5343c865e888a8309f6f3c956bf86ec06081e143a63  libcolor.so  365688 B

`csp_loss status` must report `offered` = 269. `csp_loss` counts every frame this node
transmits on CAN0, so the figure is the 267 data fragments plus the two-frame port-7
metadata handshake. The guard is fragments + 2, which for `libcolor.so` is 1454.
Confirmed 2026-08-26 on dtp_L0_s1.

## The matrix

3 builds x 3 levels x 3 seeds = **27 runs, about 1.7 h** at 4800 bit/s.

| experiment | build | command | target | 0.00 | 0.10 | 0.30 |
|---|---|---|---|---|---|---|
| E1 | DTP, deployed uploader | `upload_file` | 5424 | 3 | 3 | 3 |
| E2 | RDP, reliable path | `upload` | 5431 | 3 | 3 | 3 |
| E3 | satdeploy, recovery build | `satdeploy upload` | 5427 | 3 | 3 | 3 |

Seeds 1, 2, 3. The same seed gives every build the identical drop schedule. Claims are
existence and absence, so what is needed is unanimity across seeds, not a rate.

0.00 is the control and must pass before any lossy run in that experiment counts.

DTP runs under `-M 8` so drops stay off the port-7 handshake. RDP and satdeploy run
plain `-L`: they retransmit, and `-M` would block the same fragment on every resend.


## Spot runs

Three runs that make the size discussion concrete, at iid 0.30, seed 1, on
`libcolor.so` (365 688 B, 1452 fragments, the median flight module, 10.2 min at 4800):

| build | why | est. |
|---|---|---|
| satdeploy | invocations scale with fragment count; the RQ3 number | 20 min |
| DTP | confirms the failure is size-independent | 12 min |
| RDP | confirms loud failure holds at 1452 fragments | 14 min |

About 45 min. Optional and separable: if cut, RQ1 and RQ2 are unaffected.

Nothing larger is run. `libclassification.so` at 2 075 152 B needs 57.6 min at flight
rate, about 6.5 contacts. That is arithmetic and belongs in the discussion, not on the
bench. RDP could not receive it anyway: `vmem_node` serves 1 MiB.

## Gilbert-Elliott, optional

`csp_loss -B 7.8 -L <uplink marginal> -S <seed>` replays the measured burst channel and
ties the Link Characterisation chapter to this one. Two runs at seed 1, for RDP and
satdeploy. About 20 min.

Not available for DTP: `-B` and `-M` are mutually exclusive (`csp_loss_apm.c:336`) and
DTP needs `-M` to protect the handshake. Leave DTP out rather than change the
instrument before submission.

The marginal is the uplink one-way figure from Link Characterisation, not the
round-trip number. `[VERIFY]` until that chapter is final.

## Total

27 core runs, plus 3 size runs and 2 GE runs if wanted. **About 2.8 h.**

No code changes. `MAX_PASSES` already caps satdeploy invocations; exceeding it is the
outcome "did not complete within budget", which is a result, not an invalid run.

## Instrument and verdict

The csh rig: `experiments/*.csh` with the `csp_loss` APM injecting. The authoritative
verdict is the external hash computed over the board's serial console. The injector's
drop count is self-reported and is used only for the `offered` check.

## Exclusion rule, frozen before run 1

A run is invalid if and only if:

1. a board reset was detected during the run
2. the deployment agent stopped answering liveness probes
3. the bench host's CAN interface went down
4. a serial probe returned no canary-validated answer
5. the harness crashed mid-run
6. `offered` did not equal 269 (or 1454 for a size run)
7. more than one `upload_gs-server` was running

Invalidity is a property of the apparatus, never of the outcome, and no invalid run is
re-rolled or re-run in place.

Conditions 6 and 7 are new. Both were observed on 2026-08-25 and neither was covered by
the original five, which would have admitted three void runs as valid.

## Progress

**E1 run 1 of 9 done, 2026-08-26.** `dtp_L0_s1`: `offered 269, dropped 0, delivered 269`, source and
board md5 both `a2414c4e3ad4a586304c838e2b743bd1`, VERDICT MATCH. The injector's own bus check reported "1 ground server, no competing injector" before starting.

The 2026-08-25 deficit is explained: three runs reported `offered 1027` against a true
expectation of 1043 (1041 fragments + 2 handshake), a 16-frame loss caused by two
`upload_gs-server` processes racing. With one server the deficit is zero.

## Run naming

`<build>_L<level>_s<seed>`, for example `dtp_L0.30_s1`, `recovery_L0_s2`. Size runs get
an `m_` prefix, GE runs a `ge_` prefix. Capture filenames are never renamed once
written, per revision brief §12.3.

## Retiring the old set

The 256 KiB captures are archived, not deleted, until this matrix is complete and the
manuscript re-points to it.

## GSTACK REVIEW REPORT

| Review | Trigger | Why | Runs | Status | Findings |
|--------|---------|-----|------|--------|----------|
| CEO Review | `/plan-ceo-review` | Scope & strategy | 0 | — | — |
| Codex Review | `/codex review` | Independent 2nd opinion | 0 | — | — |
| Eng Review | `/plan-eng-review` | Architecture & tests (required) | 2 | ISSUES_OPEN | 15 issues, 6 critical gaps |
| Design Review | `/plan-design-review` | UI/UX gaps | 0 | — | — |
| DX Review | `/plan-devex-review` | Developer experience gaps | 0 | — | — |

**CROSS-MODEL:** the outside voice (Claude subagent, Codex not installed) found nine
issues this review missed, three of which contradicted its findings. The three
contradictions were resolved in the author's favour of the outside voice on two
(headline metric, R4 unmeasured) and against it on one (the satdeploy failure is not
interval-list overflow; the traces show a single empty gap on the first repair round).
Highest-value outside-voice finding: the failed libcolor zero-loss control is a one-line
harness defect (`-t 10000` hardcoded on the prefill while `UP_TO` scales), not a result.

**VERDICT:** ENG REVIEW NOT CLEARED — 6 critical gaps, 11 implementation tasks (T1-T11)
in `~/.gstack/projects/MahmoodSeoud-csp-linksim/tasks-eng-review-20260826-153006.jsonl`.
T1 through T7 are P1 and block citing the current results.

Decisions taken: schema consolidated without re-running (D2); control rule enforced in
the harness; `offered` guard becomes a lower bound; measured requirements table in main
text with frozen expectations to an appendix; invalid runs may be re-run at the same seed
once the apparatus fault is fixed; four operator metrics made canonical with never-lethal
leading RQ1/RQ2; `-d PORT` filter added so DTP gets a Gilbert-Elliott row; verdict fails
closed; shared preflight with a first-run gate and resume; `airtime_guard.sh` extended to
`csp_loss`; contact-interrupt experiment added, funded by cutting DTP to one seed per
lossy level.

NO UNRESOLVED DECISIONS
