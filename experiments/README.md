# Upload-integrity experiments (csh-operator style)

Every experiment here is driven by the operator's own tool: a plain `.csh` init file
that csh runs top to bottom. What is committed is exactly what ran. Shell prep and the
external verdict run inside csh via the `sh` command (csp_shell APM).

```
~/thesis/csh/builddir/csh -i experiments/<cell>.csh
```

Use the **patched csh** at `~/thesis/csh/builddir/csh`, not the one on `PATH`. The
`bridge` command and `csp_loss -R` pacing exist only there; see
`../docs/csh-bridge-injector/` for the patches and why each is needed.

## The three arms

| Arm | Files | Terminals | Why |
|---|---|---|---|
| Deployed uploader (DTP) | `dtp-1-injector.csh` + `dtp-2-cell.csh` | 2 | the data flows server→board and never crosses the operator's shell, so a shell has to be put on the path as a bridge |
| Reliable path (RDP) | `rdp-baseline.csh`, `rdp-cell.csh` | 1 | csh's own `upload` sends the bytes |
| satdeploy (full tool) | `satdeploy-baseline.csh`, `satdeploy-cell.csh` | 1 | `satdeploy push` sends from the shell |

Each `*-cell.csh` has a single marked line to edit for loss, seed, burst and rate.
Setting `-L 0.0` turns any cell into its own clean control, and **the control must pass
before a lossy result means anything.**

## Choosing the drop mode: `-M` or not

This is the one setting that is easy to get wrong.

- **DTP uses `-M 8`.** Identity-keyed drops on the data port, so the RDP metadata
  handshake on port 7 survives and the failure is attributable to loss on the file
  body. The deployed path is fire-and-forget: it never retransmits, so there is
  nothing for identity-keyed drops to block.
- **RDP and satdeploy must NOT use `-M`.** Both retransmit. `-M` keys the drop to a
  packet's identity, so the same fragment is dropped on every resend and recovery can
  never converge — you would be measuring the injector, not the mechanism. Use
  per-transmission loss: plain `-L`, or `-L` with `-B` for a bursty channel.

## Validity gate

After every run, in the injector shell:

```
csp_loss status
```

`offered` must equal the number of packets the sender reported. If it is lower, frames
went missing before the injector and the cell is void whatever the md5 says. Two causes
seen on this bench: a ground server started with `-c can0` (it answers the board
directly and bypasses the injector entirely — `csp_loss` warns about this when it arms),
and ZeroMQ's default 1000-message high-water mark silently discarding the tail of a
transfer that a paced bridge drains slower than the sender fills.

Second gate: **wait for the transfer to finish before hashing.** A paced 256 KiB
transfer takes ~230 s at 9600 bit/s, and the client pre-allocates the destination at
full size, so an early hash reports a full-size file with a wrong digest — exactly what
the finding looks like, but wrong.

## Ground infrastructure (once per session, not csh)

```
zmqproxy &                                    # defaults to 6000/7000
cd ~/thesis/disco/src/upload_gs-server/builddir
./upload_gs-server -z 127.0.0.1 -a 5424 &
```

The DTP server **hardcodes `file.bin`** (`vmem_dtp_server.c`) and ignores `upload_file
-f`, which is trigger metadata only. Whatever `file.bin` sits in that builddir is what
ships, so it must be the artifact you intend to compare against. A smaller `file.bin`
is the way to make runs quick: 32 KiB is ~29 s against ~230 s for 256 KiB.

Start the server on `-z` (ZMQ). Started with `-c can0` it answers the on-board client
directly on the bus and no injector can see the transfer.

## Receivers

- RDP: `vmem_node` at 5431 (`bigmem`, 1 MiB byte-faithful RAM) — `scripts/bringup-vmem-node`.
  DIPP's `stora` is too small and wedges; do not use it for this.
- DTP: the on-board `upload_client` at 5426, spawned by the app-sys manager at 5421. It
  exits after every transfer, so `dtp-2-cell.csh` toggles `mng_util` through 0 to
  respawn it each run.
- satdeploy: `satdeploy-agent` on the board at 5427.

## Other files

- `rdp-tinyfile.csh`, `rdp-tinyfile-sweep.csh`, `rdp-tinyfile-verdict`, `mk-sentinel` —
  the one-packet-transfer regime the on-orbit logs revealed, with a 0xAA sentinel
  control so "the bytes never arrived" is proven rather than inferred. Result on
  record: can0 stays honest even here.
- `bringup-dipp.csh`, `bringup-upload-client.csh` — node bring-up over the manager (5421).
- `flatsat-updown.csh` — pass rehearsal against the payload board (iface-name discovery,
  `csp_link` direction split, monitor capture, calibrated replay). Note it sets
  `mng_dipp_vmem_path`; a value whose path does not exist leaves DIPP unable to start
  until the path is restored and `mng_dipp` cycled.

Loss sweeps that need the bash injector live in `../scripts/` (`rdp-board-sweep`,
`satdeploy-board-sweep`, `dtp-board-point`, `csh-loss-cell`).
