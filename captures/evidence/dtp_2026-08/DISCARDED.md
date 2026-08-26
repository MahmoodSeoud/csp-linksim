# Discarded: pre-fix instrument runs, 2026-08-26

The runs recorded in `dtp_experiment_2026-08_DISCARDED-prefix-instrument.csv` were made
before `dtp-experiment` pinned the injector's `-m`/`-O`.

`csp_loss` defaults to mtu 1024 / overhead 8, which is satDeploy's libdtp header. The
dipp session under test is mtu 256 / overhead 4. With the defaults the injector computed
the fragment index as `offset / 1016` while real offsets step by 252, so four consecutive
fragments collapsed onto one index and shared a single drop decision. The channel was
bursty in runs of four while the injector reported "independent per-transmission loss",
and a nominal 0.10 realised as 0.0743 in all three seeds (20 dropped = 5 index groups x 4
frames), which is what exposed it.

These runs are not invalid under the exclusion rule: the apparatus worked and the hashes
are real. They measured a different loss model, so under the standing rule that an
instrument change makes a new instrument version, they are discarded rather than mixed
with the corrected set.

The three 0.00 controls were unaffected (`-M` is not passed at zero loss) but are re-run
anyway so the whole experiment is recorded by one instrument version.

Fix: `scripts/dtp-experiment` now passes `-M 8 -m 256 -O 4`.
