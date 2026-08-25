# dtp-upload.csh - TERMINAL 2 for the deployed uploader: restart the client, upload.
#
#     ~/thesis/csh/builddir/csh -i experiments/dtp-upload.csh
#
# Run dtp-injector.csh in TERMINAL 1 first. Two shells are unavoidable: while a
# bridge is active, csp_bridge_work forwards every frame instead of delivering it
# locally, so the bridging shell cannot also complete the trigger's connection.
#
# The mission init below is what makes `set -n 5421 mng_util` resolve by name: it
# loads the param definitions (list_load.csh) as well as the APMs.
run /home/mseo/thesis/disco/config/init/can.csh

# -- the on-board client exits after every transfer; toggle THROUGH 0 to respawn
set -n 5421 mng_util_server 5426
set -n 5421 mng_util_interface 0
set -n 5421 mng_util 0
sleep 4000
set -n 5421 mng_util 5424
sleep 6000
ping 5424

# -- the real operator command (the FRR verification item), unchanged.
#    -f is trigger metadata only: the server hardcodes file.bin (vmem_dtp_server.c), so
#    what ships is whatever file.bin sits in its builddir. -f is pointed at that same
#    file here so the command on screen matches what actually crosses the link.
upload_file -f /home/mseo/thesis/disco/src/upload_gs-server/builddir/file.bin -d /home/root/dtp_upload.bin -n 5424 -s 5426

# -- VERDICT, on the board over the serial console, AFTER the paced transfer:
#      md5sum /home/root/dtp_upload.bin
#    against the ground's  md5sum of that same file.bin
#    A paced 256 KiB transfer takes ~230 s at 9600 bit/s and the client
#    pre-allocates the destination at full size, so hashing early reports a
#    full-size wrong digest that looks exactly like the finding but is not.
#    In TERMINAL 1, `csp_loss status` must show offered = the server's packet
#    count; anything less means frames vanished before the injector and the experiment
#    is void whatever the md5 says.
