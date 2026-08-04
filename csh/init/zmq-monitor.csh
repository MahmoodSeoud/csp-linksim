# zmq-monitor.csh - watch a live CSP-over-ZMQ bus (a zmqproxy broker) passively.
#
# Run FROM THE REPO ROOT, after the front-ends are built (docs/HOWTO.md #1):
#   csh -i csh/init/zmq-monitor.csh
#
# Use case: a satdeploy/SVU/DTP transfer is running through a zmqproxy (or
# zmqproxy-lossy) on localhost and you want an independent record of every
# frame that actually crossed the broker -- oracle B for a virtual bus.
#
# ZMQ is a broadcast hub: the broker publishes every frame to every
# subscriber, so joining promiscuously (-p) sees the whole bus without
# owning any address in the flow. addr 30 is arbitrary and unused by the
# satdeploy dev stack (19 = ground, 5425 = agent).
#
#   -d -1    capture ANY dport. Scope it instead with:
#              -d 9   SVU bulk data        -d 11  SVU meta handshake
#              -d 8   DTP data plane       -d 13  RDP/DIPP meta
#   -O 8     satDeploy's DTP data-header overhead (4 = dipp). Only affects
#            fragment indexing for DTP rows; SVU/RDP rows are unaffected.
#
# Stop + flush the CSV with:  csp_monitor stop
csp init
csp add zmq -p 30 localhost
apm load -p build/apm
csp_monitor start -d -1 -o captures/zmq_live.csv
