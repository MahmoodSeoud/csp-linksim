# Board loss sweep with external verification — run log

Opened 2026-08-16 (UTC) before any run. Purpose: re-run the entire satdeploy
board loss sweep so that every cell's verdict comes from a check satdeploy
cannot influence (serial-console read-back + ground-side hash by
`scripts/external-verdict.py`, which imports Python stdlib only and shares no
code, process, or state with satdeploy). Both verdicts — the tool's claim and
the external verdict — are recorded per cell for the agreement matrix.

## Grid (fixed; read from the original sweeps' config and logs)

- Loss rates: 0, 0.02, 0.05, 0.10, 0.20, 0.30 (Bernoulli-seeded drops at the
  injector, Gilbert-Elliott burst parameter 4 — identical to the original
  sweeps' injector invocation).
- Seeds: smart arm 1–5 (July-era seed set), naive arm 1–3. 30 + 18 = 48 cells.
- Per-pass injector seed: `seed*1000 + pass*10 + setup_fails` (original rule).
- Artifact: `captures/payload_256k.bin`, 262144 bytes — ground truth below.
- Fragment grid: push MTU pinned to 256 → 1058 fragments (`-m 256`; the APM's
  compiled default drifted to 1024 after the era sweep — the drifted 259-frag
  grid of the August logs is a documented instrument defect and is NOT
  reproduced here, per user decision 2026-08-16).
- Pacing: RATE_BPS=4800 (user decision 2026-08-16, the flight rate; the
  original sweep script's configured default). PACE_US=426666/frame,
  ≈451 s clean pass.
- MAX_PASSES=12, resume ON, unique destination per cell
  `/home/root/sd_xf_<label>.bin` (fresh TAG `xf_` → board-fresh app namespace).
- Agent: satdeploy-agent-(smart|naive)-bk at CSP addr 5427 on can0, launched
  `-i CAN -p can0 -a 5427`. satdeploy runs AS RELEASED — no modification.
- Dry-run/protocol-validation cell: smart loss 0 seed 99 (seed outside the
  grid; reported alongside but not part of the 48-cell grid).

## EXCLUSION RULE (frozen now, before the first run; may not change)

A cell is **INVALID**, with reason code, iff any of:

1. **CONSOLE_UNVERIFIED** — after 6 serial probes, no canary-validated answer:
   neither a well-formed SIZE + 64-hex hash pair nor two consecutive
   canary-validated NOFILE answers.
2. **AGENT_DOWN** — the agent fails the 3-try liveness probe (with settle
   gaps) before or during the cell. The cell is recorded INVALID, the sweep
   halts for an operator agent restart, and on resume continues from the NEXT
   cell. The INVALID cell is never re-rolled.
3. **HARNESS** — stray injector bridge detected, injector fails to start, or
   harness crash mid-cell.
4. **SETUP_STUCK** — 4 consecutive setup hard-errors (a pass injecting 0
   frames while the agent answers pings; control-plane failure, not data
   loss). The first 3 are uncounted retries, per the original sweep semantics.
5. **BOARD_RESET** — board `/proc/uptime` at cell end lower than at cell start.

NOT INVALID: reaching 12 passes without DEPLOYED — a valid measured outcome
(tool claim INCOMPLETE); the external verdict is still taken.

External verdicts: **PASS** (console hash == ground truth),
**FAIL_MISMATCH**, **FAIL_ABSENT** (two canary-validated NOFILE),
**INVALID** (rules above). INVALID cells are reported, never dropped or
re-rolled (the harness skips any label already present in transfer.csv;
deleting rows is prohibited).

## Instrument provenance (sha256)

| item | hash |
|---|---|
| payload_256k.bin (ground truth, below) | 2c1fa79ab2799d2fd1f2a0ff35c6f369d8cd7625b8d636c9528044641a628ba5 |
| ci_inject_bridge (build 2026-08-04) | 8a2b1ffc292189a82584d897c6d6c0cd34917da8c268ff0158fdfad901d05a5f |
| libcsh_satdeploy_apm.so (build 2026-08-04, same .so as the era sweeps) | 72d4c4f02323e206388c3c0131b61f008147beb5c9c99500d55579750b50b265 |
| csh (maintained fork) | e5fddc92f1002ae96f17946c6fdea59cdd495323260d77c46d9dcd4b1e573f65 |
| satdeploy-agent-smart-bk (on board, via console) | e4d7cf20ef9dd254ebd9c129d3d52164383076229c0029f85fbbb4ddeb0abae8 |
| satdeploy-agent-naive-bk (on board, via console) | d07b9866a1b2894cfd9e2ac1bd52ce5ec3ef07a070e10f8cbd0c1e976e1bfe1d |

Serial console: /dev/serial/by-id/usb-FTDI_FT232R_USB_UART_A50285BI-if00-port0
(FTDI → payload board root shell). Board clock is wrong (reads 2022); all
timestamps in evidence are ground-side UTC. Board uptime at preflight:
1074926 s (~12.4 d).

## Per-cell evidence (directory `<arm>_L<loss>_s<seed>/`)

`harness.log` (timestamped) · `push_p<N>_a<M>.log` (satdeploy's own csh output
— its claimed verdict, unmodified) · `inject_p<N>_a<M>.csv` (injector
per-frame drop log) + `.out` (injector counters incl. reverse channel) ·
`candump.log.gz` (promiscuous capture of all can0 traffic for the cell) ·
`console_probe_<n>.txt` (raw serial transcripts) · `external_verdict.json`.
Sweep-level: `transfer.csv` (tool-side facts, written by the harness) and
`verdicts.csv` (written ONLY by external-verdict.py).

## Re-derivation

- Ground truth: `python3 scripts/external-verdict.py digest --artifact captures/payload_256k.bin --out-dir captures/board_external_full`
- Any cell's external verdict: recompute from `console_probe_*.txt` (the
  64-hex line) vs `groundtruth.sha256`; the JSON/CSV must agree.
- Tool claim: `grep -aE "> Deployed|Already deployed" push_p*.log` per cell.
- Injected/dropped: row counts / col-8 sums of `inject_p*_a*.csv`.
- Agreement matrix: join `transfer.csv` and `verdicts.csv` on label.
- Verdict-script imports: `grep -n '^import' scripts/external-verdict.py`.

## Amendment 2026-08-17 (pre-grid; user-directed after dry-run 1)

- RATE 4800 bit/s CONFIRMED by user ("5800" in an earlier message was a typo).
- TOOL MODIFICATION, user-approved wording "default plus parameters": the
  ground-side APM client's push verify-wait (= SVU serve window) is now
  configurable via env SATDEPLOY_PUSH_TIMEOUT_MS (satDeploy base commit
  66f599a + satdeploy_client_patch.diff, 38 lines). Unset preserves the
  released 360 s. The sweep runs with SATDEPLOY_PUSH_TIMEOUT_MS=1200000.
  Rebuilt libcsh_satdeploy_apm.so sha256
  e5a23dcc79bd9be48eaff54adc308d86e4b4a477b77095827c4f3503aa22c7c3
  (released build was 72d4c4f0…). The AGENT on the board stays the released
  binary (smart e4d7cf20…, naive d07b9866…) — the deploy/verify/recovery
  mechanism under test is unmodified. The released-client incompatibility at
  4800 (360 s < 451 s serialization) stays documented via dry-run 1.
- HARNESS: PUSH_TO now also exceeds the client window (client must time out
  on its own, never be killed by the harness); setup-hard-error counter made
  CONSECUTIVE (reset after each data-bearing pass) to match the frozen rule's
  wording — the era-derived code counted cumulatively; under consecutive
  counting dry-run 1 (s99) would not have gone STUCK, but its recorded row
  stands (never re-rolled; it is a protocol cell, not a grid cell).
- Dry-run 2 = smart loss 0 seed 98 under the amended protocol.

## Amendment 2 — 2026-08-17 (pre-grid; user decision after dry-run 2)

- Dry-run 2 (smart_L0_s98, 4800, patched client): tool=DEPLOYED external=PASS,
  but via 2 data passes + "Already deployed" — agent log shows (a) a 3-frame
  RX hole at ZERO injected loss (accepted 1055/1058; bridge forwarded 1058),
  (b) the released agent's multi-round recovery sessions report VERIFIED yet
  materialize a truncated .tmp (4096/24576/36864 B = block multiples) so the
  size check fails the pass; single-round sessions materialize correctly.
  At 4800, pass/overhead metrics would measure these agent artifacts, not
  injected loss. At 9600, 16 previously externally-judged cells show clean
  era semantics.
- USER DECISION: full 48-cell grid at RATE_BPS=9600 with the RELEASED client
  — patch stashed (satDeploy `git stash`), rebuild verified BYTE-IDENTICAL to
  the era build (sha 72d4c4f0…, also matches ~/.local/lib/csh era copy). No
  satdeploy modification is active anywhere in the grid. Afterward: a small
  documented 4800 exploration row (smart, patched client e5a23dcc…,
  SATDEPLOY_PUSH_TIMEOUT_MS=1200000, labels suffixed `_r4800`) formalizing
  the flight-rate findings.
- Harness deltas: SATDEPLOY_PUSH_TIMEOUT_MS now opt-in (unset = released
  360 s); RATE_BPS default 9600; resume-skip keyed on full label; optional
  LBL_SUFFIX for non-grid rows. Dry-run rows (s98/s99) remain recorded.
- Pre-grid board reset: agent fresh-restart (nohup), /opt/satdeploy archived
  to /opt/satdeploy.pre-grid-20260817, state dir wiped, dry-run sd_xf_* files
  removed from the board (their hashes/verdicts are recorded on the ground).

- 2026-08-17T02:52 VALIDATION CELL smart_L0_s97 (9600, released client):
  INVALID(SETUP_STUCK) — HARNESS DEFECT, not a link/tool event: the sweep-start
  log line referenced the now-unset SATDEPLOY_PUSH_TIMEOUT_MS under `set -u`;
  the dying pipeline subshell ran the inherited EXIT trap, which removed the
  APM staging dir, so csh had no `satdeploy` command and every attempt aborted
  with 0 frames on the wire (bridge counters all zero). Row stands (never
  re-rolled; non-grid seed). Harness fixed: defaulted expansion + cleanup
  guarded to the main shell (BASHPID check). Validation repeated as s96.

- 2026-08-17T04:09 smart_L0.05_s5 recorded INVALID(BOARD_RESET), sweep halted
  — LATER SHOWN FALSE POSITIVE: the board's uptime is continuous (1,102,613 s
  at 04:15, consistent with the cell-start reading 1,102,281 s); the "8" the
  harness read at cell end was serial character loss inside the digits (the
  probe's canary protected the line, not the number). Per the frozen rule the
  INVALID row STANDS (never re-rolled); the tool-side facts (DEPLOYED, 1102
  injected) and aux console evidence remain in the cell dir. Detector
  garble-proofed before resuming: uptime value must round-trip twice
  identically inside one framed token. Cells 1–14 of the smart arm: all
  DEPLOYED x PASS, single-pass, era-consistent counts.

- 2026-08-17T04:18 smart_L0.10_s2 INVALID(AGENT_DOWN) — the known state-scan
  wedge: after 16 measured cells the agent (pid 17511) sat at 3:09 CPU,
  process alive but node deaf; /var/lib state was EMPTY (sidecars clean on
  success) — the accumulation is registry apps + backups under /opt/satdeploy
  (16 apps). Row stands per the frozen rule. Agent restarted (pid 18047),
  registry archived to /opt/satdeploy.wedge1-20260817. HARNESS AMENDMENT
  (ops-maintenance, between cells only, transfers untouched): preventive
  agent refresh every REFRESH_EVERY=8 measured cells — kill, archive registry
  to /opt/satdeploy.acc-<arm>-c<N>, wipe session state, nohup relaunch with
  garble-proofed check-then-launch (never two agents on one addr).

## Amendment 3 — 2026-08-17 (pre-registered before any v1/rollback run; user
directive: measure what was deferred as future work)

