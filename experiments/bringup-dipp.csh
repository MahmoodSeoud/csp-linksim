# bringup-dipp.csh - turn ON the DIPP node (5423) before uploading to it.
# 5423 is spawned by the A53 app-sys-manager on node 5421 (param mng_dipp, run_node 5423).
# Toggle THROUGH 0: stop, then spawn. Run from your csh session before rdp-baseline.csh:
#   run /home/mseo/thesis/csp-linksim/experiments/bringup-dipp.csh
list download 5421
set -n 5421 mng_dipp_interface 0
set -n 5421 mng_dipp 0
sleep 4000
set -n 5421 mng_dipp 5423
sleep 6000
ping 5423
