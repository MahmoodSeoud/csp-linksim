# Thesis loss-measurement — handoff (2026-06-15)

> **HISTORICAL.** This describes the June flatsat campaign (3-arm design, satDeploy as the
> subject). The thesis has since been reframed around the instrument, the evaluation is
> 4 arms (DTP / raw DTP / RDP / satdeploy), and three of them reproduce on a laptop via
> `scripts/reproduce`. Kept as the operational record of how the board data in `captures/`
> was collected; the script names and paths below reflect the repo as it was then.

## What this is
Master's thesis. **satDeploy** (loss-resilient satellite file uploader) is the subject;
**csp-intercept** (`~/thesis/csp-intercept`, git `master`) is an independent measurement
instrument I built to evaluate it. It injects controlled, CSP-aware packet loss on the REAL
DISCO2 flatsat CAN bus (`can0`) and verifies delivery with a two-oracle check (injector
drop-log = oracle A; promiscuous monitor = oracle B). Goal: the headline figure —
**completion vs injected loss**, deployed uploader vs satDeploy.

**3-arm design** (same injector, payload, bus — recovery is the only variable):
- `deployed-dipp` — real-world baseline (disco `upload_gs-server`→`upload_client`), fire-and-forget.
- `satDeploy-naive` — satDeploy built `-Dnaive_baseline=true` (retry/resume OFF).
- `satDeploy-smart` — satDeploy default (retry rounds + cross-push resume).

## STATUS
- **Arm A (dipp): DONE, clean, 30/30** → `captures/dipp_sweep.csv`. Result: delivered ≈ (1−loss),
  binary completion 0% for any loss ≥2% (pure fire-and-forget). Two independent runs agree to 4 dp.
- **Arm B (satDeploy-smart): DONE, 30/30 (loss 0–0.30 × 5 seeds)** → `captures/satdeploy_sweep.csv`.
  All DEPLOYED. Metric = PASSES-TO-COMPLETE (push-until-DEPLOYED, resume on): 0/2/5/10% = 1 pass;
  20% = 2 passes; 30% = 3 passes. Overhead ratio 1.0→1.4×. Each completion is the agent's own sha256.
  (The high-loss curve that was blocked on 06-13 is now complete.)
- **Arm C (satDeploy-naive): PARTIAL, 8 points** → same CSV. loss 0 (2 seeds) = DEPLOYED; loss
  0.05/0.10/0.20 (2 seeds each) = **INCOMPLETE even after 2 full pushes** (recovery off → fails like
  dipp). Missing: seeds 3–5, and the 0.30 level. Contrast is already unambiguous.
- **Core thesis SHOWN, all 3 arms**: at any loss ≥5%, dipp and naive complete 0% of files; smart
  completes + self-verifies everywhere, paying a bounded 1→3-pass / ~1.4× overhead cost.
- **Headline figure: DONE** → `figures/completion_vs_loss.{png,pdf}` via `scripts/plot_headline.py`
  (reads both CSVs, no live bus needed). Panel (a) completion-vs-loss 3-arm; (b) smart's cost of completion.

## OPEN DECISION (what's left to finish)
- **(A, recommended)** finish Arm C for symmetry: seeds 3–5 at 0.10 + the 0.30 level, then stop. This is
  robustness, not a new result — naive already fails at every loss ≥5%. Needs the user at the picocom
  console (payload board is user-only). Naive fails in ≤2 passes, so it is light on the agent.
- **(B)** stop the dataset here (the contrast is statistically clean) and move to write-up.
- Agent stability note (still true): silent death ~every 18 deploys; intermittent DTP `hard error` at
  setup. Reset accumulators between runs (see PAYLOAD BOARD). A more stable build would speed up (A).