- V1 ARM (whole-artifact predecessor, matched on-board comparison): agent and
  APM built from satDeploy e8b0842 (the commit before the SVU replacement,
  fresh worktree ~/thesis/satDeploy-v1). Agent aarch64 sha256 95445442…,
  deployed to the board as /home/root/satdeploy-agent-v1; matched v1 APM
  sha256 154916f8… (ground). Data plane: DTP dport 8, overhead 4 → 1041
  fragments; injector drops dport 8. Grid: same losses × seeds 1–5, 9600,
  push MTU 256, MAX_PASSES=12, TAG xf_, labels v1_L<loss>_s<seed>. Same
  frozen exclusion rule, same external verdicts, same evidence layout.
  Validation cell v1 L0 s95 before the grid.
- ROLLBACK DEMO CELL (closes "rollback not exercised"): on the smart agent —
  deploy artifact A (payload_256k.bin), then artifact B (defined as the
  bytewise complement of A, materialized as captures/payload_256k_B.bin,
  ground truth computed pre-run by external-verdict.py into
  board_external_full/rollback/), then `satdeploy rollback`; the external
  console digest of the destination must equal A's ground truth. Evidence in
  board_external_full/rollback/ with the same per-step logs.
- Sequencing: naive arm → smart agent swap → 4800 exploration row (patched
  client, labels _r4800) → rollback cell → v1 agent swap → v1 validation +
  arm → final report.

