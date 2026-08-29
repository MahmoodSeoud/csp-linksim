# Lost primary sources: RDP session logs, 2026-08-26

`scripts/rdp-board-sweep` wrote its per-run csh session log to
`/tmp/rdpcl_<mode>_L<loss>_b<burst>_s<seed>_session.log` and never archived it. When
`PAY` became a parameter the label gained no artifact identity, so the `libcolor.so`
runs overwrote the `libjpegxl.so` logs at the same coordinates.

Confirmed by mtime: `/tmp/rdpcl_stock_L0.30_b0_s1_session.log` is timestamped after the
libcolor runner finished, so it holds the libcolor run. **The primary source behind the
committed `stock,0.30,1` libjpegxl row no longer exists.**

Rows affected, all libjpegxl at coordinates the libcolor runs reused:
`stock,0,1` and `stock,0.30,1`.

What survives for those rows: the capture line itself (claimed, crc, sha, drops,
verdict, elapsed_s) and `runner.log`. What is gone: the full csh transcript, including
the `csp_loss` arming lines that prove the run was paced and the injector armed.

The rows are NOT withdrawn. Their integrity fields were recorded at the time and the
elapsed times are internally consistent with the surviving rows at the same loss level
(594.750 against 580.375 and 598.681). But they cannot be audited from primary source,
and that limitation belongs in the threats section rather than being discovered later.

Fixed 2026-08-26: the label now carries the artifact basename, and every session log is
copied into this directory as it is produced.
