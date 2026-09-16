# NAS Recording Decoupling Implementation Plan

> **For agentic workers:** Use test-driven implementation and independent code review. The user has approved implementation and a private GitHub branch; do not deploy to field devices.

**Goal:** An unhealthy or missing NAS health snapshot must not abort an already-running recording, change its session, or prevent scheduled segment rollover. There is no NAS-outage grace timeout.

**Architecture:** Separate checks of the actual recording destination from NAS admission checks used when starting a new recording. Preserve truthful NAS health reporting and uploader gating/retries. Actual filesystem failures and local destination capacity errors retain their existing behavior and must not be reported as successful writes.

**Tech Stack:** Linux C++17 receiver, Python uploader and integration tests, CMake/CTest.

## Global Constraints

- Work in `D:/wireless_video_delivery-nas-decoupling`, branch `codex/nas-recording-decoupling`.
- Runtime baseline: `9c8bc21c5cfa2bc77b7aed59a119f276afff11da`, on upstream `fix/native-rgb-capture-20260911`. Upstream main does not contain the field NAS capacity guard.
- Private destination: `nullbig69-lab/wireless_video_delivery`. Do not push changes upstream, change GitHub accounts, add credentials, or modify the sender.
- Keep health observations accurate. Do not force `ready=true`, delete health files, disable upload verification, or remove cached recordings on failed publication.
- Use temporary directories and loopback ports for tests. No production NAS files, configs, services, recordings, or fault injection.
- No automatic 30/60/120-second cutoff for NAS snapshot failure. New-recording admission remains separate from ongoing-recording behavior.

## Task 1: Receiver regression and fix

**Files:** `02_receiver_linux/src/detail/config_state.inl`, `recording.inl`, `receiver_app.inl`; `02_receiver_linux/CMakeLists.txt`; new `10_tests/test_receiver_nas_outage.py`.

- [ ] Build the unmodified baseline in a local Linux container and run existing receiver storage tests.
- [ ] Add a black-box test using synthetic RGB/depth packets and the receiver HTTP API. Start with a valid NAS snapshot and `shared_nas_min_free_mb > 0`, then publish `ready=false/free_bytes=null`; assert the same session remains active and frame files continue to grow. Run it on the old receiver and capture the expected failure.
- [ ] Cover stale/missing/malformed snapshots, valid but low NAS capacity during an existing session, snapshot recovery, rollover while NAS is unhealthy, and explicit user stop. Verify start admission while NAS is unhealthy still fails. Exercise a real destination capacity failure to ensure errors remain visible.
- [ ] Split the shared-NAS admission check from actual recording-root space checks. Recording writes, segment creation/rollover, and destination status must not treat NAS snapshot failure as local disk exhaustion. New-recording admission retains the original NAS checks and recovery headroom.
- [ ] Register the regression in CTest; run focused tests and the existing receiver suite.

## Task 2: Upload retention and recovery

**Files:** `05_tools/recording_uploader.py`, `10_tests/test_recording_uploader.py`, `10_tests/test_nas_discovery.py`, and any focused regression justified by the audit.

- [ ] Inspect unavailable/recovery paths and verify upload scheduling pauses without stopping the receiver or deleting local pending data.
- [ ] Test that a queued local segment survives unavailable NAS snapshots and is published after recovery, with content verification before cache cleanup.
- [ ] Change uploader code only if the tests reveal a requirement gap. Keep unrelated uploader scheduling defects out of this patch.

## Task 3: Documentation, independent review, and private branch delivery

**Files:** `04_docs/nas-recording-decoupling-20260916.md`, relevant existing recording documentation, this plan.

- [ ] Document the independent recording/NAS/upload states, behavior matrix, remaining actual-write limitations, baseline, and reproducible test commands/results.
- [ ] Request independent review of the complete diff and requirements. Fix material findings and rerun affected tests.
- [ ] Verify sender sources and field services were untouched, check diff whitespace, commit the implementation, and push only to the verified private repository.
- [ ] Verify GitHub branch commit and repository privacy; report the branch link, tests, and that deployment has not occurred.