## Amendment 4 — 2026-08-17 (user decision: FULL 4800 grids, both arms)

- The 4800 exploration row is upgraded to FULL grids: smart 30 cells + naive
  18 cells at RATE_BPS=4800, patched client (SATDEPLOY_PUSH_TIMEOUT_MS=
  1200000, .so e5a23dcc…), labels suffixed _r4800, same losses/seeds/rule/
  evidence as the 9600 grid. The 9600 grid remains the primary era-comparable
  dataset; the 4800 grids measure the tool at the operated flight rate, where
  the released agent's multi-round materialization defect and board RX stalls
  are part of the measured behavior (documented via dry-runs s98/s99).
- Revised sequencing (minimizes agent swaps): naive-9600 (running) →
  naive-4800 (no swap, ~24 h) → smart swap → smart-4800 (~8 h) → rollback
  cell → v1 swap → v1 validation + v1 arm (~4 h) → final report.

## Amendment 5 — 2026-08-17 (user: append the ch5.2 instrumented resume run)

- Appended after the v1 arm: ONE instrumented rawdtp resume run to decide the
  ch5.2 truncation hypothesis (gs-server sums end×payload − start×payload
  unsigned over the client's interval list with no end>start check; an
  inverted/out-of-range interval underflows to just below 2^32). Design: no
  tool modification — reproduce the traced resume cell (scripts/rawdtp-point,
  RESUME_LOSS grid), capture the resume-request packet's interval list on the
  wire (promiscuous monitor) alongside the server's logged session length,
  then recompute the unsigned sum offline from the captured intervals.
  CONFIRMED iff the recomputation reproduces the logged near-2^32 length;
  REFUTED if the captured intervals are well-formed or the arithmetic
  disagrees. Full cell protocol frozen here before that run; requires
  gs-server restart first (stale-DTP-state gotcha). Evidence under
  board_external_full/resume_underflow/.

