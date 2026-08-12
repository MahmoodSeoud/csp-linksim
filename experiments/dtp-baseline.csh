# dtp-baseline.csh - Deployed DTP (upload_file PUSH) experiment. ONE command:
# brings up the Upload-Client (5426), monitors, pushes.
#   run /home/mseo/thesis/csp-linksim/experiments/dtp-baseline.csh
# upload_file tells the Upload-Client (5426) to pull file.bin from the gs-server (5424)
# and save it on the PAYLOAD at /home/root/got.bin. The file lands on the board, so it
# CANNOT be verified from the ground here - this arm yields the wire-loss number only.
# For a ground-verifiable DTP result use exp_dtp_pull.csh instead.
# PREREQ (one-time ground infra): gs-server up on can0 at 5424 serving file.bin.
# Output in your csh working directory: uploadfile_monitor.csv (port 8).
run /home/mseo/thesis/csp-linksim/experiments/bringup-upload-client.csh
csp_monitor start -d 8 -m 256 -O 4 -o uploadfile_monitor.csv
sleep 1000
upload_file -f file.bin -d /home/root/got.bin -n 5426 -s 5424
sleep 30000
csp_monitor stop
