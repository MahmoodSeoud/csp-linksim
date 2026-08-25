# bringup-upload-client.csh - turn ON the Upload-Client (5424). dtp-2-cell.csh does this
# inline; this file is for bringing the client up by hand from an existing session.
# 5426 is spawned by the A53 app-sys-manager on node 5421 (param mng_util). It also exits
# after each transfer, so run this before every push. Run from your csh session:
#   run /home/mseo/thesis/csp-linksim/experiments/bringup-upload-client.csh
list download 5421
set -n 5421 mng_util_server 5426
set -n 5421 mng_util_interface 0
set -n 5421 mng_util 0
sleep 4000
set -n 5421 mng_util 5424
sleep 6000
ping 5424
