# dtp-1-injector.csh - TERMINAL 1 of the deployed-DTP arm: the loss injector.
#
#     ~/thesis/csh/builddir/csh -i experiments/dtp-1-injector.csh
#
# Leave this shell running; it is the bridge. Then drive the transfer from
# TERMINAL 2 with dtp-2-cell.csh.
#
# WHY THIS ARM NEEDS A BRIDGE AT ALL. csp_loss drops packets a node TRANSMITS.
# The deployed upload is a PULL: csh only sends the trigger, while the 1041 data
# frames flow from upload_gs-server (ZMQ side) to the on-board client (CAN side)
# and never cross the operator's shell. So this shell is put ON that path as a
# transparent bridge, and csp_loss drops the frames it forwards.
#
# PREREQ, from a normal shell:
#   zmqproxy &                                        (defaults to 6000/7000)
#   cd ~/thesis/disco/src/upload_gs-server/builddir
#   ./upload_gs-server -z 127.0.0.1 -a 5424 &
# The server MUST be on -z (ZMQ). Started with -c can0 it answers the board
# directly on the bus, the data never crosses this injector, and the run reads
# as lossless. csp_loss warns about exactly that when it arms.
csp init
csp add can -p -c can0 5391
apm load -p /home/mseo/thesis/csp-linksim/build/apm

# ---- THE ONE LINE TO EDIT -------------------------------------------------
# -L loss fraction   -S seed (replayable)   -R pacing bit/s (flight = 4800)
# -M 8 keys drops to the DTP data port, so the RDP metadata handshake on port 7
#      survives and a failure is attributable to loss on the file body.
#      Identity-keyed drops are correct here because the deployed path is
#      fire-and-forget: it never retransmits, so there is nothing to converge.
# Clean control: csp_loss start -i CAN0 -L 0.0 -R 9600
csp_loss start -i CAN0 -L 0.30 -S 1 -R 9600 -M 8
# ---------------------------------------------------------------------------

csp add zmq -m 14 5426 127.0.0.1
bridge ZMQ0 CAN0 5424
# Split-horizon on 5424 is load-bearing: the ZMQ broker reflects every frame the
# bridge publishes, and a blindly forwarding bridge laps zmq->can->zmq until the
# RDP handshake gives up (client re-sending its SYN every 5 s, no data phase).
