# Test status — 2026-09-07

The goal remains true 4K60 with high quality and low game delay. Not complete.
The latest working path is native INJPLAY, HEVC, SDR Rec. 709, requested 80 Mbps.
Audio and controller input are still disabled. No Sunshine binary was modified.

## Results

All delay values below are software intervals, not optical panel or controller
latency. Sparse captures cannot prove 60 displayed frames per second.

| Test | Source draw to TV capture, median | Finding |
| --- | ---: | --- |
| Static marker, host 60 Hz | 150.254 ms | Host component 66.444 ms |
| Matching marker, host 120 Hz, TV 60 Hz | 103.149 ms | Host component 15.366 ms |
| 4K moving detail, host 120 Hz | 117.594 ms | Bounds 87.602–147.455 ms |

The moving-detail test matched 17 captured outputs to 527 decoded input markers,
with no B frames. The trace covered 8.76 seconds, about 60 incoming frames/s.
The inner capture call gives TV residence about 77.119 ms, bounded by
63.535–90.449 ms. This is tighter accounting, not a physical speed improvement.
Synthetic detail is not proof of imperceptible compression in all games.

Host 120 Hz removed about 51 ms in the matching browser test. This is not proof
of the same gain in every game. The stock Windows Sunshine display configuration
used auto resolution, manual 120 Hz, HDR disabled, verify-only display handling,
and revert-on-disconnect enabled. The TV stream remains 4K60.

No clear gain: combined queue settings, HEVC filler NAL, single NVENC engine,
Game Mode cycle, two-reference-frame negotiation, and forced free-run sync.
Read-back showed free-run was already active in the reference path. These tests
do not justify changing the defaults. A one-frame-gap run was captured but its
matched trace analysis was not completed when publication work started.

## Start crash fixed

An intermittent SIGILL occurred in OpenSSL `_armv7_tick` during TLS setup, before
decoder startup. The VIDAA ARM build now clears only the optional timer capability
bit before connecting. Crypto acceleration and OS entropy remain enabled.
Three consecutive starts then passed with moving captures. A forced timer-bit
test also passed. The bridge now records child exit code/signal and avoids a
SIGCHLD registration race. This improves reliability, not video delay.

## Measurement safeguards

The TV provides the monotonic reference clock through a bounded, token-protected
test server. v3 markers include a CRC-protected random page ID. Source analysis
requires matching, bracketing clock checks. Stale-clock runs are TV-only; never
report their provisional host/total values. Traces stop at 64 MiB, so moving
detail can reach the cap before the requested trace duration.

## Next work

Publication checks passed: 38 Python tests, 8 browser fixtures, and native codec,
bitstream, runtime, capture-forwarding, and bridge tests under ASAN/UBSAN. The
ARM client build passed with the existing local dependency tree. This does not
mean a clean machine can build without supplying the stated dependencies.

Reduce the roughly 70–80 ms TV residence. Normal tuning has not shown a clear
gain. A decoder handshake/manual display path is under ABI study, not implemented.
Do not call it safe or working yet. Then implement and measure audio and input.
