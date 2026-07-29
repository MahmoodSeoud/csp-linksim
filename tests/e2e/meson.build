# End-to-end tests for the front-ends (Linux/-Dfrontends only). ci_gen produces a
# fixed CSP stream (needs libcsp to frame packets); ci_sub is a discard subscriber
# that keeps the XSUB/XPUB subscription path exercised (needs only libzmq).
e2e_zmq = dependency('libzmq')
e2e_csp = dependency('csp', fallback: ['csp', 'csp_dep'])

ci_gen = executable('ci_gen', 'ci_gen.c',
  dependencies: [e2e_zmq, e2e_csp],
  override_options: ['c_std=gnu11', 'werror=false'])

ci_sub = executable('ci_sub', 'ci_sub.c',
  dependencies: [e2e_zmq],
  override_options: ['c_std=gnu11', 'werror=false'])

# Bound-guard generator: conforming RDP frames interleaved with oversized (>2054B)
# frames that must trip the proxy's recv->memcpy guard instead of overflowing it.
ci_oversize = executable('ci_oversize', 'ci_oversize.c',
  dependencies: [e2e_zmq, e2e_csp],
  override_options: ['c_std=gnu11', 'werror=false'])

# Proves the reproducibility gate: same seed + same stream -> identical drop vector.
# zmqproxy_lossy is the proxy target from proxy/meson.build (same meson scope).
test('proxy determinism (same seed -> identical drop vector)',
  find_program('determinism.sh'),
  args: [zmqproxy_lossy, ci_gen, ci_sub],
  timeout: 90)

# CRITICAL regression: the drop logic must not break XSUB->XPUB subscription
# forwarding (items[1]); otherwise the whole virtual bus silently blackholes.
test('xsub/xpub forwarding regression',
  find_program('forwarding.sh'),
  args: [zmqproxy_lossy, ci_gen, ci_sub],
  timeout: 60)

# Frame-size bound guard (TODOS#4): oversized wire frames must be rejected at the
# recv->memcpy, not overflow the reused packet buffer. Asserts the proxy survives,
# counts them malformed, and keeps forwarding/logging only conforming frames.
test('proxy frame-size bound guard (oversized frame must not overflow)',
  find_program('bound_guard.sh'),
  args: [zmqproxy_lossy, ci_oversize, ci_sub],
  timeout: 90)

# CLI boundary validation: an out-of-range -M (CSP dport 0-63) must be rejected, not
# silently match nothing and produce a falsely-lossless run.
test('proxy -M dport boundary validation',
  find_program('cli_guard.sh'),
  args: [zmqproxy_lossy],
  timeout: 60)

# APM start/stop E2E: stands up a libcsp node and #includes the monitor APM so it
# drives the real start/stop commands + drainer + drain helper headlessly (no csh).
# Guards ISSUE-001 (promisc clones must be drained on stop -> no buffer-pool leak)
# and the per-packet/#WINDOW CSV contract. csp is linked for the node runtime; the
# apm_csh/param includes are header-only (apm.h), so no unresolved dlopen symbols.
# slash is LINKED in full: the APM's start command uses optparse_* (provided by
# slash). The apm/ shared-object can take slash as header-only because the host csh
# resolves optparse at dlopen, but a standalone test exe must link it.
e2e_slash = dependency('slash',   fallback: ['slash', 'slash_dep'])
e2e_apm   = dependency('apm_csh', fallback: ['apm_csh', 'apm_csh_dep']).partial_dependency(includes: true, links: false)
e2e_param = dependency('param',   fallback: ['param', 'param_dep']).partial_dependency(includes: true, links: false)
e2e_threads = dependency('threads')

apm_host = executable('apm_host', 'apm_host.c',
  include_directories: include_directories('../../apm'),
  dependencies: [e2e_csp, e2e_slash, e2e_apm, e2e_param, ci_dep, e2e_threads],
  override_options: ['c_std=gnu11', 'werror=false'])

test('apm start/stop drain (no buffer-pool leak across cycles)', apm_host, timeout: 90)

# Two-oracle loop: a REAL libcsp node that joins the lossy proxy over zmqhub and runs
# the actual csp_monitor APM, vs the proxy's own drop-log. ci_gen publishes a
# deterministic RDP stream through the proxy; the proxy drops a reproducible subset by
# per-flow identity and logs it (oracle 1); the monitor observes the rest (oracle 2).
# two_oracle.sh asserts the two sets partition the sent seqs exactly (the MVP "whoa").
# Links zmq (zmqhub interface) on top of the same deps as apm_host.
ci_monitor_host = executable('ci_monitor_host', 'ci_monitor_host.c',
  include_directories: include_directories('../../apm'),
  dependencies: [e2e_csp, e2e_zmq, e2e_slash, e2e_apm, e2e_param, ci_dep, e2e_threads],
  override_options: ['c_std=gnu11', 'werror=false'])

test('two-oracle agreement (proxy drop-log vs live APM observed)',
  find_program('two_oracle.sh'),
  args: [zmqproxy_lossy, ci_gen, ci_monitor_host],
  timeout: 120)

