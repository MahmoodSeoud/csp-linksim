# updown_flatsat.csh - the flight-pass rehearsal against the PAYLOAD BOARD.
#
# Same instrument as updown_selftest.csh, but the far end is the flight-
# representative payload: APPSYS (5421) supervises, and this drill brings up
# DIPP (5423) itself via the manager param, then measures against DIPP -- the
# node real transfers terminate on. Includes the step a real pass needs that
# can only be rehearsed here: discovering the far end's iface NAME for ifstat.
# Validated 2026-08-04 against the live board: both payload nodes answer on
# iface "CAN" -- NOT "CAN0" (which times out and reads exactly like node-down).
#
#   csh -i /home/mseo/thesis/csp-linksim/experiments/updown_flatsat.csh
#
# PREREQ: payload board powered (APPSYS answers ping at 5421).
run /home/mseo/thesis/disco/config/init/can.csh
apm load -p /home/mseo/thesis/csp-linksim/build/apm
ping 5421

# --- 1) bring up DIPP via the APPSYS manager (idempotent if already running) ---
node 5421
set mng_dipp 5423
sleep 1000
ping 5423

# --- 2) iface-name discovery (the real-pass rehearsal step; on this board the
#        answer is CAN -- CAN0 times out) ---
ifstat -n 5423 -t 3000 CAN

# --- 3) quick direction split against DIPP ---
csp_link 5423 -c 10 -I CAN

# --- 4) pass workflow: capture + counter polling, both hops on the -r list:
#        DIPP first (verdict pair), PCDU second (bus cross-check; the monitor
#        subtracts its own cross-target poll traffic from the verdict) ---
csp_monitor start -d -1 -r 5423:CAN,33:CAN -P 5 -o /tmp/flatsat_pass.csv
ping 5423
ping 5423
ping 5423
ping 5423
# load-bearing: the verdict differences counter polls taken every -P 5 s
sleep 11000
csp_monitor status
csp_monitor stop
# CSV: /tmp/flatsat_pass.csv -- same schema/analysis as the real pass capture

# --- 5) CALIBRATED REPLAY: the payload path under a measured-shape channel ---
# -L/-B below are the HISTORICAL priors from scripts/mine-pass-logs (~78%
# marginal loss, mean burst ~7.8, round-trip); replace them with the
# per-direction numbers from the first real pass measured with csp_link -I /
# csp_monitor -r. -S 1 makes the channel replayable: same seed = same good/bad
# runs, so every tool compared later suffers the identical channel.
# The experiment: introduce the loss FIRST, then do a real management op
# (set --force of DIPP's vmem path) THROUGH the lossy channel, with the
# monitor recording every attempt. Read-back after the channel is clean again
# shows whether the op actually landed vs merely seemed to.
csp_monitor start -d -1 -r 5423:CAN -P 5 -o /tmp/flatsat_replay.csv
csp_loss start -L 0.78 -B 7.8 -S 1
set --force mng_dipp_vmem_path "/home/root/dippvmem & mkdir -p /home/root/mseo_test ; printf 'csp init\ncsp add can -c can0 -b 0 -d 5425\n' >/home/root/mseo_test/mseo-ping.csh #"
# fire it: the -v path is only re-read when the manager relaunches DIPP, so cycle
# mng_dipp (0 kills, 5423 restarts) to make the set actually take effect. Both
# sets ride the lossy uplink -- that IS the management op measured under the
# channel. Safe only because the value's pre-`&` token (/home/root/dippvmem)
# exists, so the relaunched DIPP starts clean instead of stranding down.
set mng_dipp 0
set mng_dipp 5423
sleep 11000
csp_loss status
csp_loss stop
# clean link again: did the set survive the channel? (expect the monitor CSV
# to show the retries; expect this get to answer quickly)
get mng_dipp_vmem_path
# restore the board's real value (observed default) so a landed test-set can't
# leak into the next DIPP restart -- runs on the clean link, so it always lands.
# The 2026-08-05 brick was a value whose path token did NOT exist: DIPP fails to
# START under a nonexistent vmem path and stays down until the path is restored
# AND mng_dipp is cycled again -- hence the recovery cycle just below. The param
# is persistent (vmem-backed config), so a forgotten test value survives reboots.
set --force mng_dipp_vmem_path /home/root/dippvmem

csp_monitor status
csp_monitor stop
# CSVs: /tmp/flatsat_pass.csv (clean drill) + /tmp/flatsat_replay.csv (the
# replay: per-packet retries with dir column, verdict should read ~ -L uplink)
