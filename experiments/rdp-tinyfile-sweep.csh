# rdp-tinyfile-sweep.csh -- 8 tiny-file cells, one per seed, in ONE run.
#
#     csh -i experiments/rdp-tinyfile-sweep.csh      (from the repo root)
#
# Seed 1 delivered its single packet, so the upload's success claim was CORRECT.
# We need the cell where the DATA packet is the one dropped: then the send loop
# still finishes (one iteration, 0.001 s) and vmem_upload returns 12 = "success"
# over a region that never changed. That is the on-orbit 2026-07-06 failure
# (Uploaded 12 bytes / crc32 mismatch) reproduced with a sentinel control.
#
# Each cell: 0xAA pre-fill (clean) -> lossy 12-byte upload -> read back -> judge.
# Watch each cell's "Uploaded 12 bytes" vs "didn't complete", and pair it with
# that cell's verdict line. A cell reading STILL SENTINEL after a success claim
# is the result.
#
# Prereq: vmem_node up at 5431 (scripts/bringup-vmem-node).

csp init
csp add can -c can0 -b 0 -d 20
apm load
ping -n 5431 -t 5000
sh /home/mseo/thesis/csp-linksim/experiments/mk-sentinel 4096 /tmp/sentinel_4k.bin
sh head -c 12 /home/mseo/thesis/csp-linksim/captures/payload_256k.bin > /tmp/tiny12.bin

# ---------------- seed 1 ----------------
sh echo ================ SEED 1 ================
sh rm -f /tmp/pre4k.bin /tmp/got4k.bin
upload -n 5431 -v 2 -t 10000 /tmp/sentinel_4k.bin 0x10000000
sleep 2000
download -n 5431 -v 2 -t 10000 0x10000000 4096 /tmp/pre4k.bin
sleep 1000
csp_loss start -L 0.50 -S 1
upload -n 5431 -v 2 -t 10000 /tmp/tiny12.bin 0x10000000
sleep 2000
csp_loss status
csp_loss stop
sleep 12000
download -n 5431 -v 2 -t 10000 0x10000000 4096 /tmp/got4k.bin
sleep 1000
sh /home/mseo/thesis/csp-linksim/experiments/rdp-tinyfile-verify 1

# ---------------- seed 2 ----------------
sh echo ================ SEED 2 ================
sh rm -f /tmp/pre4k.bin /tmp/got4k.bin
upload -n 5431 -v 2 -t 10000 /tmp/sentinel_4k.bin 0x10000000
sleep 2000
download -n 5431 -v 2 -t 10000 0x10000000 4096 /tmp/pre4k.bin
sleep 1000
csp_loss start -L 0.50 -S 2
upload -n 5431 -v 2 -t 10000 /tmp/tiny12.bin 0x10000000
sleep 2000
csp_loss status
csp_loss stop
sleep 12000
download -n 5431 -v 2 -t 10000 0x10000000 4096 /tmp/got4k.bin
sleep 1000
sh /home/mseo/thesis/csp-linksim/experiments/rdp-tinyfile-verify 2

# ---------------- seed 3 ----------------
sh echo ================ SEED 3 ================
sh rm -f /tmp/pre4k.bin /tmp/got4k.bin
upload -n 5431 -v 2 -t 10000 /tmp/sentinel_4k.bin 0x10000000
sleep 2000
download -n 5431 -v 2 -t 10000 0x10000000 4096 /tmp/pre4k.bin
sleep 1000
csp_loss start -L 0.50 -S 3
upload -n 5431 -v 2 -t 10000 /tmp/tiny12.bin 0x10000000
sleep 2000
csp_loss status
csp_loss stop
sleep 12000
download -n 5431 -v 2 -t 10000 0x10000000 4096 /tmp/got4k.bin
sleep 1000
sh /home/mseo/thesis/csp-linksim/experiments/rdp-tinyfile-verify 3

