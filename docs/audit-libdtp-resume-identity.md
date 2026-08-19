# Neutral audit: libdtp resume vs. changed-source detection

Date: 2026-08-18. Method: four independent auditors, adversarial brief (find
defeaters first). Scope: DISCO-2 libdtp fork, upstream layout @21fece86,
dipp-apm session hooks, csh upload tooling, ops scripts, satDeploy.

CLAIM UNDER TEST
S1: "The libdtp session/resume file records byte intervals but no hash of the
    source artifact, so a resumed uplink cannot detect a source that changed
    between attempts."
S2: "A resume against a changed source silently produces a mixed-content
    artifact that is reported as successfully delivered."

NOTE: S1 appears in the draft at chapters/chapter2_background.tex:185.
S2 appears NOWHERE in the manuscript; it is a proposed strengthening.

## PART A — DEFEATERS

A1 identity binding — NOT FOUND (claim survives).
  On-wire request dtp_meta_req_t (dtp_protocol.h:26-34): throughput,
  nof_intervals, payload_id, mtu, intervals[19]. On-wire response
  dtp_meta_resp_t (dtp_protocol.h:47-51): size_in_bytes, total_payload_size.
  No artifact descriptor either direction. payload_id is a server-specific
  selector, not a content binding.
  Two candidate bindings inspected and both dead:
   - protobuf UploadMetadataItem.checksum: operator-typed CLI value
     (dipp-apm/src/uploader_apm.c:52,107), transmitted, and never read by the
     receiver (upload_sat-client/src/main.c:406-421 consumes 4 other fields).
   - gs-server MD5 (upload_gs-server/src/vmem/vmem_dtp_server.c:77,204-269):
     real digest of the source, but published to a PM_READONLY telemetry param
     (upload_logs.c:17-20) with no reader in the codebase; never entered into
     dtp_meta_resp_t, never written to the session file. Only get_hash==0
     (computation failure) is tested.
  Sweep for stat/fstat/st_mtime/st_size/st_ino/uuid/build_id/mtime across both
  dtp libs and all three uploaders: zero hits.

A2 size guard — ABSENT (claim does NOT narrow to same-length rebuilds).
  dtp_protocol.c:38  session->payload_size = meta_resp->total_payload_size;
  Unconditional overwrite of the deserialised value, before any comparison.
  Order: dtp_prepare_session -> dtp_deserialize_session (loads recorded size)
  -> read_remote_meta_resp (destroys it) -> start_receiving_data. The only
  comparisons (dtp_client.c:35, :269) are bytes_received vs payload_size, both
  from the same session.
  Build evidence anyway: dipp-apm's own build tree holds two builds of
  libcsh_dtp_client.so at identical length 564384 with different content
  (BuildID fcc7f077 vs f0a447cf). Same-length rebuilds are real, not
  hypothetical.

A3 sender snapshot — NOT a defeater (mechanism is live at the sender).
  upload_gs-server observation_read (vmem_dtp_server.c:27-50) opens, seeks,
  reads and closes file.bin ONCE PER PACKET. get_payload_meta (:59-100)
  re-opens per session/meta request. Nothing is cached across passes.
  (Current satdeploy SVU does snapshot -- svu_serve_session.c:40 -- but only
  within one push, so a between-pass rebuild is still picked up.)

A4 session lifecycle — DEFEATS the claim for the deployed uplink.
  upload_sat-client/src/main.c:418 (working tree; :456 at flight rev 21c1b65)
  opts->resume = 0;  -- the SOLE assignment, on all seven branches audited.
  dtp_client.c:26  if (resume) { res = dtp_deserialize_session(...) }  is the
  only gate on reading the session file. protos/uploadmetadata.proto carries
  no resume/offset field, so the trigger cannot request it. The session file
  IS written after every transfer (session_hooks.c:148-184) and never read:
  write-only data in the deployed uplink.
  Direction: libdtp resume lives entirely on the CLIENT=RECEIVER side; the
  DTP server has no session state (upload_gs-server/src/main.c:53). dipp-apm
  exposes -r/--resume ONLY on the downlink pull command
  (dtp_client_apm.c:133); the uplink command upload_file has no resume flag.
  Flight-image provenance: meta-disco-upload recipe pins upload_sat-client
  rev 21c1b65 and libdtp rev 504e2cd.

A5 tooling guards — NONE code-enforced on the libdtp path (supports the claim).
  No tool computes/stores/transmits/compares a digest across attempts. No tool
  compares size across attempts. No content-addressed names, versioned paths,
  staging dirs or no-overwrite rule: the ground server hardcodes a single
  fixed source name, vmem_dtp_server.c:34,64 snprintf(filename,..., "file.bin")
  with comment "placeholder name for now".
  Documentation points the OPPOSITE way: UPLOAD_FLOW.md:11-12 instructs the
  operator to overwrite file.bin in place; upload_runbook.md:96 does exactly
  that between two uploads; push_csh.sh:24,28 overwrites the single source
  artifact twice in one run. The only mandated verification anywhere is a
  MANUAL crc32 (upload_runbook.md:84) on the VMEM path, not libdtp.
  The SHA256 present in the flight binary is NOT artifact integrity: it hashes
  <=255 bytes of line 1 of a .task file and strcmps against a hardcoded
  EXEC_PASSWORD constant (origin/flight session_hooks.c:36,43-55,87) -- an
  execution auth gate.