- 2026-08-17T12:08 naive_L0.10_s2 INVALID(AGENT_DOWN) — SECOND wedge type:
  agent deaf at near-zero CPU (0:08), persisting 2+ h (ping re-test at ~14:30
  still unanswered), arriving ~24 sessions (2 naive cells) after the cell-8
  preventive refresh ran on schedule. Unlike the smart busy-loop wedge (3:09
  CPU), this is deaf-at-idle; naive cells run 12 sessions each, so the wedge
  clock appears to tick per session, not per cell. Row stands per the frozen
  rule. Agent restarted (pid 19154, registry archived wedge2-20260817);
  naive arms now run with REFRESH_EVERY=2 (≈ every 24 sessions). Naive-9600
  cells 1–10: 3 clean DEPLOYED x PASS at loss 0, 7 lossy cells honest
  INCOMPLETE x FAIL_ABSENT through 12 full passes each — the designed
  contrast, externally confirmed.

- 2026-08-17T~15:00 CORRECTION to the 12:08 entry: the "second wedge type"
  interpretation was WRONG. The bench host's can0 was found DOWN (state DOWN,
  qdisc noop, counters zeroed) — the liveness probes died on the BENCH side;
  the board agent was healthy the whole time (clean startup log, listening,
  one process). The naive_L0.10_s2 INVALID(AGENT_DOWN) row still stands — the
  pre-registered probe protocol did fail — but the cause is recorded as a
  bench-instrument failure (can0 down; cause unknown: kernel log unreadable
  without root; counters-zeroed pattern fits USB re-enumeration or bus-off).
  The 2+ h "persistent deafness" observation is void (same dead interface).
  The naive per-session wedge theory is withdrawn: naive ran 87+ sessions
  cleanly; the smart wedge correlates with REGISTRATION count (16 apps), not
  sessions — REFRESH_EVERY reverts to 8 for the remaining naive cells.
  Campaign paused awaiting operator can0 bringup (needs root).

## Amendment 6 — 2026-08-18 (harness mis-execution, disclosed in full)

