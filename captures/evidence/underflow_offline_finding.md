# ch5.2 underflow hypothesis — offline reproduction attempt (2026-08-19)

Question: does the committed traced-resume evidence already confirm the
gs-server transfer-size underflow, without a new bench run?

Method (scripts/dtp_underflow_offline.py, read-only, no bench): port
upload_gs-server compute_transfer_size verbatim (sum of end*252 - start*252,
unsigned, PAY = mtu-4 = 252), run it over (a) the intervals stored in each
committed dtp_session_meta.bin and (b) the round-2 verify-request intervals
in cli_p2.log. Compare against the client-logged value at dtp_protocol.c:37.

Source fact pinned: dtp_protocol.c:36-37 sets session->payload_size from
total_payload_size (= the true file size) but LOGS size_in_bytes
(= compute_transfer_size, the interval sum). So the near-2^32 numbers in the
traces are compute_transfer_size outputs, confirming that function is the
right place to look.

Result: NOT REPRODUCED.
  loss0_20260816: logged size_in_bytes = 3871951764; stored request = 18
    well-formed intervals (all end>start), compute_transfer_size = 175392.
  20260719:       logged size_in_bytes = 3090533264; stored request = 18
    intervals (incl. one 0xFFFFFFFF whole-file marker), = 239716.
  Neither the stored session-file request nor the round-2 verify request
  reproduces the logged value. No inverted interval is present in the
  serialised request either trace committed.

Interpretation: the corrupt session length does NOT arise from the intervals
the client serialised (those are well-formed). It arises in the request_meta
the SERVER actually parsed at the first exchange — a value not visible in any
committed artifact. Both session files also show the internal inconsistency
bytes_received == payload_size == 262144 alongside 18 missing intervals, i.e.
a p1 session that recorded itself complete while holding gaps.

Consequence for ch5.2: the specific sentence "a single inverted or
out-of-range interval underflows" is not supported by the committed data; the
adjacent sentence "a session length taken from an unvalidated, partly
uninitialised handshake" IS the better description and should carry the claim.
Confirming the exact trigger REQUIRES the instrumented run (RUNLOG Amend. 5),
and that run must capture the on-wire request_meta the server receives,
including nof_intervals and any slots past it, not just the client's view.
The offline attempt therefore refutes "already confirmed" and sharpens the
pending experiment's required capture point.
