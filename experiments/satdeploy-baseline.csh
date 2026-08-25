# satdeploy-baseline.csh - the satdeploy FULL TOOL, clean link. Does the arm work at all?
#
#     ~/thesis/csh/builddir/csh -i experiments/satdeploy-baseline.csh
#
# `satdeploy push` copies a file to the board and the on-board agent verifies it
# against a whole-artifact digest before reporting the install complete. On a
# clean link this must report success AND the board's md5 must match. Run this
# before trusting any satdeploy-cell.csh result.
#
# PREREQ: satdeploy-agent running on the payload board at 5427, e.g.
#   /home/root/satdeploy-agent-smart-bk -c can0 -a 5427     (on the board)
csp init
csp add can -c can0 -b 0 -d 20
apm load
apm load -p /home/mseo/thesis/csp-linksim/build/apm
# Both paths are needed: `satdeploy push` lives in the installed APM
# (~/.local/lib/csh/libcsh_satdeploy_apm.so) while csp_loss is built here.

ping -n 5427 -t 5000

# SVU bulk data rides port 9; recording it makes the fragment grid visible.
csp_monitor start -d 9 -m 256 -O 8 -o /tmp/satdeploy_baseline_monitor.csv
satdeploy push -f /home/mseo/thesis/csp-linksim/captures/payload_256k.bin -r /home/root/satdeploy_baseline.bin -n 5427 -m 256
sleep 2000
csp_monitor stop

# VERDICT, on the board over the console:
#   md5sum /home/root/satdeploy_baseline.bin
# against  md5sum captures/payload_256k.bin  on the ground. Must match.
