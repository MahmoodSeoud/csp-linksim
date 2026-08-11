# rdp-host-silent.csh -- the reliable path REPORTING SUCCESS OVER A WRONG FILE,
# driven entirely from csh, with the operator's own `upload` command.
#
#     csh -i experiments/rdp-host-silent.csh        (run from the repo root)
#
# WHY NOT can0: on the flight bus this same code is honest -- the connection
# dies while the send loop is still running, so vmem_upload returns a short
# count and csh prints "Upload didn't complete". 38/38 board cells did exactly
# that. The lie needs the send loop to FINISH with packets still unacknowledged;
# that happens on a transport fast enough to outrun the connection. This cell
# creates that regime on a loopback segment and leaves everything else identical:
# same csh, same `upload`, same vmem_upload, same seeded loss shim.
#
# WHAT TO WATCH, in order:
#   1. "PREFILL-CLEAN"            the region is exactly 0xAA before we start
#   2. "Uploaded 262144 bytes"    <- THE CLAIM, from the operator's own tool
#   3. the verdict block          the delivered file differs, and the tail is
#                                 still 0xAA: whole packets never arrived
#
# Change the loss/seed on the `sh ... bridge` line below to try other cells.

# `apm load` comes FIRST: the `sh` command lives in the csp_shell APM, and
# without it every sh line below is silently "No such command" -- the infra
# never starts and the cell measures nothing while still looking like it ran.
csp init
apm load
sh /home/mseo/thesis/csp-linksim/experiments/host-rdp-infra start
csp add zmq -d 26 127.0.0.1
ping -n 5431 -t 5000

# -- prep: 256 KiB 0xAA sentinel, so "still 0xAA" proves non-delivery
sh /home/mseo/thesis/csp-linksim/experiments/mk-sentinel 262144 /tmp/sentinel_256k.bin
sh rm -f /tmp/hostpre.bin /tmp/hostgot.bin

# -- 1. sentinel pre-fill over the CLEAN bridge, then prove the region is 0xAA
upload -n 5431 -v 2 -t 120000 /tmp/sentinel_256k.bin 0x10000000
sleep 2000
download -n 5431 -v 2 -t 120000 0x10000000 262144 /tmp/hostpre.bin
sleep 1000
sh /home/mseo/thesis/csp-linksim/experiments/prefill-check /tmp/hostpre.bin /tmp/sentinel_256k.bin

# -- 2. THE MEASURED UPLOAD, 20% seeded loss (seed 2). Watch the line it prints.
#    The connection must survive long enough for the send loop to FINISH -- that
#    is the whole condition for the lie. With csh's stock 10 s conn_timeout the
#    connection dies mid-loop and the tool honestly reports a short count
#    ("didn't complete at 43584"), which is the same honest path can0 takes.
#    These are the `tuned` options the board sweep also used.
rdp opt -w 3 -c 120000 -p 5000 -k 2000
sh /home/mseo/thesis/csp-linksim/experiments/host-rdp-infra bridge 0.20 2 /tmp/hostdrops.csv
upload -n 5431 -v 2 -t 120000 /home/mseo/thesis/csp-linksim/captures/payload_256k.bin 0x10000000
sleep 3000

# -- 3. read back over a CLEAN bridge and judge from outside the mechanism
sh /home/mseo/thesis/csp-linksim/experiments/host-rdp-infra bridge 0 1
download -n 5431 -v 2 -t 120000 0x10000000 262144 /tmp/hostgot.bin
sleep 1000
sh /home/mseo/thesis/csp-linksim/experiments/rdp-host-silent-verify

sh /home/mseo/thesis/csp-linksim/experiments/host-rdp-infra stop
exit
