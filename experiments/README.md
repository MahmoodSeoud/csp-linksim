# Upload integrity experiments (csh-operator style)

Every experiment here is driven by the operator's own tool: a plain `.csh` init
file that csh runs top to bottom. What's committed is exactly what ran —
no harness binaries in the measurement path. Shell prep and the external
verdict run inside csh via the `sh` command (csp_shell APM).

```
~/thesis/csh/builddir/csh -i experiments/<cell>.csh
```

Naming convention — files sort by arm family:

- `<arm>-baseline.csh` — no-loss check that the arm works at all
- `<arm>-<cell>-<phase>.csh` — a measured cell; numbered phases run in order
- `<family>-verdict` — the bash external check a cell calls via `sh` (the
  verdict logic stays visibly *outside* the mechanism under test)
- `<family>-try` — bash driver that sequences a cell's phases and records rows
- `bringup-*` / `flatsat-*` — node bring-up and flatsat operations
- everything else (`mk-sentinel`, `prefill-check`, `host-rdp-infra`) — helpers
  named for what they do

## The RDP silent-corruption cell (host transport)

The flagship cell. csh's `upload` reports **queued** bytes, not delivered ones;
if the process exits right after the report (as any scripted upload does),
whatever the final RDP window still had in flight is silently lost. Two files,
run in order:

| Phase | File | What it does |
|---|---|---|
| 1 | `rdp-silent-1-upload.csh` | pre-fill region with 0xAA sentinel, upload the pinned 256 KiB payload under 20% seeded loss, **exit immediately** |
| 2 | `rdp-silent-2-readback.csh` | fresh csh (the sender is dead), read the region back over a clean bridge, print the verdict |

Phase 1's last line is csh printing `Uploaded 262144 bytes`. Phase 2 then says
whether that was true: `DELIVERED OK`, or `FILE WRONG` + `TAIL IS SENTINEL`
(= the final packets never arrived — silent corruption). The verdict logic
lives outside the mechanism, in `rdp-silent-verdict`.

**Why two files — this cannot be one .csh.** The corruption exists only because
the sender *process dies* before RDP's retransmit timer fires. A single csh
that uploaded and then read back would still be alive: the timer would quietly
repair the tail during the read-back and every run would come out intact (that
was the deleted `rdp-host-silent.csh`, which always self-repaired). The
one-command way to run both phases is `rdp-silent-try`.

Whether a given run lies is decided mostly by the seed's drop schedule (does it
hit the final window?) plus wall-clock retransmit jitter. Seed 4 lies often;
seed 8 has never lied. To sample across seeds and append run-tagged rows to
`captures/rdp_csh_silent.csv`:

```
experiments/rdp-silent-try              # one attempt, committed cell as-is (seed 4)
experiments/rdp-silent-try 4 6 15 16    # one attempt per seed (runs a /tmp copy)
```

Helpers (bash, called from the .csh via `sh`): `host-rdp-infra` (brokers,
receiver `vmem_node` @5431, seeded loss bridge — start/bridge/stop),
`mk-sentinel`, `prefill-check`, `rdp-silent-verdict`.

## The tiny-file cell (can0)

`rdp-tinyfile.csh` / `rdp-tinyfile-sweep.csh` / `rdp-tinyfile-verdict` — the
1-packet-transfer regime the on-orbit logs revealed. Result on record: can0
stays honest even here (see `captures/` data, committed 2026-08-11).

## Baseline checks and bring-up (board/flatsat)

From an initialised session (`csp init` + `csp add can` + `apm load`), chain
with `run`:

- `rdp-baseline.csh` — RDP `upload`/`download` against `vmem_node` 5431 (`bigmem`,
  1 MiB byte-faithful RAM; DIPP `stora` is too small and wedges — don't use it
  for this). Node up via `../scripts/bringup-vmem-node`.
- `dtp-baseline.csh` — deployed DTP push arm (`upload_file`); runs
  `bringup-upload-client.csh` first. Needs the ground gs-server (below).
- `satdeploy-baseline.csh` — the satdeploy manifest-core arm (`svu` csh command against
  `svu_daemon`; on the payload use the aarch64 build `build-arm/svu/svu_daemon`).
- `bringup-dipp.csh`, `bringup-upload-client.csh`, `flatsat-updown.csh` —
  node bring-up over the manager (5421).

Ground gs-server (one-time per session, not csh):

```
cd ~/thesis/disco/src/upload_gs-server/builddir
cp ~/thesis/csp-linksim/captures/payload_256k.bin file.bin
setsid nohup ./upload_gs-server -c can0 -a 5424 >/tmp/upsrv_can0.log 2>&1 </dev/null &
ps -eo pid,args | grep '[u]pload_gs-server'   # liveness check — NOT ping
```

Its `file.bin` must be byte-identical to `payload_256k.bin`, or the pull arms
compare against the wrong manifest and report a false FAILED.

## Loss sweeps

Controlled-loss sweeps that need the bash injectors live in `../scripts/`
(board: `rdp-board-sweep`, `satdeploy-board-sweep`; host RDP: `rdp-silent-try`
here). The `.csh` baseline checks run on live can0 — loss is whatever the
real link drops.
