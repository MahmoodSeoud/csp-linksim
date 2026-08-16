csp init
csp add can -c can0 -b 0 -d 16
apm load -p /tmp/rawdtp_dtptrace_L0.10_s1_x2sR/apm
dtp_client -n 5424 -i 0 -m 256 -t 1024 -r
sleep 3000
dtp_info
sleep 1000
exit