## LIVE SETUP (host side, must be up for any measurement)
- Loopback broker: `build/proxy/zmqproxy-lossy -s tcp://127.0.0.1:6000 -p tcp://127.0.0.1:7000` (bg).
- Arm-A ground server: `upload_gs-server -z 127.0.0.1 -a 5424` **started with cwd=`disco/src/upload_gs-server/`**
  (it fopen's a relative `file.bin`). Launch: `env -C <that dir> <binary> -z 127.0.0.1 -a 5424`.
- `file.bin` there is the PINNED payload (256 KiB random, sha256 `2c1fa79a…`); canonical copy
  `captures/payload_256k.bin`. The `-f` arg to `upload_file` is IGNORED — payload = whatever file.bin is.
- can0 nodes: 15(host) 33(pcdu) 34(obc-payload) 5421(A53 mgr) 5424(my dipp server) 5426(dipp client) 5427(satdeploy agent).

## PAYLOAD BOARD (user-only, via picocom — `fsimx8mp`, BusyBox: no `head -c`, use `dd bs=8`)
- dipp receiver = node 5426 (`upload_client`). It EXITS after each transfer; restart by toggling
  `mng_util` on the A53 (see `scripts/restart-upload-client`).
- satDeploy agent = node 5427. User launches: `/home/root/satdeploy-agent-{smart,naive} -i CAN -p can0 -a 5427 > /tmp/sda.log 2>&1 &`.
  Both binaries deployed + sha256-verified on the board (smart `fbee1c32…`, naive `1843331d…`).
  Reset its accumulators after a death: `rm -rf /opt/satdeploy /var/lib/satdeploy /home/root/sd_*.bin`.

## SCRIPTS (`scripts/`)
- `upload-point <loss> <seed> <label>` — one dipp point (restart client, monitor=oracle B, injector=oracle A, fire upload_file, join).
- `dipp-sweep` — Arm A grid (done; resumable CSV).
- `restart-upload-client [client=5426] [server=5424]` — respawn dipp client via A53 mng_util toggle.
- `satdeploy-sweep <smart|naive>` (env MAX_PASSES/LOSSES/SEEDS) — Arm B passes-to-complete; pings agent
  per point, exits 2 cleanly if it dies (resumable).
- Injector invocation: `ci_inject_bridge zmq:6000,7000,<rxfilt> can:can0 <dport> <mtu> <overhead> <loss> <burst> <seed> <log> [src_addr] [pace_us]`.
  dipp: `8 256 4 ... 5424`; satDeploy: `8 256 8 ... 5425`.

## CRITICAL GOTCHAS (the expensive ones)
- **ALWAYS `pgrep -ax ci_inject_bridge` before any can0 run.** A stale bridge double-forwards (the broker
  is an XSUB/XPUB reflector) and silently invalidated the entire first dipp sweep. The scripts now guard this.
- Real session **MTU=256** (not 200). dipp header 4B, satDeploy header 8B → use `-O 4` / `-O 8`. 1041 dipp frags / 1058 satDeploy frags for 256 KiB.
- `meson.build`: `csp:buffer_count=4096` AND `csp:qfifo_len=4096` are REQUIRED (server bursts ~1024 KB/s into ~125 KB/s can0; defaults silently truncate).
- The bridge uses **split-horizon by src_addr** (argv[10]) to kill the echo loop (CRC dedup can't — cross-transport frames differ). `[pace_us]` (argv[11]) slows egress for reliable file deploys.
- csh headless: redirect `script` output to a FILE then grep it (piping into grep loses output when `timeout` kills it). `ping` is async → use `sleep`, not `exit`, to capture the reply.
- The deployed dipp **receiver corrupts files silently** (no end-to-end checksum; ftruncates to size but leaves stale bytes in dropped slots) — deploying the agent itself needed 6 accumulating passes + manual sha256. This is itself a thesis finding vs satDeploy's built-in verify.

## MEMORY FILES (auto-loaded; read for detail)
`bridge-split-horizon-fix`, `dipp-sweep-results`, `satdeploy-arm-deploy`, `satdeploy-arm-wiring`,
`restart-upload-client-a53`, `t4-stage0-baseline`, `driving-real-upload-measurement`, `flatsat-can0-bus-map`.
