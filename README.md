# csp-intercept

**A fault-injection instrument for satellite file upload.** It sits in the path of a real
CSP file transfer, drops a known, repeatable fraction of packets, paces the link to the real
radio rate, and then checks whether the file actually arrived intact — independently of what
the software claims. It exists to answer one question that mission software hides:

> When the uplink is lossy, does your upload path deliver the file correctly, or does it
> report success while silently delivering a corrupt file?

CSP = CubeSat Space Protocol (`libcsp`). DTP = a bulk-transfer library on top of CSP
(`libdtp`). CSH = the CSP shell used to drive transfers. The lab CAN bus stands in for the
UHF radio: 4800 bit/s (confirmed by DISCO-2 flight telemetry), GMSK with Reed-Solomon(223,255)
FEC; operators throttle DTP to ~1 KB/s.

---

## 👉 Start here

**[docs/HOWTO.md](docs/HOWTO.md)** — the shortest path to *running* something: the unit tests,
a live bus watch, and the loss bench. Read this first if you just want to drive it.

---

## What it found

Run on the real DISCO-2 flatsat, the instrument produced these (data in `captures/`,
figures in `figures/`):

| Result | Evidence | Number |
|--------|----------|--------|
| **Calibration** — the injector drops exactly what it claims | `figures/calibration.*` | worst error 1.66 pp (under the 2 pp bound) |
| **H1** — fire-and-forget upload collapses under loss | `captures/dipp_sweep.csv` | completion 100% at 0% loss, **0% at any loss ≥2%** |
| **RQ3 (headline)** — flight software silently accepts corruption | `captures/rq3_corruption.csv` | **25/25** lossy uploads delivered a corrupt file reported as success; **0/5** at zero loss |
| **RQ4** — the cost of a real fix (sha256 verify-retry) | `captures/satdeploy_sweep.csv` | completes 0–30% loss at **≤1.7×** bytes |
| **H2** — libdtp resume also false-completes | `captures/rawdtp_sweep.csv` | **8/8** lossy transfers reported DELIVERED but failed checksum |

The throughline: mission software's "success" signal is unreliable under loss; only an
end-to-end checksum tells the truth, and this instrument is what measures it.

---

## How it works — the three oracles

Every measurement is cross-checked by three independent views, so a result is trustworthy
rather than anecdotal:

- **Oracle A (drop log)** — the injector records every fragment it dropped.
- **Oracle B (wire monitor)** — a promiscuous monitor records every fragment that actually
  crossed the bus.
- **Oracle C (sha256)** — the delivered file is hashed against the original. This is the only
  authority on *delivery*; A and B validate the *injection*.

The load-bearing invariant: every fragment is either dropped **or** observed, never both
(`max(dropped ∩ observed) = 0`). The test suite and the CAN bench both prove it.

---

## The pieces

| Folder / file | What it is |
|---------------|-----------|
| `lib/` | the brain — parses CSP/RDP/DTP, decides drops, computes loss, Gilbert-Elliott burst model. Pure C, unit-tested. |
| `apm/` | the **monitor** — a plugin loaded into the CSP shell (csh) to watch a bus (oracle B). |
| `proxy/` | the **ZMQ loss injector** — a lossy broker for virtual/lab buses. |
| `inject/` | the **CAN/KISS loss injector** — drops packets in-path on a real radio link. |
| `tests/e2e/ci_inject_bridge.c` | the **bridge injector** — ingress (zmq/can) → drop shim → egress, with the drop log. |
| `scripts/` | the run drivers (one transfer, and full sweeps). See the table below. |
| `captures/` | result CSVs and finding notes. |
| `figures/` | regenerable figures (`scripts/plot_calibration.py`). |

---

## How a human uses it

### 0. Build (once)

The one-command path, on any host with Docker (macOS included):

```sh
scripts/reproduce test      # build + the regression suite, expect 14/14 green
scripts/reproduce quick     # suite + two loss levels per reproducible arm (~6 min)
scripts/reproduce full      # suite + the six-level, three-seed sweeps (~1 h)
```

Or natively (Linux only; deps: `libzmq3-dev`, `libsocketcan-dev`, `pkg-config`, `python3`,
meson, ninja, a C toolchain):

