project('csp-intercept', 'c',
  version: '0.1.0',
  default_options: [
    'default_library=static',   # self-contained proxy/APM, no libcsp.so at runtime
    # --- vendored CSH libs (only consumed when -Dfrontends=true) ---
    # We deliberately do NOT impose our strict c_std/-Werror globally: the vendored
    # libs need gnu11 (glibc _DEFAULT_SOURCE for be16toh/strnlen) and their own
    # warning tolerance. Our own targets re-impose strict c11 + -Werror via the
    # `ci_strict` override_options below. werror=false here overrides each lib's own
    # project() default (csp sets werror=true on itself).
    'csp:werror=false',
    'param:werror=false',
    'slash:werror=false',
    'apm_csh:werror=false',
    # libcsp build options. conn_rxqueue_len is LOAD-BEARING: csp_promisc_enable()
    # ignores its argument, so the promisc queue depth is fixed here at build time
    # (eng review Arch-4). use_promisc is required for the monitor.
    'csp:packet_padding_bytes=42',
    # 4096, not 1000: a fire-and-forget sender bursts the whole transfer (1041 packets
    # for 256 KiB) into the bridge faster than a paced egress drains it, so the pool must
    # hold a full transfer or packets die BEFORE the drop shim and oracle A undercounts.
    # The June bench host ran with exactly this override ("REQUIRED ... defaults
    # silently truncate") but it was never committed; the 4800 bit/s paced DTP arm is
    # where the truncation finally reproduced locally (26 of 1041 packets seen).
    'csp:buffer_count=4096',
    'csp:buffer_size=2048',
    'csp:conn_max=20',
    'csp:conn_rxqueue_len=1000',
    'csp:qfifo_len=4096',
    'csp:rdp_max_window=1000',
    'csp:port_max_bind=16',
    'csp:use_rtable=true',
    'csp:use_promisc=true',
  ])

# Strictness applied to OUR code only (the pure lib + tests). Portable C11, all
# warnings as errors -- this is the code we own and police.
ci_strict = ['c_std=c11', 'warning_level=2', 'werror=true']

# Shared parse / rule / measurement library (eng review decision 4: one source of
# truth, linked by both the monitor APM and the lossy proxy). Pure C, no libcsp in
# the parse/rule/measurement layer, so it is trivially unit-testable.
ci_inc = include_directories('lib')
ci_lib = static_library('ci',
  ['lib/ci_rdp.c', 'lib/ci_dtp.c', 'lib/ci_rule.c', 'lib/ci_meas.c', 'lib/ci_ge.c', 'lib/ci_sha256.c', 'lib/ci_svu.c'],
  include_directories: ci_inc,
  override_options: ci_strict,
  install: false)

ci_dep = declare_dependency(link_with: ci_lib, include_directories: ci_inc)

# Unit tests (dependency-free).
test_lib = executable('test_lib', 'tests/test_lib.c',
  dependencies: ci_dep,
  override_options: ci_strict)
test('lib unit tests', test_lib)

# Self-Verifying Uploader core: proves the block-integrity + verified-completion
# tracker reports CORRUPT (with the exact re-request interval) exactly where the
# old DTP byte-counter would say "delivered". Pure lib, no frontends needed.
ci_svu_test = executable('ci_svu_test', 'tests/ci_svu_test.c',
  dependencies: ci_dep,
  override_options: ci_strict)
test('self-verifying uploader core', ci_svu_test)

# Gilbert-Elliott burst-loss generator (T3): marginal loss + mean burst length
# track the target, deterministic per seed, and the generated vector drives the
# rule's replay path. Pure lib, no frontends needed.
ge_cc = meson.get_compiler('c')
ge_m = ge_cc.find_library('m', required: false)
ge_deps = [ci_dep]
if ge_m.found()
  ge_deps += ge_m
endif
ge_host = executable('ge_host', 'tests/ge_host.c',
  dependencies: ge_deps,
  override_options: ci_strict)
test('gilbert-elliott burst-loss generator', ge_host)

# SVU resume loop: end-to-end proof that sender-manifest + burst-loss/corrupting
# channel + verified-completion receiver converges to a block-verified file (bytes
# == source), re-requesting only the bad ranges. Uses ci_ge (needs -lm), so it sits
# in the ge_deps block.
svu_loop_host = executable('svu_loop_host', 'tests/svu_loop_host.c',
  dependencies: ge_deps,
  override_options: ci_strict)
test('self-verifying uploader resume loop', svu_loop_host)

# Injector calibration: the drop vector this C rule produces must match the offline
# Python reimplementation the thesis calibrates against (scripts/calibrate-injector.py).
# Nothing enforced that before: a change to ci_draw() or to the threshold arithmetic would
# silently invalidate every predicted drop count in the evaluation while both sides kept
# running happily.
calibrator_host = executable('calibrator_host', 'tests/calibrator_host.c',
  dependencies: ci_dep,
  override_options: ci_strict)
test('injector calibration matches the offline reimplementation',
  find_program('tests/calibrator_diff.sh'),
  args: [calibrator_host, files('scripts/calibrate-injector.py')])

# Front-ends: the vendored lossy proxy and the monitor APM. Gated behind the
# `frontends` option so the pure lib above stays buildable with zero dependencies.
#
# Linux-only: libcsp (and CSH) target Linux/embedded; the instrument runs on the
# Linux ground station. We deliberately do NOT carry macOS-host shims for libcsp
# (it needs ELF sections, <endian.h>, etc.). On macOS build the front-ends via
# Docker or CI (see scripts/bench). The pure lib above still builds everywhere.
if get_option('frontends')
  if host_machine.system() != 'linux'
    error('frontends build is Linux-only (libcsp does not build on @0@). '
      .format(host_machine.system())
      + 'Use scripts/bench (Docker) or CI. The pure lib builds here with the default -Dfrontends=false.')
  endif
  subdir('proxy')
  subdir('apm')
  subdir('inject')
  subdir('svu')
  subdir('tests/e2e')
endif
