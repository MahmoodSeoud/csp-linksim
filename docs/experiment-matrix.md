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
| R4 | Work completed before an abort must survive it, so the next contact resumes rather than restarts | RDP aborts under sustained loss and loses the partial transfer |
| R5 | Completion must be reachable within a bounded number of contacts, and that number must be observable | RDP gives no completion bound |
| R6 | Recovery must work under correlated loss, not only isolated drops | Link Characterisation: mean burst 7.8 |

R1 to R5 come from Part 1. R6 comes from the Link Characterisation chapter, which is why
the Gilbert-Elliott runs apply to satdeploy only and DTP's absence there is structural
rather than a gap.

### How the builds map onto the requirements

| build | R1 | R2 | R3 | R4 | R5 | R6 |
|---|---|---|---|---|---|---|
| DTP | fail | fail | fail | fail | fail | n/a |
| RDP | pass | pass | pass | fail | fail | untested |
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

`csp_loss status` must report `offered` = 267. That is the artifact-identity check.

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
6. `offered` did not equal 267 (or 1452 for a size run)
7. more than one `upload_gs-server` was running

Invalidity is a property of the apparatus, never of the outcome, and no invalid run is
re-rolled or re-run in place.

Conditions 6 and 7 are new. Both were observed on 2026-08-25 and neither was covered by
the original five, which would have admitted three void runs as valid.

## Before run 1

One DTP run at 0.00, seed 1. `offered` must read 267. Three runs on 2026-08-25 reported
`offered 1027` against an expected 1041 while also warning that two `upload_gs-server`
processes were racing. That warning is the fault; exactly one server must be up.

## Run naming

`<build>_L<level>_s<seed>`, for example `dtp_L0.30_s1`, `recovery_L0_s2`. Size runs get
an `m_` prefix, GE runs a `ge_` prefix. Capture filenames are never renamed once
written, per revision brief §12.3.

## Retiring the old set

The 256 KiB captures are archived, not deleted, until this matrix is complete and the
manuscript re-points to it.
