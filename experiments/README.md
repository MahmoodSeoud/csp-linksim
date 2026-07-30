# Upload integrity experiments (csh)

Each experiment is a single `.csh` you `run` from your csh session — it does the whole
clean-link check (bring up the node, upload, read back, verify) and writes its own
received file, so the arms are directly comparable. **This folder is `.csh` only.**
Controlled **loss sweeps** (which need the bash loss injector) live in `../scripts/`.

Run from an initialised session (`csp init` + `csp add can` + `apm load` already done):

```
run /home/mseo/thesis/csp-intercept/experiments/exp_rdp.csh
run /home/mseo/thesis/csp-intercept/experiments/exp_upload_file.csh
run /home/mseo/thesis/csp-intercept/experiments/exp_svu.csh
```

| Arm | File | Uploader | Target | Verify |
|---|---|---|---|---|
| CSH RDP | `exp_rdp.csh` | `upload`/`download` (vmem) | node 5431 `bigmem` | expect **OK** |
| Deployed DTP (push) | `exp_upload_file.csh` | `upload_file` | payload file | silently corrupts under loss |
| SVU | `exp_svu.csh` | `svu` (csh command) | `svu_daemon` | expect **VERIFIED** |
| ~~Deployed DTP (pull)~~ | (deleted) | — | — | **RETIRED** — `dtp_client` is the DIPP downlink proxy, not the deployed upload path; use the push arm |

The `svu` command is scp/cp-like: `svu -p <src> <dest>` uploads to the **default node**
(set once with `node <addr>`), or `svu -p <src> <node>:<dest>` to name it explicitly.
`-p` preserves the source mode (so a deployed binary lands executable); `svu_daemon`
answers `ping` like any node.

**RDP arm uses a purpose-built RAM oracle, not DIPP `stora`.** `stora` is only 10 KB
(overflows a 256 KiB upload) and wedges under retransmit stress — both produce *false*
"corruption". `vmem_node` (in-project `tests/e2e/vmem_node.c`) serves a 1 MiB flat RAM
`bigmem` region at node 5431 that is byte-faithful and won't wedge. Start it with
`../scripts/bringup-vmem-node`, then run `exp_rdp.csh` (clean check) or the loss sweep
`../scripts/rdp-bigmem-sweep`. Full finding + data in
`~/thesis/MasterThesis---SatDeploy/notes/rdp-integrity-finding.md`.

`svu <src> <node>:<dest>` is the scp-like SVU uploader as a native csh command (APM
`libcsh_svu.so`, loaded by `apm load`). The destination node runs `svu_daemon` (the
receiver); on the payload use the aarch64 build `build-arm/svu/svu_daemon`.

`verify` is the proof (did the file arrive intact); the monitor CSV is context (how
lossy the link was). The monitor number is trustworthy only for a bulk transfer
captured whole — on RDP it double-counts retransmits, so treat RDP's figure as
indicative; RDP's real result is `verify → OK`.

## Node bring-up (also csh, chained via `run`)

The experiments pull these in automatically, but you can run them alone too:

- `bringup_dipp.csh` — turns on the DIPP node (5423) via `mng_dipp` on the A53 manager
  (5421).
- `bringup_upload_client.csh` — turns on the Upload-Client (5426) via `mng_util` on 5421.
  `exp_upload_file.csh` runs this first.

## One-time ground infra (not csh — a server binary, started once)

The two DTP arms pull from the ground gs-server. Start it once per session:

```
cd ~/thesis/disco/src/upload_gs-server/builddir
cp ~/thesis/csp-intercept/captures/payload_256k.bin file.bin
setsid nohup ./upload_gs-server -c can0 -a 5424 >/tmp/upsrv_can0.log 2>&1 </dev/null &
ps -eo pid,args | grep '[u]pload_gs-server'    # liveness check — NOT ping (the server ignores ping)
```

Its `file.bin` must be the same bytes as `payload_256k.bin`, or the pull arms' `verify`
compares against the wrong manifest and reports a false FAILED.

## Loss

The `.csh` clean-link checks run on live can0, so loss is only what the real link drops.
To sweep controlled loss levels, use the injector-based bash drivers in `../scripts/`:
the RDP arm is `scripts/rdp-bigmem-sweep` (with `scripts/bringup-vmem-node` +
`scripts/rdp-repro-5431`); the other arms use the `scripts/*-sweep` bench.
