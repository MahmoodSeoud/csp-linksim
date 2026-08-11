# rdp-forcesilent.csh -- can RDP be made to corrupt SILENTLY on the FlatSat bus?
#
# SINGLE-FILE experiment: prep, bus commands, and verdict all here, replayable
# with (from the repo root, short path -- csh truncates long -i paths):
#
#     csh -i experiments/rdp-forcesilent.csh
#
# Background: the csp_loss board sweep showed RDP aborts loudly on can0 under
# loss (honest). This cell makes the connection unkillable (long conn/packet
# timeouts) so the send loop can finish while the connection lives -- the
# condition where vmem_upload returns full success over a truncated region
# (vmem_client.c:159-185, RST-at-close csp_rdp.c:892).
#
# Prereqs: vmem_node up at 5431 on can0. Shell commands run via the csp_shell
# APM's `sh` (loaded by `apm load` from ~/.local/lib/csh).
#
# THE FORK to watch on the second upload:
#   "Uploaded 262144 bytes"        -> silent candidate (check the sha below)
#   "Upload didn't complete, ..."  -> loud even unkillable (also a result)

csp init
csp add can -c can0 -b 0 -d 20
apm load

# -- prep: 0xAA sentinel (known pattern; "still 0xAA" == never delivered)
sh python3 -c "open('/tmp/sentinel_AA.bin','wb').write(bytes([170])*262144)"
sh rm -f /tmp/pre.bin /tmp/got.bin

# -- 1. sentinel pre-fill over the CLEAN link, then prove the region is 0xAA
upload -n 5431 -v 2 -t 10000 /tmp/sentinel_AA.bin 0x10000000
sleep 3000
download -n 5431 -v 2 -t 10000 0x10000000 262144 /tmp/pre.bin
sleep 1000
sh "cmp -s /tmp/pre.bin /tmp/sentinel_AA.bin && echo PREFILL-CLEAN-no-prior-garbage || echo PREFILL-DIRTY-TEST-INVALID"

# -- 2. unkillable connection, then the payload under seeded loss (be patient:
#       20 s packet timeouts under 30% loss can stall minutes; cap is 600 s)
rdp opt -w 5 -c 600000 -p 20000 -k 1000
csp_loss start -L 0.30 -S 1
upload -n 5431 -v 2 -t 600000 /home/mseo/thesis/csp-linksim/captures/payload_256k.bin 0x10000000
sleep 6000
csp_loss status
csp_loss stop

# -- 3. connection teardown, then verify over the clean link
sleep 15000
crc32 -n 5431 -v 2 -f /home/mseo/thesis/csp-linksim/captures/payload_256k.bin 0x10000000
sleep 500
download -n 5431 -v 2 -t 10000 0x10000000 262144 /tmp/got.bin
sleep 1000

# -- 4. verdict, external to the mechanism (eyeball + strict machine check)
sh sha256sum /home/mseo/thesis/csp-linksim/captures/payload_256k.bin /tmp/got.bin
sh "cmp /home/mseo/thesis/csp-linksim/captures/payload_256k.bin /tmp/got.bin || true"
sh "xxd /tmp/got.bin | tail -5"
sh /home/mseo/thesis/csp-linksim/experiments/rdp-forcesilent-verify
exit
