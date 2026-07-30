# HOWTO — what to run

(New here? Read the [README](../README.md) first for what this is. This page is just
the commands.)

## First-time setup (once per clone)

```sh
git submodule update --init --recursive   # pulls the vendored CSH deps (csp/slash/param/apm_csh)
```

Native deps (Debian/Ubuntu): `sudo apt install libzmq3-dev libsocketcan-dev libbsd-dev pkg-config python3 meson ninja-build build-essential`.

That's it. The test suite needs nothing else. The CAN bench (#3) additionally needs a
**csh binary** — `scripts/can0-bench` auto-finds it at the usual DISCO2 locations; if
yours is elsewhere, run it as `CSH=/path/to/csh scripts/can0-bench`.

## Install the monitor for csh (`./install`)

To use the monitor from a real csh session, install it where csh's `apm load` looks
(`~/.local/lib/csh`), same as dipp-apm/csh:

```sh
./install
```

This builds the front-ends and drops `libcsh_csp_monitor.so` into `~/.local/lib/csh/`
and `zmqproxy-lossy` into `~/.local/bin/`. Then in csh: `apm load` finds the monitor.
(If `~/.local/lib/csh` is root-owned, run `sudo chown -R $USER ~/.local/lib` once first.)

## The three things you actually run

### 1. Run the test suite (proves everything compiles + works)

```sh
meson setup build -Dfrontends=true   # first time only
meson compile -C build
meson test -C build --print-errorlogs
```

Expect **14/14 OK**. This is synthetic (no hardware): lib unit tests, proxy determinism,
the APM drain test, RDP + DTP two-oracle loops, the in-path drop shim, and the half-duplex
airtime guard. Run this after any code change. On macOS use `scripts/bench test` instead
(Docker); the front-ends do not build natively there.

### 2. Monitor a real CAN bus from a csh session

Drive the **real csh** with the committed init script; it joins `can0`, loads the
monitor, starts capturing, and drops you at the `csh #` prompt. Point `$CSH` at your
csh binary (the DISCO2 build is usually at `~/disco/src/csh/builddir/csh`):

```sh
CSH=~/disco/src/csh/builddir/csh
"$CSH" -i csh/init/can-monitor.csh
```

Note: `can0` must be up first (the DISCO2 `caninit` script does this). The monitor
loads the APM from `build/apm/`, so run the test-suite build (#1) at least once before this.

At the prompt: `info` shows `CAN0 rx:` climbing as real frames arrive;
`csp_monitor stop` flushes the CSV; Ctrl-D exits. Watch the capture live from another
terminal:

```sh
tail -f captures/can0_live.csv
```

NOTE: the monitor only writes rows for **RDP/DTP** traffic. Routine flatsat housekeeping
is neither, so `rx:` can climb while the CSV stays sparse — that is the instrument
staying scoped to the RDP-vs-DTP study, not a bug. To get rows, either run a real DTP
transfer on the bus, or use the bench (#3) which generates in-scope traffic.

### 3. Run the two-oracle bench on the real flatsat CAN bus

One command: builds, runs the csh monitor + the in-path injector on `can0`, joins the
two oracles, prints PASS/FAIL.

```sh
scripts/can0-bench            # RDP (port 13)
scripts/can0-bench dtp        # DTP bulk (port 8)
scripts/can0-bench rdp 0.5 7  # protocol, loss probability, seed
```

Safe on the flatsat by construction: our flow is **src=10 → dst=20** (both free; the
occupied addresses are 0/15=this host, 33/63=pcdu, 34=obc-payload). dst=20 doesn't
exist, so no real node processes our frames; the shim drops only our own TX; the join
filters the monitor's CSV to `src==10` so live DISCO2 traffic can't pollute the result.
No root needed (`-b 0` skips the privileged CAN bitrate set).

---

## Where things live

| Path | What |
|------|------|
| `lib/` | shared parse + drop rule + measurement (pure C, unit-tested) |
| `apm/` | the monitor APM (`libcsh_csp_monitor.so`) |
| `proxy/` | the lossy ZMQ proxy |
| `inject/` | the in-path CAN/KISS drop shim |
| `tests/e2e/` | the 9-test suite: `.sh` drivers + `ci_*` traffic generators / harnesses |
| `scripts/can0-bench` | the real-CAN two-oracle bench (#3) |
| `scripts/oracle_join.awk` | the drop-log-vs-observed join helper |
| `csh/init/can-monitor.csh` | csh init for monitoring `can0` (#2) — lives in the `csh` repo |
| `captures/` | CSV output (gitignored) |
| `docs/can-kiss-injection.md` | how CAN/KISS injection works + transport matrix |
| `TODOS.md` | what's left (field work) |
