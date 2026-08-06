# bringup_upload_client.csh - turn ON the Upload-Client (5426) before exp_upload_file.csh.
# 5426 is spawned by the A53 app-sys-manager on node 5421 (param mng_util). It also exits
# after each transfer, so run this before every push. Run from your csh session:
#   run /home/mseo/thesis/csp-linksim/experiments/bringup_upload_client.csh
list download 5421
set -n 5421 mng_util_server 5424
set -n 5421 mng_util_interface 0
set -n 5421 mng_util 0
sleep 4000
set -n 5421 mng_util 5426
sleep 6000
ping 5426