A6 overwrite semantics — interval-only, offset-addressed; deployed uplink never
  resumes.
  Writes: session_hooks.c:90 (VMEM mmap, offset = seq*(mtu-4)); vmem_mmap.c:14
  opens O_CREAT|O_RDWR with NO O_TRUNC and :28-31 declines to shrink.
  DISCREPANCY, recorded honestly: on origin/flight, apm_on_start:213 opens the
  destination with fopen(path,"wb+") which TRUNCATES; the working tree uses the
  non-truncating mmap path. If the flight variant is what runs, an enabled
  resume would produce a HOLEY file, not a blend. Unresolved without hardware.

## PART B — SUPPORT

B1 serialised state — CONFIRMED: no artifact-identity field.
  apm_on_serialize (upload_sat-client/src/session/session_hooks.c:148-184;
  dipp-apm/src/session_hooks.c:113-143 identical) writes exactly:
  DTP_SESSION_VERSION, remote_cfg.node, request_meta.mtu, timeout,
  request_meta.throughput, request_meta.payload_id, bytes_received,
  payload_size, nof_segments, then {u32 start,u32 end} pairs.
  Source struct dtp_t (dtp_session.h:25-39) has no hash/uuid/mtime member to
  serialise. Deserialisation's only test is the wire-format version, which
  merely warns on mismatch.
  Sample artifact csp-linksim/dtp_session_meta.bin (472 B) mapped exactly:
  version=1, node=5424, mtu=256, timeout=16, throughput=1024, payload_id=0,
  bytes_received=196560, payload_size=262144, 56 intervals; 24+56*8=472. No
  byte-string field anywhere.

B2 byte flow — split by path.
  Deployed uplink: CANNOT OCCUR (resume never enabled; A4).
  Ground-side downlink pull with dtp_client -r, source changed but same length:
  pass-1 offsets retain version-A bytes, pass-2 re-requested intervals receive
  version-B bytes (sender re-reads per packet, A3), no range rewritten twice,
  no truncation, no size signal. Status: dtp_client.c:104 sets DTP_OK and the
  only assignment away from it is :151 on csp_bind failure; the idle-timeout
  exit (:143-145) logs a warning and changes nothing -> DTP_OK returned.
  No layer notices: sender-side MD5 describes the SOURCE not the delivery;
  upload_log_status=100 is set inside observation_read on the SENDER on every
  successful fread (vmem_dtp_server.c:52) and means only "source was readable";
  the deployed satellite receiver performs no verification and issues no
  completion report at all (grep csp_send in its main.c = 0).

  EMPIRICAL STATUS OF S2: unsupported by any recorded run. Both committed
  traces byte-diffed against the pinned payload give 5028 differing bytes with
  EVERY differing byte zero in the received file (never-written holes), and the
  server's file.bin was constant (2c1fa79a) across all runs. No capture in the
  tree varies the source between passes.

## PART C — VERDICT: NARROWS (S1), FAILS (S2)

S1 narrows to a latent library property, not deployed uplink behaviour.
S2 is struck: unreachable on the deployed uplink, mis-attributed on the path
where resume is reachable (that is the downlink pull), and unsupported by any
measurement.

SUPPORTED REPLACEMENT TEXT (for chapter2_background.tex:185):
  libdtp's serialised session state records the wire-format version, node,
  MTU, timeout, throughput, payload id, byte counters and the missing-interval
  map, and no digest or other identity binding for the artifact; the resume
  path performs no such check, and it overwrites the recorded payload size
  with the server's current one rather than comparing them
  (dtp_protocol.c:38). A resume is therefore blind to a source that changed
  between attempts. This is a latent property of the library rather than a
  behaviour of the deployed uplink: the flight upload client hardcodes
  resume off (upload_sat-client/src/main.c:418) and its trigger message
  carries no resume field, so the session file it writes after every transfer
  is never read back. The resume flag is exposed only on the ground-side
  downlink pull.

CITABLE CONTRAST (satdeploy, verified in source):
  session_state.c:233-239 discards resume state on a full 64-hex SHA-256
  mismatch (expected_hash, session_state.h:16); deploy_handler.c:773-793
  recomputes the digest post-transfer and fails the deploy with
  ERR_CHECKSUM_MISMATCH, unlinking the temp file; svu_transfer.c:70-77 states
  the manifest/bitmap are deliberately not persisted so a ground-side rebuild
  between passes self-heals instead of resuming into a stale artifact.

ADJACENT LIVE MECHANISM (NOT libdtp; flagged, not claimed):
  csh upload -o N (disco/src/csh/src/vmem_client_slash_ftp.c:244-272), the
  resume procedure the runbook actually recommends (upload_runbook.md:122),
  re-opens the CURRENT file per invocation. A rebuild between passes yields
  [0,N) old and [N,size) new at the same VMEM address, returning
  SLASH_SUCCESS with no whole-region verify (crc32 is a separate manual step).
  This is libparam/vmem-client, not libdtp, and is likewise unmeasured.

UNDETERMINED
  1. Whether the deployed ground-station binaries match the audited source
     (the satellite binary was matched to origin/flight; no committed
     ground-side binary to hash-compare).
  2. Whether the flight variant's truncating fopen or the working tree's
     non-truncating mmap path is what runs on the unit (A6 discrepancy).
  3. CWD of the running upload_client (derived from popen inheritance plus a
     unit file with no WorkingDirectory=, not from repo evidence).
  4. upload_client_rec: referenced at app-sys-manager/main.c:450, no source or
     recipe anywhere in the tree.
  5. Whether any operator ever ran dtp_client -r against a rebuilt file.bin --
     no log, capture or CSV records a source change between resume passes.
