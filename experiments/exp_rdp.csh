# exp_rdp.csh - CSH-RDP (CSP RDP + CRC32) integrity, clean single-shot. Runnable + reproducible.
#
# Uploads the pinned 256 KiB payload to the RAM node 'rdprx' (node 5431, region bigmem, fixed
# address 0x10000000) over CSP RDP, checks integrity with crc32, and downloads it back.
# Clean link => crc32 prints "Success". RDP+CRC32 never silently corrupts: it delivers
# intact (Success) or fails loudly.
#
#   run /home/mseo/thesis/csp-linksim/experiments/exp_rdp.csh
#
# PREREQ: the rdprx node up ->  scripts/bringup-vmem-node   (serves bigmem at fixed 0x10000000)
# For the controlled LOSS SWEEP (injector, stock vs tuned RDP, crc + sha checks), from a shell:
#   scripts/rdp-csploss-sweep   (board, csp_loss)  or  experiments/rdp-silent-try  (host)
csp init
csp add can -d 20
node add -n 5431 rdprx
upload   -v 2 -n rdprx -t 10000 /home/mseo/thesis/csp-linksim/captures/payload_256k.bin 0x10000000
crc32    -n rdprx -v 2 -f /home/mseo/thesis/csp-linksim/captures/payload_256k.bin 0x10000000
download -v 2 -n rdprx -t 10000 0x10000000 262144 rdp_got.bin
