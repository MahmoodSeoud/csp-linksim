# Upload-integrity experiments (csh-operator style)

Every experiment here is driven by the operator's own tool: a plain `.csh` init file
that csh runs top to bottom. What is committed is exactly what ran. Shell prep and the
external verdict run inside csh via the `sh` command (csp_shell APM).

```
~/thesis/csh/builddir/csh -i experiments/<file>.csh
```

Use the **patched csh** at `~/thesis/csh/builddir/csh`, not the one on `PATH`. The
`bridge` command and `csp_loss -R` pacing exist only there; see
`../docs/csh-bridge-injector/` for the patches and why each is needed.

## The three systems

| System | Files | Terminals | Why |
|---|---|---|---|
| Deployed uploader (DTP) | `dtp-injector.csh` + `dtp-upload.csh` | 2 | the data flows server→board and never crosses the operator's shell, so a shell has to be put on the path as a bridge |
| Reliable path (RDP) | `rdp-upload.csh` | 1 | csh's own `upload` sends the bytes |
| satdeploy, recovery build | `satdeploy-upload.csh` | 1 | `satdeploy upload` sends from the shell |

Each file is named for the command it runs: `dtp-upload.csh` runs `upload_file`,
`rdp-upload.csh` runs `upload`, `satdeploy-upload.csh` runs `satdeploy upload`.

Two terms, matching the thesis. A **system** is one transfer mechanism as built: the
deployed uploader, the reliable path, satdeploy's recovery build, satdeploy's naive build.
An **experiment** is one run of one system at one loss level and one seed, which is what
running one of these files produces. `dtp-injector.csh` is the exception: it is the
instrument, the only file that measures nothing on its own.

There are no separate baseline files. `var set LOSS 0.0` turns any file into its own clean
control, run through the identical path, which is a stronger control than a
differently-written file.

Parameters live in csh variables at the top of each transfer file:

```
var set LOSS 0.30
var set SEED 1
var set RATE 9600
```

csh has its own variable store with `$(NAME)` expansion; it does **not** read the shell
environment, so `LOSS=0.3 csh -i ...` silently expands to nothing. Either edit those
lines, or delete them and set the values interactively before `run`-ing the file, which
lets one file drive every cell. Setting `LOSS` to `0.0` turns a cell into its own clean
control, and **the control must pass before a lossy result means anything.**

### Where the loss is injected differs by system

For RDP and satdeploy, csh is the sender, so `csp_loss` drops packets out of this node's
own transmit path: one terminal, but the drops are **sender-side** and only affect the
forward direction, and the drop log is the injecting module's own count with no
independent monitor. For DTP the loss is imposed **mid-path** at the bridge, where the
promiscuous monitor can corroborate it. That difference is a real methodological
distinction, not a convenience: see `sec:eval-threats` in the thesis.

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
./upload_gs-server -z 127.0.0.1 -a 5426 &
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
- DTP: the on-board `upload_client` at 5424, spawned by the app-sys manager at 5421 via
  `mng_util` (which sets the client's own address; `mng_util_server` is the ground server
  it pulls from, 5426). It
  exits after every transfer, so `dtp-upload.csh` toggles `mng_util` through 0 to
  respawn it each run.
- satdeploy: `satdeploy-agent` on the board at 5427.

## Other files

- `rdp-tinyfile-loss.csh`, `rdp-tinyfile-sweep-loss.csh`, `rdp-tinyfile-verdict`, `mk-sentinel` —
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
