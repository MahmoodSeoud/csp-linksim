# Scope decision: board-only (2026-08-23)

**Decision (author).** The thesis keeps only the **flight-hardware (payload-board,
CAN) arms** paced at 4800 or 9600 bit/s. Every host / ZMQ-loopback arm is cut from
the thesis and its data removed from the citeable set. Rationale: RQ1/RQ2 stand on
board evidence alone; host arms carry only design/mechanism claims and invite the
"you're passing host results off as flight results" objection. Cleaner to cut than
to defend.

**Parked (NOT done for now):** running raw DTP *on the board* (the on-board
`upload_client` already plumbs resume to `dtp_client_main`; it is pinned off at
`upload_sat-client/src/main.c:418`; binary is aarch64 and already runs on the
board). This is the one thing that would let board-only recover the
resume-false-completes result on flight hardware. Left out per author call.

## What STAYS (board, 4800/9600, console digest)
- Deployed uploader, board 9600 — 25/25 silent corruption (`rq3_corruption.csv`).
- Deployed uploader, board 4800 — 6/6 no-file-at-destination (`dtp_flight_sweep.csv`).
- satdeploy recovery + naive, board 9600 and 4800 (`board_external_full/`,
  `satdeploy_external.csv`, `satdeploy_board_rev.csv`, rollback cell).
- Reliable path, board `csp_loss` sweep — loud aborts (`rdp_csploss_sweep.csv`).

## What is CUT (host / loopback) — data to remove LAST, after prose stops citing it
- Deployed uploader host replication: `captures/dtp_zmq_sweep.csv`.
- Raw DTP arm: `captures/rawdtp_sweep.csv`, `captures/h2_rawdtp_finding.md`,
  `captures/evidence/rawdtp_trace_20260719/`,
  `captures/evidence/rawdtp_trace_loss0_20260816/`,
  `captures/evidence/underflow_offline_finding.md`; scripts `rawdtp-point`,
  `dtp_hole_analysis.py`, `dtp_underflow_offline.py`.
- Reliable-path HOST arm: `captures/rdp_zmq_sweep.csv`, `rdp_zmq_verify.csv`,
  `rdp_csh_silent.csv`, `captures/evidence/rdp_zmq_silent_forensics_*.csv`,
  `captures/evidence/rdpcl_*session.log`; scripts `rdp-host-sweep`,
  `rdp-rate-compare`, `rdp-settle-check`; `experiments/rdp-silent-*`.
  (Board `csp_loss` reliable path STAYS.)
- satdeploy transfer-core HOST + v1 h2h: `captures/svu_sweep.csv`, `h2h_svu.csv`,
  `satdeploy_core_paced.csv`, `repro_svu_deterministic_*.csv`; scripts
  `satdeploy-core-host-sweep`, `satdeploy-core-host-paced-sweep`,
  `satdeploy-host-sweep`.

## What board-only COSTS (accepted)
- The resume-false-completes result (raw-DTP traced cell, 5028 bytes wrong) — host
  only. Localization "defect is in libdtp, not our uploader" now rests on the
  byte-identical-code fact (`start_receiving_data` md5 `07e6c4f4…` across
  `upload_sat-client`, `dipp-apm`, host client) plus the board arm running that
  exact function, not on the raw-DTP arm.
- The reliable path's *silent* corruption (9/53) — host only; on the board it
  aborts loudly, so the real-link story becomes "reliable path fails loudly."
- The v1-vs-core adoption comparison and the 36-matched-cell core figure — host
  only. RQ3 reverse-channel cost survives via the board 7-cell grid (<1% forward).

## Manuscript surgery required (prose FIRST) — see cut map handed to author
abstract; ch1 (contributions 2 & 3, scope); ch4 (mechanisms list, checks, loss-model
exception para); ch5 (eval-differential cuts, delete sec:eval-h2h, eval-completion
reliable-path column, eval-threats caveats 1/6 + refuted-0%); ch6 (all three
sections); ch7 (all three RQ answers); DELETE appendix_svu; rework/DELETE
appendix_era; tables (regen tab:differential, DELETE tab:rdpincidence + tab:h2h,
prune armconfig + evidence-map, tab:completion column); figures (redraw fig:silent,
fig:evidence-map); re-point make_tables/make-results; re-baseline acceptance test.
Then remove the cut data files. Then rebuild PDF + run check-manuscript.
