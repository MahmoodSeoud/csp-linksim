# satdeploy-cell.csh - the satdeploy FULL TOOL under seeded loss. ONE terminal.
#
#     ~/thesis/csh/builddir/csh -i experiments/satdeploy-cell.csh
#
# No bridge needed: `satdeploy push` sends from this shell, so csp_loss drops the
# frames out of this node's own transmit path.
#
# PREREQ: satdeploy-agent running on the payload board, e.g.
#   /home/root/satdeploy-agent-smart-bk -c can0 -a 5427     (on the board)
# Check with scripts/deploy-agent, or ping 5427 below.
csp init
csp add can -c can0 -b 0 -d 20
apm load
apm load -p /home/mseo/thesis/csp-linksim/build/apm
# Both paths are needed: `satdeploy push` lives in the installed APM
# (~/.local/lib/csh/libcsh_satdeploy_apm.so) while csp_loss is built here.

ping -n 5427 -t 5000

# ---- THE ONE LINE TO EDIT -------------------------------------------------
# As with RDP, do NOT use -M: satdeploy recovers by re-requesting missing blocks,
# and identity-keyed drops would re-drop the same block every round, so recovery
# could never converge. -B 4 is the burst shape the campaign used.
# Clean control: csp_loss start -L 0.0 -R 9600
csp_loss start -L 0.30 -B 4 -S 1 -R 9600
# ---------------------------------------------------------------------------

# -m 256 pins the fragment grid (1058 fragments for the 256 KiB artifact); the
# compiled-in default drifted to 1024 once and silently changed the grid.
satdeploy push -f /home/mseo/thesis/csp-linksim/captures/payload_256k.bin -r /home/root/satdeploy_cell.bin -n 5427 -m 256
sleep 2000
csp_loss status
csp_loss stop

# -- VERDICT. satdeploy decides completion at the RECEIVER, so its own report is
#    the claim under test: it either reports a verified install or reports the
#    transfer unfinished. Judge it from outside, on the board over the console:
#      md5sum /home/root/satdeploy_cell.bin
#    against  md5sum captures/payload_256k.bin  on the ground.