- THE 7 `_r4800` NAIVE ROWS RAN THE RELEASED CLIENT, NOT THE PATCHED ONE.
  Cause (mine, not the bench's): the patch lives in a `git stash`, so the
  satDeploy tree build reverted to the released 360 s binary when the 9600
  grid was restored; the 4800 launch exported SATDEPLOY_PUSH_TIMEOUT_MS but
  the harness still loaded the tree .so. The sweep-start line records it:
  `push_timeout_ms=1200000 apm_sha=72d4c4f0…` (released), where the patched
  build is e5a23dcc…. Caught at cell 8 of 18; the arm was stopped mid-cell.
- DISPOSITION: the 7 rows are RETAINED, not deleted and not re-rolled. They
  are honest measurements of a degenerate configuration — the released client
  at flight rate, i.e. dry-run 1's finding repeated across the naive arm
  (every cell INCOMPLETE × FAIL_ABSENT because the 360 s window cannot span a
  451 s paced pass, independent of injected loss). They are NOT the naive
  flight-rate dataset and must never be cited as one; label suffix `_r4800`
  is hereby reserved for this mis-executed series.
- The intended arms run under a distinct suffix `_r4800p` (p = patched
  client, pinned copy at board_external_full/bin/
  libcsh_satdeploy_apm_patched.so, sha e5a23dcc…, verified to carry the
  SATDEPLOY_PUSH_TIMEOUT_MS symbol). Both series therefore coexist in the
  data with no overwriting.
- HARNESS FIX (prevents recurrence): when SATDEPLOY_PUSH_TIMEOUT_MS is set,
  the harness now selects the pinned patched client automatically AND a
  preflight aborts the run if that binary lacks the override symbol. A
  released-client slow-rate run can no longer start by accident.

## Rollback cell RESULT — 2026-08-18T05:40Z

- Protocol as pre-registered (Amendment 3): push A → push B → `satdeploy
  rollback <path>`; B = bytewise complement of A (captures/payload_256k_B.bin,
  ground truth e7163f41…, computed before the run by external-verdict.py).
- Result: A tool=DEPLOYED/external=PASS(A); B tool=DEPLOYED/external=PASS(B);
  after rollback the tool printed "> Rolled back sd_rb_20260818 to 2c1fa79a"
  and the EXTERNAL console digest of the destination equals A's ground truth
  → PASS. Rollback is demonstrated end-to-end under an external check.
- CONDITIONS (disclose when citing): this cell is UNPACED and carries NO
  injected loss — direct CAN route, no injector bridge. It exercises the
  backup/restore mechanism, not transfer under adversity, which is what the
  "not exercised" gap needed. Each step took ~15 s.
- METHOD BONUS (control for the read-back path): the same console probe
  returned A's digest, then B's, then A's again on the same destination path.
  A stale or cached answer cannot produce that sequence, so the external
  channel demonstrably tracks the artifact's actual content.
- Evidence: captures/board_external_full/rollback/ (push_A.log, push_B.log,
  rollback.log, console_probe_*_step{1,2,3}.txt, verdicts.csv, both ground
  truths, harness.log).

## Incident 2026-08-18T09:27Z — sweep killed by session teardown (smart-4800)

- The smart-4800 arm's controlling shell was torn down when the previous
  agent session's process exited; the sweep died mid-cell. Last completed
  cell: smart_L0.10_s1_r4800p (09:15:03Z). Cell smart_L0.10_s2_r4800p had
  finished pass 1 (1058 injected, 65 dropped, not deployed) and was 367
  injector rows into pass 2 when the kill landed (push log ends "Session
  terminated, killing shell..."; partial pass-2 counters injected=367
  dropped=31, forwarded=336, in inject_p2_a2.{csv,out}).
- DISPOSITION per the frozen rule, item 3 (HARNESS = harness crash mid-cell):
  smart_L0.10_s2_r4800p is INVALID(HARNESS) and is NOT re-rolled. The
  external verdict was written by scripts/external-verdict.py --invalid
  HARNESS (verdicts.csv); its two aux console probes returned no trustworthy
  answer, so no aux digest is recorded. All raw evidence is retained
  (candump.log.gz, both injector logs, both push logs, harness.log).
- The transfer.csv row for this cell was HAND-RECORDED, because the harness
  died before it could write one: passes=1, injected=1058, dropped=65,
  agent_claim=INTERRUPTED, wall_s=668 (cell start to last evidence write).
  The partial pass-2 injector counts are deliberately EXCLUDED from the
  injected/dropped totals, matching the harness's own semantics (a pass
  counts only when it completes). This is the only row in the campaign not
  written by the harness; it is marked here for audit. No external verdict
  was hand-written -- that remains exclusively the judge script's output.
