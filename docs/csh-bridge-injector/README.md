# Loss injection from the csh prompt (`bridge` + `csp_loss`)

Result, 2026-08-25, FlatSat payload board over CAN, 256 KiB artifact, 9600 bit/s pacing:

| Cell | md5 on board | Size | Verdict |
|---|---|---|---|
| 0 % control | `a6e18f7c4531e124d81b36e7962288ba` | 262144 | MATCH (source) |
| 30 % loss, seed 1 | `e3131a31c4d19a64734c4895e6e220ed` | 262144 | MISMATCH, no error raised |

`csp_loss` counters for the lossy cell: `offered 1085, dropped 275, delivered 810 (25.35 % actual)`.
Source artifact `captures/payload_256k.bin` = `a6e18f7c4531e124d81b36e7962288ba`.

The deployed uploader delivered a size-correct, content-wrong binary under seeded loss, and the
same path delivered bit-perfect at 0 %. Both cells were driven entirely from the operator shell.

## Why this needed new code

`csp_loss` drops packets on the interface a node *transmits* from. The deployed DTP upload is a
**pull**: the trigger goes out from csh, but the 1041 data frames flow from `upload_gs-server`
(ZMQ side) to the on-board client (CAN side), never through the operator's csh. So csh had to
become the forwarding hop. Three things were missing, and all three are required:

1. **A bridge in the shell.** csh has no bridge command; `csp_bridge_work()` lives only in the
   separate `spacebridge` binary. Patch 01/02 add `bridge <ingress> <egress> [src_addr] | off`
   and make csh's existing router thread call `csp_bridge_work()` when a bridge is active.

2. **Source-based split-horizon.** The DTP metadata handshake is a *stateful RDP connection*
   (`csp_connect(CSP_PRIO_HIGH, node, 7, 5, CSP_O_RDP)`, port 7, 5 s timeout). A blindly
   forwarding bridge cannot carry it: the ZMQ broker is an XSUB/XPUB reflector, so every frame
   the bridge publishes returns on its own subscription and laps zmq->can->zmq. The symptom is
   the client re-sending its RDP SYN every 5 s forever, with no data phase. The fix (taken from
   `tests/e2e/ci_inject_bridge.c`) is to forward ingress frames only if they are **from** the
   data source, and egress frames only if they are **not** from it. Patch 02.

3. **Rate pacing.** Unpaced, the server's 1041 frames cross a 1 Mbit/s bus in under a second and
   the board's receiver is overrun, truncating the file at ~22 KB regardless of injected loss.
   Patch 03 adds `-R <bit/s>` to `csp_loss`, holding each frame for its airtime (dropped frames
   are charged too, as on a real link).

### Bus-conflict check (built into `csp_loss`)

Arming the injector now scans `/proc` and reports what would silently invalidate the cell,
because none of it is detectable from the network:

- a ground server started with `-c <dev>`, i.e. attached to the bus the board is on. It answers
  the on-board client directly, so the transfer never crosses the injector and the run reads as
  lossless. This is the single most expensive mistake on this bench.
- more than one `upload_gs-server`, where whichever replies first wins.
- a competing `ci_inject_bridge`, which duplicates every frame.

Matching is on the executable's basename and on real `argv` entries, so a wrapper shell that
merely mentions the name is not counted. The check reports and never refuses: an operator
exploring a link is a legitimate caller. `csp_loss status` repeats it on demand. A clean bus
prints `bus check clean (1 ground server, no competing injector)`.

Build config also matters (`meson configure builddir`): `buffer_count=8000`, `qfifo_len=4000`.
The server dumps all 1041 frames into the bridge in ~0.25 s while the bridge drains them at
~4.7 frames/s, so the default 1000-deep queue overflows and silently drops ~41 frames.

## Reproduction

Prerequisites: `zmqproxy` on 6000/7000; `upload_gs-server -z 127.0.0.1 -a 5424` started **from
its own builddir** (it serves `file.bin` from the cwd); board up on `can0`; **no other csh
router or bridge on can0**; no `upload_gs-server -c can0` anywhere (a server on the bus answers
the client directly and bypasses the injector entirely).

Apply the patches, then rebuild:

```
cd ~/thesis/csh
meson configure builddir -Dcsp:buffer_count=8000 -Dcsp:qfifo_len=4000
ninja -C builddir csh
ninja -C ~/thesis/csp-linksim/build apm/libcsh_csp_loss.so
```

**Injector shell** (`~/thesis/csh/builddir/csh`), in this order (the APM must be loaded and
`csp_loss` started before the ZMQ interface brings traffic in):

```
csp init
csp add can -p -c can0 5391
apm load -p /home/mseo/thesis/csp-linksim/build/apm
csp_loss start -i CAN0 -L 0.30 -S 1 -R 9600 -M 8
csp add zmq -m 14 5426 127.0.0.1
bridge ZMQ0 CAN0 5424
```

`-L 0.0` (and no `-M`) gives the clean control. `-M 8` keys drops to the DTP data port so the
RDP handshake on port 7 is left intact, which is what makes the loss attributable to the file
body. `-R 9600` is the pacing rate; the flight rate is 4800.

**Restart the on-board client before every transfer** (it exits after each one):

```
scripts/restart-upload-client 5426 5424
```

**Trigger shell** (a second csh):

```
csp init
csp add can -c can0 5395
apm load
upload_file -f /home/mseo/thesis/csp-linksim/captures/payload_256k.bin -d /home/root/csp_X.bin -n 5426 -s 5424
```

**Verify**, on the board over the serial console:

```
md5sum /home/root/csp_X.bin
```

against `md5sum captures/payload_256k.bin` on the ground.

## Gotchas that cost the most time

- **Wait ~240 s before hashing.** A paced 1041-frame transfer at 9600 bit/s takes about four
  minutes, and the client pre-allocates the destination at full size. Hashing early reports a
  full-size file with a wrong digest, which looks exactly like silent corruption but is not.
- **One bridge, one trigger shell.** Every extra csh on can0 is another CSP router; duplicates
  at the same address produce loops and unattributable drops.
- **`csp_loss status` is the ground truth** for what the shim actually saw (`offered/dropped`).
  A reading of `offered 0` means the frames are not crossing this node at all.

## Relationship to `ci_inject_bridge`

`tests/e2e/ci_inject_bridge` remains the instrument of record for the thesis campaign: it adds
the drop log, the promiscuous-monitor cross-check, airtime accounting and the harness-health
counters that make a cell citable. The shell path documented here reproduces its core mechanism
(transparent bridge + seeded drop + pacing) from the operator prompt, which is useful for
demonstration and for pointing the instrument at a link interactively.
