# rdp-silent-1-upload.csh -- PHASE 1: upload under loss, then EXIT IMMEDIATELY.
#
#     csh -i experiments/rdp-silent-1-upload.csh     (then phase 2, below)
#
# WHY THE IMMEDIATE EXIT IS THE WHOLE POINT.
# vmem_upload counts bytes as it queues them and returns that count; csp_close
# sends RST without draining (vmem_client.c:159-185, csp_rdp.c:892). But
# csp_rdp_check_timeouts keeps RETRANSMITTING unacknowledged packets during
# CLOSE_WAIT for as long as the process lives. So an interactive csh that sits
# at a prompt after the upload quietly delivers the tail in the background --
# the report was still unfounded when it was made, but the file ends up correct
# and nobody notices.
#
# A SCRIPTED upload does not sit around. `csh -i <file>` runs the commands and
# exits, which is how mission operators drive uploads. When the process exits,
# the RDP task dies with it and whatever was still unacknowledged is simply
# gone -- while the operator already saw "Uploaded 262144 bytes".
#
# So: NO sleep between the upload and exit. That is the experiment.
#
# WATCH: the "Uploaded 262144 bytes" line. Phase 2 says what actually landed.

csp init
apm load
sh /home/mseo/thesis/csp-linksim/experiments/host-rdp-infra start
csp add zmq -d 26 127.0.0.1

# -- sentinel pre-fill over the CLEAN bridge, and prove the region is 0xAA
sh /home/mseo/thesis/csp-linksim/experiments/mk-sentinel 262144 /tmp/sentinel_256k.bin
sh rm -f /tmp/hostpre.bin /tmp/hostgot.bin
upload -n 5431 -v 2 -t 120000 /tmp/sentinel_256k.bin 0x10000000
sleep 2000
download -n 5431 -v 2 -t 120000 0x10000000 262144 /tmp/hostpre.bin
sleep 1000
sh /home/mseo/thesis/csp-linksim/experiments/prefill-check /tmp/hostpre.bin /tmp/sentinel_256k.bin

# -- THE MEASURED UPLOAD under seeded loss, then die at once.
#    The connection must outlive the send LOOP (else the loop exits early, the
#    count comes up short, and csh honestly says "didn't complete at 43584" --
#    the same honest path can0 takes with the stock 10 s conn_timeout). Fast
#    retransmits (-p 1000) just keep the run to ~2 min; they do not rescue the
#    final window, because this process exits before that retransmit is due.
rdp opt -w 3 -c 120000 -p 1000 -k 250
sh /home/mseo/thesis/csp-linksim/experiments/host-rdp-infra bridge 0.20 4 /tmp/hostdrops.csv
upload -n 5431 -v 2 -t 120000 /home/mseo/thesis/csp-linksim/captures/payload_256k.bin 0x10000000
exit