```sh
git submodule update --init --recursive
meson setup build -Dfrontends=true
meson compile -C build
meson test -C build --print-errorlogs        # expect 14/14 green
```

### 1. Run one instrumented transfer

You normally do **not** call the injector by hand — the drivers wire it up. But this is the
raw interface so you know what is happening underneath:

```
ci_inject_bridge <in> <out> <dport> <mtu> <overhead> <loss> <burst> <seed> <drop.csv|-> [src_addr] [pace_us]
```

| arg | meaning |
|-----|---------|
| `in` / `out` | ingress / egress, e.g. `zmq:tcp://127.0.0.1:6000,tcp://127.0.0.1:7000,5426` and `can:can0` |
| `dport` | CSP dest port to act on (8 = DTP data plane) |
| `mtu` / `overhead` | fragment size and protocol header size, so the drop is fragment-accurate |
| `loss` | drop probability, 0.0–1.0 |
| `burst` | Gilbert-Elliott mean burst length (1 = independent loss) |
| `seed` | makes the drop set **deterministic and replayable** across runs/arms |
| `drop.csv` | oracle A output |

### 2. Sweep across loss levels

The drivers run a full grid (loss × seed), one real upload per cell, and append one row per
run to a CSV. They are **resumable** — an already-recorded `(loss, seed)` is skipped.

The laptop-reproducible arms (Docker only, via `scripts/bench`):

```sh
# Deployed uploader over DTP (needs the DISCO source tree, see scripts/dtp-host-sweep):
scripts/bench dtp-host-sweep

# Reliable vmem reference path (RDP + CRC32):
scripts/bench rdp-host-sweep

# SVU, the self-verifying uploader:
scripts/bench satdeploy-core-host-sweep
```

The flatsat arms (`scripts/satdeploy-board-sweep`, `scripts/dtp-board-point`) need the payload board
on can0; their results are committed under `captures/`. Unpaced cells are seconds each;
paced at the flight rate a 256 KiB pass is ~7 min (4800 bit/s, 1041 fragments).

### 3. Read the result

The CSV columns (real schemas):

```
dipp_sweep.csv      arm,loss,seed,label,sent,kept,dropped,realized,observedB,drop_obs,kept_obs,deliv_frac,status
satdeploy_sweep.csv arm,loss,seed,label,passes,total_injected,total_dropped,overhead_ratio,result,status
rawdtp_sweep.csv    arm,loss,seed,label,passes,total_injected,total_dropped,overhead_ratio,result,sha256_verdict,status
rq3_corruption.csv  loss,seed,label,bytes,delivered_sha256,matches_original,client_reported_success,corrupt_but_accepted
```

Regenerate the calibration figure with no bench needed:

```sh
python3 scripts/plot_calibration.py     # injector calibration
```

---

## Scripts reference

Naming convention: `<arm>-<place>-sweep` runs a full loss grid, where the arm is
the upload mechanism (`rdp`, `dtp`, `satdeploy`; `satdeploy-core` = the manifest
core measured standalone in the July era, before its adoption into satdeploy) and
the place is where it runs: `host` = loopback ZMQ in the container, no hardware;
`board` = the real payload board / flatsat CAN. `<arm>-<place>-point` runs one
instrumented cell. Everything else is an operational helper or a one-off
diagnostic, named for what it does. **Provenance note:** the committed CSVs in
`captures/` keep the names the drivers had when the data was taken (e.g.
`rdp-host-sweep` was `rdp-zmq-sweep` and writes `rdp_zmq_sweep.csv`;
`rdp-board-sweep` was `rdp-csploss-sweep` → `rdp_csploss_sweep.csv`;
`dtp-board-sweep` was `dtp-flight-sweep` → `dtp_flight_sweep.csv`;
`satdeploy-board-sweep` was `satdeploy-sweep` → `satdeploy_sweep.csv`;
`satdeploy-core-*` were `svu-*` — the code modules are still `svu_*` — and write
`svu_sweep.csv` / `h2h_svu.csv`).

**Entry points**

| Script | What it does |
|--------|--------------|
| `scripts/reproduce` | **start here** — build, regression suite, and every laptop-reproducible arm |
| `scripts/bench` | Docker wrapper: build/test/run any driver on any host, macOS included |
| `scripts/flatsat-build` | build the full mission stack (csh, satdeploy APM, agents, uploader) in the container |

