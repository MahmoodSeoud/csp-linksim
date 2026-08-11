# rdp-forcesilent.csh -- can RDP be made to corrupt SILENTLY on the FlatSat bus?
#
# The csp_loss board sweep showed RDP aborts loudly on can0 under loss (honest).
# This cell tries to force the silent variant on the same bus by making the
# connection unkillable: long conn/packet timeouts, so the send loop can finish
# while the connection is still alive -- the condition under which vmem_upload
# returns full success over a truncated region (vmem_client.c:159-185 + the
# RST-at-close in csp_rdp.c:892).
#
# Run:      csh -i experiments/rdp-forcesilent.csh     (from ~/thesis/csp-linksim)
# Prereqs:  vmem_node up at 5431 on can0; /tmp/sentinel_AA.bin exists (the
#           wrapper 'experiments/rdp-forcesilent' makes it and runs the verdict).
# Verdict:  experiments/rdp-forcesilent-verify   (external, after this exits)

csp init
csp add can -c can0 -b 0 -d 20
apm load

# 1. sentinel pre-fill over the CLEAN link, and read it back for the verdict's
#    "no prior garbage" proof (region must be exactly 0xAA before the run)
upload -n 5431 -v 2 -t 10000 /tmp/sentinel_AA.bin 0x10000000
sleep 3000
download -n 5431 -v 2 -t 10000 0x10000000 262144 /tmp/pre.bin
sleep 1000

# 2. make the connection unkillable, then upload the payload under seeded loss
#    THE FORK: "Uploaded 262144 bytes" = silent candidate; "didn't complete" = still loud
rdp opt -w 5 -c 600000 -p 20000 -k 1000
csp_loss start -L 0.30 -S 1
upload -n 5431 -v 2 -t 600000 /home/mseo/thesis/csp-linksim/captures/payload_256k.bin 0x10000000
sleep 6000
csp_loss status
csp_loss stop

# 3. let the RDP connection fully tear down, then verify over the clean link
sleep 15000
crc32 -n 5431 -v 2 -f /home/mseo/thesis/csp-linksim/captures/payload_256k.bin 0x10000000
sleep 500
download -n 5431 -v 2 -t 10000 0x10000000 262144 /tmp/got.bin
sleep 1000
exit