- Arm state at the interruption: 17 of 30 smart-4800 cells recorded (16
  DEPLOYED x PASS through L0.05, plus L0.10_s1 DEPLOYED x PASS at 9 passes /
  5.16x overhead, plus this INVALID). Remaining: 13 cells from L0.10_s3.

## Incident 2026-08-19T10:36Z — naive-4800 launch killed by launcher-call timeout

- The naive-4800 arm was launched detached (setsid) inside the same tool call
  that then waited for its first cell; the call hit its own 5-minute timeout
  and the harness cleanup killed the freshly created session, sweep included.
  Cell naive_L0_s1_r4800p died ~4.5 min into pass 1 (554 partial injector
  rows). DISPOSITION per frozen rule item 3: INVALID(HARNESS), never
  re-rolled; judge wrote the external verdict (--invalid HARNESS); transfer
  row HAND-RECORDED with completed-pass semantics (passes=0, injected=0 —
  partial pass-1 counts excluded, matching the smart_L0.10_s2_r4800p
  precedent; raw partial evidence retained in the cell dir).
- Cause is the operator toolchain, not the bench. FIX: detached launches now
  happen in a call that returns immediately; waiting/verification happens in
  a separate call (the pattern that worked for the smart-4800 resume).
- Orphan check after the kill: no stray bridge or candump survived.

## Timeline

- 2026-08-16T (preflight): bench + board preflight green (console canary OK,
  smart-bk pid 16547 running, binaries hash-verified, 4.9G free on board,
  state dir empty, 7 backups from the rev run pending archive). GO received
  from user; RATE_BPS=4800 and console-side swap/wipe approved.
- 2026-08-16T~20:00 (smart-arm agent restart, approved wipe): old smart-bk
  (pid 16547) SIGTERMed (logged "Shutting down..."), registry archived to
  /opt/satdeploy.pre-xf-20260816, /var/lib/satdeploy/state wiped. FIRST
  relaunch (setsid) died silently within minutes — no crash record in dmesg,
  no shutdown message: setsid failed to detach on the console shell, agent
  killed by SIGHUP. SECOND relaunch with nohup (pid 17162) — stability watch
  before dry-run. Board-state notes: vmem_node (pid 2304) busy-looping at
  ~94% CPU since ~Aug 11 (126 h CPU; the Aug-16 rev grid ran fine alongside
  it — left untouched for condition parity), 56 zombie tasks present, load
  1.00 steady. Board clock reads 2022 (known; ground UTC is authoritative).
- 2026-08-16T21:00 DRY-RUN RESULT (smart_L0_s99, RATE_BPS=4800): tool=STUCK,
  external=INVALID(SETUP_STUCK) per the frozen rule — with auxiliary console
  evidence that the artifact on the board is bit-perfect (sha 2c1fa79a…,
  262144 B) while satdeploy claimed "No response from agent (timeout)" on
  every pass. ROOT CAUSE (from released source, satdeploy_apm.c:47): the push
  client waits at most DEFAULT_TIMEOUT=360 s for the agent's verify response
  (hardcoded, no flag/env); the paced drain of a clean 1058-frame pass takes
  451 s at 4800 bit/s, so the reply can never arrive in time. At 9600 the
  drain is 225 s and fits — all original board-sweep rows ran at 9600.
  CONSEQUENCE: 4800 cannot reproduce the original experiment with the tool
  as released; awaiting user decision on the pacing rate. Dry-run evidence
  retained under smart_L0_s99/ and in transfer.csv/verdicts.csv (seed 99 =
  protocol-validation, not a grid cell; recorded, not re-rolled).
- Harness `scripts/satdeploy-board-external-sweep` sha256
  dec91cbf7af0e3cdf73222a8440e5591e6395f7dd1223d580d1b85f577a732b3; verdict
  script `scripts/external-verdict.py` sha256
  7fccffb0d67b14383f457a19c9e40b3cf1505e2721ff7765e3a3c53da8531285 (imports:
  argparse, datetime, hashlib, json, os, re, subprocess, sys, time — stdlib
  only). Ground truth computed and stored before any run.
