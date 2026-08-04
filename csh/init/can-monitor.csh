# can-monitor.csh - watch a real CAN bus from a csh session (oracle B, passive).
#
# Run FROM THE REPO ROOT, after the front-ends are built (docs/HOWTO.md #1):
#   CSH=~/disco/src/csh/builddir/csh
#   "$CSH" -i csh/init/can-monitor.csh
#
# What it does: joins can0 promiscuously (-p), loads the csp_monitor APM from
# build/apm/, and starts capturing every RDP/DTP frame on the bus to
# captures/can0_live.csv. Watch it live from another terminal:
#   tail -f captures/can0_live.csv
#
# Notes:
#   -b 0     skips the privileged bitrate set, so no root is needed; the bus
#            must already be up (the DISCO2 caninit script does this).
#   addr 19  a free address on the DISCO2 flatsat (occupied: 0/15 this host,
#            33/63 pcdu, 34 obc-payload). The monitor is passive either way.
#   -d -1    capture ANY dport. inferred_loss then mixes connections; scope it
#            with -d 8 (DTP data) or -d 13 (RDP/DIPP meta) for a clean number.
#
# Stop + flush the CSV with:  csp_monitor stop
csp init
csp add can -p -c can0 -b 0 19
apm load -p build/apm
csp_monitor start -d -1 -o captures/can0_live.csv
