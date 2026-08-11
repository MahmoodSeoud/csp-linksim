# rdp-forcesilent-small.csh -- second attempt to force SILENT RDP corruption on can0.
#
#     csh -i experiments/rdp-forcesilent-small.csh      (run from the repo root)
#
# WHY THIS DIFFERS from rdp-forcesilent.csh (which aborted loudly at offset 1920):
# RDP timeouts are process-global statics in libcsp (csp_rdp.c:36-41), so `rdp opt`
# only reconfigures THIS csh -- the receiver (vmem_node on the board) keeps the
# defaults, conn_timeout = 10 s. The first attempt set packet_timeout = 20 s, so
# the sender sat waiting to retransmit while the RECEIVER's connection expired:
# guaranteed loud abort. The silent regime needs the send loop to FINISH before
# either end's connection dies.
#
# So: shrink the artifact and retransmit fast. 32 KiB at ~28 KB/s is ~1.2 s clean,
# and with 200 ms packet timeouts even 10% loss should complete well inside the
# receiver's 10 s window -- the condition where vmem_upload returns the full byte
# count while the last unacked window is RST'd away at close
# (vmem_client.c:159-185, csp_rdp.c:892).
#
# Prereqs: vmem_node up at 5431 on can0.
#
# THE FORK to watch on the second upload:
#   "Uploaded 32768 bytes"        -> SILENT candidate: check the verdict below
#   "Upload didn't complete, ..." -> still honest even in the fastest regime

csp init
csp add can -c can0 -b 0 -d 20
apm load

# -- prep: 32 KiB 0xAA sentinel (helper script: `sh` mangles quotes/parens)
sh /home/mseo/thesis/csp-linksim/experiments/mk-sentinel 32768 /tmp/sentinel_32k.bin
sh rm -f /tmp/pre32.bin /tmp/got32.bin

# -- 1. sentinel pre-fill over the CLEAN link, then prove the region is 0xAA
upload -n 5431 -v 2 -t 10000 /tmp/sentinel_32k.bin 0x10000000
sleep 2000
download -n 5431 -v 2 -t 10000 0x10000000 32768 /tmp/pre32.bin
sleep 1000
sh cmp /tmp/pre32.bin /tmp/sentinel_32k.bin

# -- 2. fast retransmits, generous connection, modest loss: finish inside 10 s
rdp opt -w 5 -c 30000 -p 200 -k 100
csp_loss start -L 0.10 -S 1
upload -n 5431 -v 2 -t 30000 /home/mseo/thesis/csp-linksim/captures/payload_32k.bin 0x10000000
sleep 3000
csp_loss status
csp_loss stop

# -- 3. teardown, then verify over the clean link
sleep 12000
crc32 -n 5431 -v 2 -f /home/mseo/thesis/csp-linksim/captures/payload_32k.bin 0x10000000
sleep 500
download -n 5431 -v 2 -t 10000 0x10000000 32768 /tmp/got32.bin
sleep 1000

# -- 4. verdict, external to the mechanism under test
sh sha256sum /home/mseo/thesis/csp-linksim/captures/payload_32k.bin /tmp/got32.bin
sh /home/mseo/thesis/csp-linksim/experiments/rdp-forcesilent-verify 32768 /home/mseo/thesis/csp-linksim/captures/payload_32k.bin /tmp/pre32.bin /tmp/got32.bin /tmp/sentinel_32k.bin
exit