# DTP counterpart: same two-oracle loop on the port-8 bulk data path. ci_gen_dtp
# publishes DTP data frames (LE uint32 byte-offset header, no RDP trailer); the proxy
# matches dport 8 and keys its drop on the fragment index offset/(mtu-4); the monitor
# runs the real APM in DTP mode (-d 8). Proves the DTP loss measurement end-to-end,
# closing the gap that the port-8 path was parsed + unit-tested but never run live.
# ci_dep gives ci_gen_dtp the ci_dtp.h header (CI_DTP_DATA_PORT).
ci_gen_dtp = executable('ci_gen_dtp', 'ci_gen_dtp.c',
  dependencies: [e2e_zmq, e2e_csp, ci_dep],
  override_options: ['c_std=gnu11', 'werror=false'])

test('two-oracle agreement DTP (proxy drop-log vs live APM observed, port 8)',
  find_program('two_oracle_dtp.sh'),
  args: [zmqproxy_lossy, ci_gen_dtp, ci_monitor_host],
  timeout: 120)

# satDeploy 8-byte DTP header (T5): the SAME loop with overhead=8 threaded through all
# three actors (proxy -H 8, generator step=mtu-8, APM monitor -O 8). Proves the monitor
# reads satDeploy's 8-byte frames end-to-end -- the hard prerequisite for measuring
# satDeploy. Disjoint ports (6110/7110) so it runs concurrently with the 4-byte instance.
test('two-oracle agreement DTP satDeploy 8-byte header (-O 8 / -H 8)',
  find_program('two_oracle_dtp.sh'),
  args: [zmqproxy_lossy, ci_gen_dtp, ci_monitor_host],
  env: {'OVERHEAD': '8', 'FRONT_PORT': '6110', 'BACK_PORT': '7110'},
  timeout: 120)

# In-path drop shim (CAN/KISS injection, TODOS#2): drives ci_drop_iface's nexthop
# directly with crafted RDP frames + a counting stub downstream -- no CAN hardware,
# no vcan, no root (the real-vcan demo needs sudo and is documented separately). Asserts
# the drop contract (free + CSP_ERR_NONE, no leak), determinism (same seed -> identical
# drop set), input partition (dropped XOR delegated), and out-of-scope passthrough.
# ci_drop_dep brings csp + ci + the shim; e2e_threads for csp's posix arch.
drop_iface_host = executable('drop_iface_host', 'drop_iface_host.c',
  dependencies: [ci_drop_dep, e2e_threads],
  override_options: ['c_std=gnu11', 'werror=false'])

test('in-path drop shim (deterministic CAN/KISS injection, leak-free, partitions input)',
  drop_iface_host, timeout: 60)

# CAN bench injector: opens a real socketcan iface (sandbox vcan0), wraps it with the
# drop shim, and transmits a deterministic RDP/DTP stream through it. NOT a registered
# test (needs vcan0 + root); built so scripts/can-bench can drive a live two-oracle loop
# over CAN with the csh monitor. ci_drop_dep brings csp + ci + the shim.
ci_inject_can = executable('ci_inject_can', 'ci_inject_can.c',
  dependencies: [ci_drop_dep, e2e_threads],
  override_options: ['c_std=gnu11', 'werror=false'])

# T4 loss injector: a CSP bridge (ingress zmq/can -> [T7 shim on egress] -> egress zmq/can)
# that drops a CSP-aware, per-attempt subset of the uplink before the real bus, with NO
# source changes to any uploader (it points its existing -z/-c flag at our ingress). Egress
# is selectable so the bridge+shim+oracle path is verifiable with no root (zmq egress) and
# then run for real against can0. Links zmq (zmqhub ingress/egress) + the shim/ci lib.
ci_inject_bridge = executable('ci_inject_bridge', 'ci_inject_bridge.c',
  dependencies: [ci_drop_dep, e2e_zmq, e2e_threads],
  override_options: ['c_std=gnu11', 'werror=false'])

# The half-duplex airtime model must actually charge airtime. The rate argument was
# accepted and silently ignored for a long time, so "19200 bit/s half-duplex" cells ran at
# line rate and every reverse-channel number derived from them was unbacked. Also guards the
# reverse_frames= summary field that svu-stageb and rdp-repro-5431 parse.
test('injector half-duplex airtime is charged per frame',
  find_program('airtime_guard.sh'),
  args: [zmqproxy_lossy, ci_gen, ci_inject_bridge],
  timeout: 120)

# vmem_node: a minimal CSP node serving a big flat RAM vmem region on a CAN bus, as a
# clean byte-faithful integrity oracle for the RDP/upload arm (DIPP stora is 10 KB and
# wedges; a csh instance serves no vmem). Full param dep for vmem_server_loop.
e2e_param_full = dependency('param', fallback: ['param', 'param_dep'])
vmem_node = executable('vmem_node', 'vmem_node.c',
  dependencies: [e2e_csp, e2e_param_full, e2e_threads, e2e_zmq],
  override_options: ['c_std=gnu11', 'werror=false'])

# rdp_push: drives libparam's vmem_upload/vmem_download (the RDP+CRC32 reference path) with
# no csh dependency, so the RDP arm runs behind the injector on any host rather than only on
# the bench. csh's `upload` is a thin wrapper over the same call.
rdp_push = executable('rdp_push', 'rdp_push.c',
  dependencies: [e2e_csp, e2e_param_full, e2e_threads, e2e_zmq],
  override_options: ['c_std=gnu11', 'werror=false'])
