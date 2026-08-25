# satdeploy-cell.csh - the satdeploy FULL TOOL under seeded loss. ONE terminal.
#
#     ~/thesis/csh/builddir/csh -i experiments/satdeploy-cell.csh
#
# No bridge needed: `satdeploy upload` sends from this shell, so csp_loss drops the
# frames out of this node's own transmit path.
#
# PREREQ: satdeploy-agent running on the payload board, e.g.
#   /home/root/satdeploy-agent-smart-bk -c can0 -a 5427     (on the board)
# Check with scripts/deploy-agent, or ping 5427 below.
csp init
csp add can -c can0 -b 0 -d 20
apm load
apm load -p /home/mseo/thesis/csp-linksim/build/apm
# Both paths are needed: `satdeploy upload` lives in the installed APM
# (~/.local/lib/csh/libcsh_satdeploy_apm.so) while csp_loss is built here.

ping -n 5427 -t 5000

# As with RDP, do NOT use -M: satdeploy recovers by re-requesting missing blocks,
# and identity-keyed drops would re-drop the same block every round, so recovery
# could never converge. -B 4 is the burst shape the campaign used.
# Clean control: csp_loss start -L 0.0 -R 9600
# ---- PARAMETERS ------------------------------------------------------------
# csh has its own variable store (`var set` / `$(NAME)`); it does NOT read the
# shell environment, so `LOSS=0.3 csh -i ...` will NOT work. Two ways to set them:
#   a) edit the `var set` lines below, or
#   b) delete them, start csh, `var set LOSS 0.10` etc., then `run <this file>`
#      -- values already set survive, so the same file runs every cell.
var set LOSS 0.30
var set SEED 1
var set BURST 4
var set RATE 9600
# ----------------------------------------------------------------------------
csp_loss start -L $(LOSS) -B $(BURST) -S $(SEED) -R $(RATE)
# ---------------------------------------------------------------------------

# -m 256 pins the fragment grid (1058 fragments for the 256 KiB artifact); the
# compiled-in default drifted to 1024 once and silently changed the grid.
# SVU bulk data rides port 9; recording it makes the fragment grid visible and is
# what the deleted baseline file used to provide.
csp_monitor start -d 9 -m 256 -O 8 -o /tmp/satdeploy_monitor.csv
satdeploy upload -f /home/mseo/thesis/csp-linksim/captures/payload_256k.bin -r /home/root/satdeploy_cell.bin -n 5427 -m 256
sleep 2000
csp_loss status
csp_loss stop
csp_monitor stop

# -- VERDICT. satdeploy decides completion at the RECEIVER, so its own report is
#    the claim under test: it either reports a verified install or reports the
#    transfer unfinished. Judge it from outside, on the board over the console:
#      md5sum /home/root/satdeploy_cell.bin
#    against  md5sum captures/payload_256k.bin  on the ground.
