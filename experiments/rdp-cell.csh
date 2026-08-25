# rdp-cell.csh - the RELIABLE-PATH arm under seeded loss. ONE terminal.
#
#     ~/thesis/csh/builddir/csh -i experiments/rdp-cell.csh
#
# No bridge needed here, unlike the DTP arm: csh's own `upload` sends the bytes,
# so csp_loss drops them straight out of this node's transmit path.
#
# PREREQ: vmem_node up at 5431 ->  scripts/bringup-vmem-node
csp init
csp add can -c can0 -b 0 -d 20
apm load -p /home/mseo/thesis/csp-linksim/build/apm

# -- 0. liveness first: a dead receiver makes every line below meaningless
ping -n 5431 -t 5000

# -- 1. pre-fill the region over the CLEAN link, so a later match cannot be
#       stale bytes from an earlier run
sh head -c 262144 /dev/zero > /tmp/rdp_zeros.bin
upload -n 5431 -v 2 -t 10000 /tmp/rdp_zeros.bin 0x10000000
sleep 2000
# must print Failure: zeros are not the payload. Success here = stale region.
crc32 -n 5431 -v 2 -f /home/mseo/thesis/csp-linksim/captures/payload_256k.bin 0x10000000

# ---- THE ONE LINE TO EDIT -------------------------------------------------
# -L loss fraction  -S seed  -B mean burst (Gilbert-Elliott)  -R pacing bit/s
# Do NOT use -M here. RDP retransmits, and -M keys drops to a packet identity so
# the same fragment is dropped on every resend -- a retrying protocol can never
# converge against it. Per-transmission loss (plain -L, or -B) is the honest
# channel for a reliable transport.
# Clean control: csp_loss start -L 0.0 -R 9600
csp_loss start -L 0.30 -B 4 -S 1 -R 9600
# ---------------------------------------------------------------------------

upload -n 5431 -v 2 -t 10000 /home/mseo/thesis/csp-linksim/captures/payload_256k.bin 0x10000000
sleep 2000
csp_loss status
csp_loss stop

# -- 2. verdict over the clean link. Expect on this arm: the upload aborts
#       LOUDLY ("didn't complete", with a resume offset) rather than claiming
#       success, and crc32 disagrees. That honesty is a property of the slow
#       link expiring the connection, not of a check on what arrived.
sleep 15000
crc32 -n 5431 -v 2 -f /home/mseo/thesis/csp-linksim/captures/payload_256k.bin 0x10000000
download -v 2 -n 5431 -t 10000 0x10000000 262144 /tmp/rdp_got.bin
sh md5sum /tmp/rdp_got.bin /home/mseo/thesis/csp-linksim/captures/payload_256k.bin
