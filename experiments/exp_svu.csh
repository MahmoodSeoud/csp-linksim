# exp_svu.csh - the SVU arm: scp-like upload to a node, block-verified. Runnable + reproducible.
#
# Pushes the pinned 256 KiB payload to the payload board (which runs svu_daemon) and the
# daemon reports VERIFIED (landed intact) or FAILED (never a false success). `svu` is the
# APM command libcsh_svu.so; `-p` preserves the source mode. We set the board as the
# default node once (`node 5426`), so the push reads like `cp` -- no node in the command.
#
#   run /home/mseo/thesis/csp-linksim/experiments/exp_svu.csh
#
# PREREQ: svu_daemon running on the destination board (aarch64 build):
#   build-arm/svu/svu_daemon -c can0 -a 5426     (on the payload board)
# Optional monitor: SVU bulk data rides port 9.
csp init
csp add can -d 20
apm load
node 5426
csp_monitor start -d 9 -m 256 -o svu_monitor.csv
svu -p /home/mseo/thesis/csp-linksim/captures/payload_256k.bin /home/root/svu_uploaded.bin
csp_monitor stop
