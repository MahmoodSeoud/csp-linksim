# rdp-silent-2-verify.csh -- PHASE 2: read the region back and judge phase 1.
#
#     csh -i experiments/rdp-silent-2-verify.csh
#
# A fresh csh process, a CLEAN bridge (no loss), so the read-back cannot be
# blamed for anything. Phase 1's sender is long dead, which is the condition
# under test: whatever it left unacknowledged is gone for good.
#
# The verdict pairs with the line phase 1 printed:
#   "Uploaded 262144 bytes"  +  a wrong file whose tail is still 0xAA
#   = the reliable path reported success over data it never delivered.

csp init
apm load
sh /home/mseo/thesis/csp-linksim/experiments/host-rdp-infra bridge 0 1
csp add zmq -d 26 127.0.0.1

download -n 5431 -v 2 -t 120000 0x10000000 262144 /tmp/hostgot.bin
sleep 1000
sh /home/mseo/thesis/csp-linksim/experiments/rdp-silent-verdict
sh /home/mseo/thesis/csp-linksim/experiments/host-rdp-infra stop
exit