# ---------------- seed 4 ----------------
sh echo ================ SEED 4 ================
sh rm -f /tmp/pre4k.bin /tmp/got4k.bin
upload -n 5431 -v 2 -t 10000 /tmp/sentinel_4k.bin 0x10000000
sleep 2000
download -n 5431 -v 2 -t 10000 0x10000000 4096 /tmp/pre4k.bin
sleep 1000
csp_loss start -L 0.50 -S 4
upload -n 5431 -v 2 -t 10000 /tmp/tiny12.bin 0x10000000
sleep 2000
csp_loss status
csp_loss stop
sleep 12000
download -n 5431 -v 2 -t 10000 0x10000000 4096 /tmp/got4k.bin
sleep 1000
sh /home/mseo/thesis/csp-linksim/experiments/rdp-tinyfile-verify 4

# ---------------- seed 5 ----------------
sh echo ================ SEED 5 ================
sh rm -f /tmp/pre4k.bin /tmp/got4k.bin
upload -n 5431 -v 2 -t 10000 /tmp/sentinel_4k.bin 0x10000000
sleep 2000
download -n 5431 -v 2 -t 10000 0x10000000 4096 /tmp/pre4k.bin
sleep 1000
csp_loss start -L 0.50 -S 5
upload -n 5431 -v 2 -t 10000 /tmp/tiny12.bin 0x10000000
sleep 2000
csp_loss status
csp_loss stop
sleep 12000
download -n 5431 -v 2 -t 10000 0x10000000 4096 /tmp/got4k.bin
sleep 1000
sh /home/mseo/thesis/csp-linksim/experiments/rdp-tinyfile-verify 5

# ---------------- seed 6 ----------------
sh echo ================ SEED 6 ================
sh rm -f /tmp/pre4k.bin /tmp/got4k.bin
upload -n 5431 -v 2 -t 10000 /tmp/sentinel_4k.bin 0x10000000
sleep 2000
download -n 5431 -v 2 -t 10000 0x10000000 4096 /tmp/pre4k.bin
sleep 1000
csp_loss start -L 0.50 -S 6
upload -n 5431 -v 2 -t 10000 /tmp/tiny12.bin 0x10000000
sleep 2000
csp_loss status
csp_loss stop
sleep 12000
download -n 5431 -v 2 -t 10000 0x10000000 4096 /tmp/got4k.bin
sleep 1000
sh /home/mseo/thesis/csp-linksim/experiments/rdp-tinyfile-verify 6

# ---------------- seed 7 ----------------
sh echo ================ SEED 7 ================
sh rm -f /tmp/pre4k.bin /tmp/got4k.bin
upload -n 5431 -v 2 -t 10000 /tmp/sentinel_4k.bin 0x10000000
sleep 2000
download -n 5431 -v 2 -t 10000 0x10000000 4096 /tmp/pre4k.bin
sleep 1000
csp_loss start -L 0.50 -S 7
upload -n 5431 -v 2 -t 10000 /tmp/tiny12.bin 0x10000000
sleep 2000
csp_loss status
csp_loss stop
sleep 12000
download -n 5431 -v 2 -t 10000 0x10000000 4096 /tmp/got4k.bin
sleep 1000
sh /home/mseo/thesis/csp-linksim/experiments/rdp-tinyfile-verify 7

# ---------------- seed 8 ----------------
sh echo ================ SEED 8 ================
sh rm -f /tmp/pre4k.bin /tmp/got4k.bin
upload -n 5431 -v 2 -t 10000 /tmp/sentinel_4k.bin 0x10000000
sleep 2000
download -n 5431 -v 2 -t 10000 0x10000000 4096 /tmp/pre4k.bin
sleep 1000
csp_loss start -L 0.50 -S 8
upload -n 5431 -v 2 -t 10000 /tmp/tiny12.bin 0x10000000
sleep 2000
csp_loss status
csp_loss stop
sleep 12000
download -n 5431 -v 2 -t 10000 0x10000000 4096 /tmp/got4k.bin
sleep 1000
sh /home/mseo/thesis/csp-linksim/experiments/rdp-tinyfile-verify 8

exit
