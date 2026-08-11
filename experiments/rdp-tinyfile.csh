# rdp-tinyfile.csh -- reproduce ON-ORBIT silent corruption: a TINY file over RDP.
#
#     csh -i experiments/rdp-tinyfile.csh        (run from the repo root)
#
# WHY TINY. The 256 KiB and 32 KiB cells aborted loudly: under loss the
# connection dies while the send loop is still running, so vmem_upload returns
# count < length and csh says "didn't complete" -- honest. But the loop only has
# to FINISH for the lie to appear (vmem_client.c:159-185: count += len at build
# time, csp_send queues, csp_close sends RST without draining, return count).
#
# For a 12-byte file the loop is ONE iteration. It finishes instantly, on ANY
# link, however slow. So the silent regime is reachable at flight rate -- and the
# real pass logs show it happening: 2026-07-06 log line 4317,
#   "Uploaded 12 bytes in 6.303 s at 1 Bps"      <- reliable path claims success
#   "Calculated CRC32 0x2B60B55D on 12 bytes"    <- the board's own 12 bytes
#   "Failure: 2B60B55D != 77EE5EE0"              <- != the file. Never arrived.
# Across all 200 pass logs that check succeeded ZERO times.
#
# This cell reproduces that deliberately, with the sentinel control the flight
# log lacks, so "the bytes never arrived" is proven rather than inferred.
#
# Prereqs: vmem_node up at 5431 on can0.
#
# THE FORK on the second upload:
#   "Uploaded 12 bytes"           -> claims success; verdict says if it lied
#   "Upload didn't complete, ..." -> the SYN or the connection died first; re-run
#                                    (only the data packet's loss gives the lie)

csp init
csp add can -c can0 -b 0 -d 20
apm load

# -- prep: a 12-byte payload, and a 4 KiB 0xAA sentinel for the target region
sh /home/mseo/thesis/csp-linksim/experiments/mk-sentinel 4096 /tmp/sentinel_4k.bin
sh head -c 12 /home/mseo/thesis/csp-linksim/captures/payload_256k.bin > /tmp/tiny12.bin
sh rm -f /tmp/pre4k.bin /tmp/got4k.bin

# -- 1. sentinel pre-fill over the CLEAN link, then prove the region is 0xAA
upload -n 5431 -v 2 -t 10000 /tmp/sentinel_4k.bin 0x10000000
sleep 2000
download -n 5431 -v 2 -t 10000 0x10000000 4096 /tmp/pre4k.bin
sleep 1000
sh cmp /tmp/pre4k.bin /tmp/sentinel_4k.bin

# -- 2. the tiny upload under loss. 50% so the single data packet is likely
#       dropped; the loop finishes regardless, which is the whole point.
csp_loss start -L 0.50 -S 1
upload -n 5431 -v 2 -t 10000 /tmp/tiny12.bin 0x10000000
sleep 2000
csp_loss status
csp_loss stop

# -- 3. teardown, then verify over the clean link
sleep 15000
crc32 -n 5431 -v 2 -f /tmp/tiny12.bin 0x10000000
sleep 1000
download -n 5431 -v 2 -t 10000 0x10000000 4096 /tmp/got4k.bin
sleep 1000

# -- 4. verdict: are the first 12 bytes the file, or still the sentinel?
sh /home/mseo/thesis/csp-linksim/experiments/rdp-tinyfile-verify
exit