**Sweep drivers (one per arm)**

| Script | What it does |
|--------|--------------|
| `scripts/dtp-host-sweep` | deployed-uploader (DTP) arm on loopback ZMQ; builds DISCO's sources |
| `scripts/rdp-host-sweep` | reliable vmem reference arm (RDP+CRC32) on loopback ZMQ |
| `scripts/satdeploy-core-host-sweep` | the manifest core (July era, standalone) on loopback ZMQ |
| `scripts/satdeploy-host-sweep` | satdeploy smart/naive arm on loopback ZMQ (matched head-to-head vs SVU) |
| `scripts/rdp-board-sweep` | RDP+CRC32 integrity sweep against the board `vmem_node` (csp_loss injection) |
| `scripts/dtp-board-sweep` | the pre-registered T1 grid: deployed uploader at flight pacing on the board (per-cell driver `dtp-board-point`) |
| `scripts/satdeploy-board-sweep` | satdeploy smart/naive sweep on the flatsat; agent on the payload board as 5427 |
| `scripts/satdeploy-core-board-sweep` | the manifest core against the payload board over can0 (July era) |
| `scripts/satdeploy-core-host-paced-sweep` | the manifest core under half-duplex airtime pacing (July era) |

**Diagnostics (back specific claims in the write-up)**

| Script | What it does |
|--------|--------------|
| `scripts/rdp-rate-compare` | paced-vs-unpaced RDP failure-mode comparison (same seeds, same drops) |
| `scripts/rdp-settle-check` | discriminates real corruption from verify-timing artifacts |
| `experiments/rdp-silent-*` | csh-operator silent-corruption cell (see `experiments/README.md`) |

**Flatsat operations**

| Script | What it does |
|--------|--------------|
| `scripts/can0-bench` | one-command two-oracle bench on the real flatsat CAN bus |
| `scripts/dtp-board-point` | one fully-instrumented loss point of the deployed arm, on the flatsat |
| `scripts/restart-upload-client` | respawn the deployed upload_client (it exits after each transfer) |
| `scripts/deploy-agent` | push the patched ARM agent to the payload board at loss=0 |
| `scripts/bringup-vmem-node` | stand up the vmem target node for the RDP arms |

**Analysis**

| Script | What it does |
|--------|--------------|
| `scripts/plot_calibration.py` | regenerate the injector calibration figure (no bench needed) |
| `scripts/parse_flight_dtp.py` | parse recorded flight DTP sessions into `captures/flight_dtp_*.csv` |
| `scripts/oracle_join.awk` | join drop-oracle and monitor logs per cell |
| `scripts/lib/host-sweep-lib.sh` | shared plumbing sourced by every `*-host-sweep` driver |

---

## What you gain from using it

- **A go/no-go on your own stack:** does it silently accept a corrupt file under realistic loss?
- **A trustworthy number, not a guess:** calibrated to under 2 pp, three oracles agree.
- **A fair A/B between mechanisms:** the same seed replays a byte-identical drop pattern across
  two uploaders, so comparisons are clean.
- **The cost of a fix:** does adding integrity checking help, and what does it cost in passes/bytes?
- **Evidence for a decision:** which uploader to fly, whether to add a checksum, how big a pass budget.

---

## Honest limits

- It is a **bench instrument**: it needs the upload stack reachable on a CAN bus (or loopback),
  a sender, and the broker running. It is not yet a one-click tool an operator runs in the cloud.
- Forward-path loss only; adversarial return-path loss and half-duplex RTT cost are future work.
- The raw-DTP arm receiver runs host-side (x86), not on the ARM payload — disclosed where it matters.
- Single mission, single bus, one payload size (256 KiB) and MTU (256): external validity is limited.

## Deeper detail

- [docs/HOWTO.md](docs/HOWTO.md) — run the tests / watch a bus / run the bench.
- [docs/can-kiss-injection.md](docs/can-kiss-injection.md) — how CAN/KISS loss injection works.
- [docs/dtp-metric.md](docs/dtp-metric.md) — why DTP loss is measured the way it is.

## Build status

Test suite: **14/14 green** (`scripts/bench test`, or `meson test -C build` on Linux).
Instrument validated on the real flatsat;
the empirical findings above are measured, not simulated.
