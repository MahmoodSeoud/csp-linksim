# Terminology inventory — one claim, one era, one scheme

This is the spec that `scripts/check-manuscript` enforces against
`~/thesis/MasterThesis---SatDeploy`. It encodes the decisions from the approved
design doc *Thesis Coherence — One Claim, One Era, One Table*
(`~/.gstack/projects/MahmoodSeoud-csp-linksim/mseo-master-design-20260808-060811.md`).

The rules live in the linter's `RULES` block; this file is the human-readable
source of truth for *why* each rule exists. Edit both together.

## The scheme

The manuscript makes **one** claim: the *receiver-verification principle* —
completion decided without verified arrival causes silent corruption; a
receiver-declared, digest-gated completion removes it by construction. The two
artifacts are not rivals; they are two forms of the same principle:

| Term | Refers to | Use for |
|---|---|---|
| **SVU core** | the protocol / principle embodiment (per-block manifest over the reliable control channel, body connectionless, receiver alone declares completion) | the mechanism, the design, the property being argued for |
| **satdeploy v2** | the deployed operational agent that *carries* the SVU core onto flight-representative hardware | the tool on the board, the ablation arms, the deployment story |

`satdeploy v2` embodies the SVU core; it does not compete with it. The July
`satdeploy` (transfer layer since replaced by the SVU core) is **satdeploy v1**,
and it is appendix-only (single-era rule below).

## The rules

### 1. One-scheme terminology (ERROR in main text)

The main text must not frame the two artifacts as competitors. Banned phrases
(rival framing) — these are the exact constructions the current draft still
carries and that the gated chapter edits will remove:

- "head-to-head between satdeploy and SVU" / "satdeploy against SVU" — the
  matched comparison is **adoption evidence in the appendix**, not a main-text
  rivalry.
- "SVU, the primary contribution" (and "primary contribution" attached to SVU
  as a *rival* to satdeploy) — the primary contribution is the *principle*.
- "rivalry", "satdeploy versus SVU", "the two tools" (as competitors).

Required when naming the deployed tool in a v2 context: **satdeploy v2**, not
bare "satdeploy" (bare "satdeploy" is ambiguous between v1 and v2). Bare
"satdeploy" is WARN, not ERROR, because many correct sentences read naturally
without the version tag once context is set; the author resolves each.

### 2. Single era — main-text era-citation violations (ERROR)

Main-text evidence is the **August flight-hardware campaign only**. All pre-v2
(July host-arm) data is appendix-only, era-labeled. The one exception is the
single **adoption paragraph** that explains why v2 adopted the SVU core; mark it
with the sentinel comment `% check-manuscript: adoption-exempt` on the line(s)
that legitimately cite the July comparison.

July-era (pre-v2) capture files — ERROR if cited in `chapter[1-7]*.tex` or
`main.tex` outside an adoption-exempt region:

- `h2h_satdeploy.csv`, `h2h_svu.csv` — the July matched head-to-head
- `svu_sweep.csv` — the July SVU host sweep
- any `*_pre20260807.csv` — archived v1 board sweeps

August-era files (allowed in main text): `rq3_corruption.csv`,
`dipp_sweep.csv`, `satdeploy_sweep.csv`, `dtp_zmq_sweep.csv`, `rdp_*.csv`,
`flight_dtp_20260624.csv`, the board `naive_*`/`smart_*` per-cell logs.

### 3. Dangling references (ERROR)

Every `\ref`/`\autoref`/`\eqref` target must have a matching `\label` somewhere
in the sources. A ref with no label is a build-time "??" and an examiner-visible
defect.

### 4. Stale names (ERROR)

Names that changed and must not reappear:

- `satctl` → **csh** (the shell was renamed; `satctl` is the old name).

This list is deliberately short and high-precision. Add a name here only when it
is genuinely retired, so the check stays false-positive-free.

## Tiers

- **ERROR** — fails the build (exit non-zero). Categories 1 (banned phrases), 2,
  3, 4.
- **WARN** — advisory, does not fail unless `--strict`. Bare "satdeploy" without
  a version tag.

Run it with `make-results` before every submission build:

```
scripts/make-results && scripts/check-manuscript
```
