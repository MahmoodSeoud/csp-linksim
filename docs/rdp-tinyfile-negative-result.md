# The one-packet reliable-path check (negative result, not used in the thesis)

Retired 2026-08-25. Recorded here so the reasoning is not lost with the files
(`experiments/rdp-tinyfile.csh`, `scripts/mk-sentinel`, `scripts/rdp-tinyfile-verdict`,
recoverable from git history before this commit).

## What it tested

Whether csh's `upload` (the reliable path) reports success over bytes that never
arrived. The mechanism is visible in the source: `vmem_upload` increments its byte
counter as it hands each packet to `csp_send`, which only queues; `csp_close` then
sends an RST without draining and the function returns that counter. The count
therefore reports what was queued, not what was delivered.

It only surfaces for a one-packet file. With 12 bytes the send loop is a single
iteration and always finishes, so the counter reaches full length regardless of
delivery. At 256 KiB the connection expires mid-loop instead and the client aborts
honestly, which is what the thesis reports.

## Why it was retired

The bench reproduction failed: eight seeds at 50% loss over can0 gave 0 silent
outcomes, 7 delivered, 1 connect timeout. can0 stays honest even in the regime most
likely to produce the lie.

The motivating observation came from an operator pass log of 2026-07-06
(`Uploaded 12 bytes in 6.303 s at 1 Bps`, followed by a CRC32 mismatch against the
source, with the check reportedly never succeeding across the pass logs). That log is
not in `captures/`: operator transcripts are ground-station records this project does
not redistribute (Appendix B). So the observation cannot be cited, and the thesis makes
no on-orbit claim about the reliable path.

A negative result whose motivating evidence cannot be published, supporting no claim in
the thesis, is not worth carrying in `experiments/`. If the flight logs ever become
citable, this is the experiment to resurrect.
