# Automatic TV capture and delay test

## Current Windows benchmark (2026-09-07)

Use `tv-marker-server.c` for Windows tests. The temporary server runs on the TV,
so source calibration, capture and input traces share TV `CLOCK_MONOTONIC`.
Mac lock/sleep cannot change this subtraction. Port 5001 was unused and already
allowed by the TV firewall; no firewall rule was changed. The server accepts
only the configured Windows and Mac IPs, requires a random token, has fixed
routes with no command/file API, and stops after its bounded lifetime.

The v3 browser pattern uses header MN and a 14-by-8 grid. It includes a random
32-bit page ID covered by CRC-16. The clock checker and input/output matcher
use this ID to separate restored browser tabs. Legacy ML markers still decode.

Run `measure.py --no-marker --source-clock tv --source-label 'Windows browser'
--source-uncertainty-ms 100` with the usual duration/output arguments. The
100 ms value is provisional, not measured. Download the server JSONL after a
clock check arrives after the capture, then run `source_clock.py RUN SESSION.jsonl
--apply` followed by `analyze-trace.py RUN`. The clock check rejects missing
brackets, mixed page IDs, long gaps, reloads and drift bounds over 15 ms. Raw
samples and clock exchanges are saved even if analysis fails.

See `../STATUS.md` for the active server session, client hashes,
host config and valid results. Current test status: 38 Python tests, including
native server ASAN/UBSAN, IP restriction and expiry; eight Node fixtures.

`analyze-trace.py` requires the saved matching source-clock validation. Use
`--tv-only` if only TV input-to-capture timing is needed; this mode omits all
unvalidated host/total values and writes separate `*-tv-only*` files.

The optional `--inner-timing` capture uses a separate native binary that records
the nested `MI_CAP_CaptureOne` call without changing its arguments or pixels.
Run `inner_capture.py RUN` after trace analysis (`--tv-only` in both tools when
needed). It rejects missing, multiple, failed, or non-nested calls. These tighter
bounds are still software API intervals, not optical exposure timestamps.

The browser URL suffix `#motion` enables deterministic scrolling detail and a
native-DPR canvas. This is a synthetic load test, not proof of invisible
compression in all games. Verify `motion_mode` and `canvas_pixels` in the source
log. Source transforms reset on each draw after display-mode changes. The
64 MiB trace limit can shorten a high-bitrate motion recording well below its
requested duration; report actual trace coverage and output match count.

Verified on this NEI/VIDAA TV on 2026-09-05. No stream restart or TV remote input is needed while Moonlight is open and streaming this Mac.

## Run

From the workspace root:

```sh
sh tools/vidaa/capture/run-test.sh my-test-01 --seconds 10 --interval .25
```

Use a new run name each time. The script builds the two native tools, runs the local Python tests (and browser-fixture tests if Node.js is available), uploads the TV tool to RAM-backed `/tmp`, and records a short test. It shows a full-screen time marker on the Mac, then closes that window. Escape can close the marker early. It does not change Sunshine, the stream codec, TV picture settings, startup services, or sleep settings.

The TV binary and wrapper are temporary. Run the script again after a TV restart to upload them. SSH uses the existing `vidaa-tv` key and alias; no password or open capture server is added.

Requires the existing Zig toolchain, Swift, SSH, and Python with NumPy/Pillow. `MOONLIGHT_TEST_PYTHON` can select another Python runtime. FFmpeg is optional for the MP4 export.

## Files produced

- `tv-recording.mp4`: sampled TV output. Default is **four captures per second**, not a full-rate 60 fps recording. Do not judge stream smoothness from this file.
- `frame-NNNN.png`: each captured TV frame, including the TV overlay in default mode.
- `samples.csv`: frame ID, clock times, capture duration, delay bounds and decode errors.
- `report.json`: summary, repeat count and measurement limits.
- `clock-sync.json`: raw four-timestamp clock exchanges before and after recording.
- `tv-capture.log`: firmware capture output. No firmware setting writes are made.

`--type 2` captures video only. Default `--type 16` captures video plus OSD. `--interval 1` lowers sampling load. `--no-marker` records the current screen without a time pattern; it cannot measure delay without a visible marker.

## How the measurement works

The Mac draws a marker with a 32-bit millisecond monotonic timestamp, 16-bit frame counter, fixed header, and CRC-16. The TV hardware capture API reads the video plane and TV overlay. The recorder decodes the marker from pixels, not from Moonlight's decoder counters.

SSH clock exchanges bound the TV-minus-Mac clock offset. They use monotonic clocks; the incorrect TV wall clock does not matter. For host send/receive times `a,d` and TV receive/send times `b,c`, the offset is within `[c-d, b-a]`. The tool intersects these intervals and rejects conflicting bounds. The Swift marker and Python recorder clock agreement is checked before each run.

For each frame, the tool records the capture API call's start and end. After clock conversion, it subtracts the embedded source time to get a delay interval. The source timestamp's 1 ms rounding is included. The reported midpoint is an estimate, not an exact hardware capture time.

This is **source draw to TV capture**, not physical panel latency. It includes source rendering/capture and streaming work. It does not measure controller input, panel scanout/processing after the capture point, or sound. Sparse captures can find black output and long freezes; they cannot prove 60 fps frame pacing. A camera recording of both screens is still needed for an optical screen-to-screen comparison.

The firmware callback returns a fresh captured buffer, but does not expose its hardware sampling timestamp through this wrapper. The interval assumes the new capture occurs during the API call. A driver-side buffered frame could add bias. Do not claim sub-frame or optical accuracy from this tool without further validation.

## First measurements

Current stream: HEVC, 1080p60, 30 Mbps, `injplay`, Game Mode on. This turn did not replace the running Moonlight binary. Audio and game input were still disabled from the earlier video tests.

| Capture test | Valid samples | Median delay estimate | Median lower / upper bounds | Median capture call |
|---|---:|---:|---:|---:|
| Video + OSD, 4 samples/s | 40/40 | 211.8 ms | 186.2 / 237.3 ms | 47.4 ms |
| Video + OSD, 1 sample/s | 8/8 | 194.7 ms | 167.1 / 219.8 ms | 50.8 ms |
| Video only, 4 samples/s | 32/32 | 211.5 ms | 185.8 / 236.6 ms | 47.2 ms |

Clock uncertainty was 2.4–2.7 ms. No consecutive valid samples had the same time code. Source frame IDs advanced about 60 per second; this does not prove all those frames reached the panel. The 17 ms difference at low sampling rate may be sampling phase, stream variation, or capture load; it is not proof of capture overhead. Use repeated low-rate runs for small changes.

The final `run-test.sh` end-to-end check also passed: 6/6 markers, MP4 export, and seven local unit tests. Its median delay estimate was 213.3 ms. Results are in `runs/20260905-runner-check/`.

The previous user estimate of 100 ms was subjective. These first instrumented runs suggest about 200 ms on the measured software path, with roughly a 50 ms-wide per-frame capture interval. Do not present either number as exact panel delay.

## Firmware ABI

`capture.c` loads `libmi.so` and `libHui.so` in a separate ARM32 process. It uses `MI_SYS_Init`, `MI_DISP_Init`, `HUI_ScreenCaptureAvaliable` (firmware spelling), and `HUI_ScreenCapture`.

The recovered bitmap layout is `(uint8_t *pixels, uint32_t length, uint32_t width, uint32_t height)`. Type 16 maps to video+OSD; type 2 maps to main video. On this firmware, capture writes BGRA pixels and leaves the supplied length unchanged. It also clears one byte beyond the pixel payload. The tool allocates a full 1920×1080×4 buffer plus 4096 bytes, validates returned dimensions/pointer, and sends only `width*height*4` bytes. The reference red/blue squares confirmed byte order.

`libHui.so` needs lazy symbol resolution for this standalone capture path. Do not preload `libdfb_renderclient.so`: without the full player dependency set it fails on `DirectFBCreate`. No arbitrary frame-buffer or physical-memory dump is used.

The process serves commands only over its SSH stdin/stdout. Firmware logs go to stderr. Each native capture call has a 15-second watchdog; the recorder times out after 20 seconds. These timeouts affect only the capture test, not the Moonlight process.

## Next improvements

1. Add a firmware capture callback timestamp or a persistent native capture path to reduce the 47–51 ms timing interval.
2. Repeat low-rate measurements before and after each stream change, using the same marker and duration.
3. Add high-rate sampling or a camera check for frame pacing. Do not infer smoothness from the sampled MP4.
4. Add an optical screen-to-screen comparison to quantify the part of visible delay this capture cannot measure.

## Input/output frame matching (later 2026-09-05 work)

The TV client now has an optional compressed-frame trace. Set `trace_seconds=18` in RAM-only `/tmp/moonlight-video-tuning.conf`, restart the stream, and immediately run a 20-second capture test. Recording stops by itself; no trace is made without this option. Set baseline tuning after tests. Restarting can occasionally fail with an empty launcher log: check `/status` and fresh decoder logs, not only the `/start` response.

Copy `/tmp/moonlight-video-trace.bin` to the run's `incoming.hevc` and `/tmp/moonlight-video-trace.csv` to `incoming.csv`. Verify that both are from this run, not a failed start. Then run:

```sh
python3 work/moonlight-capture/analyze-trace.py work/moonlight-capture/runs/RUN_NAME
```

Use the Python runtime above with NumPy/Pillow. Add `--codec h264` for H.264; the input filename stays `incoming.hevc`. Run offline decoding after live measurement to avoid extra load on the source computer.

This produces `pipeline-report.json`, `decoded-input.csv`, and `matched-output.csv`. It decodes each compressed input frame and matches its visible time marker to TV captures. It rejects B-frames and input/decode frame-count mismatch. Repeated input markers widen the TV timing bounds.

Repeated HEVC measurements found about 96–104 ms from source draw to TV first packet, under 0.2 ms of client queue time, about 0.6 ms per decoder write, and roughly 76–92 ms from first packet to TV capture midpoint. The last value has a wide capture-time interval and is not pure decode time. Overall measured medians ranged about 175–195 ms in these traced tests. No tested queue/sync setting gave a repeatable gain. See `../VIDAA_MOONLIGHT_NOTES.md` for rejected tests and current state.

## Windows source pattern

`web-marker.py` serves a fixed canvas pattern and four-timestamp clock exchanges from this Mac. It binds to `192.168.1.132:47986` and accepts only that Mac and Windows `192.168.1.139`. Routes use a random session token. It has no file browsing or command API. The server stops after 15 minutes by default (`--seconds 60..3600` can set a bounded test duration); the page expires after 30 minutes. `--output SESSION.json` saves the last 180 calibration checks and ten text diagnostic reports locally. Stop its own process when testing ends. Do not add a persistent service or broad firewall rule.

The browser estimates its `performance.now()` offset to the Mac monotonic clock with 24 exchanges, then renders markers in the Mac clock domain. It repeats calibration every 20 seconds but keeps `render_offset_ms` fixed, so clock checks do not introduce time steps. Check the recorded low/high intervals against that fixed value before AND after the run. Use the largest absolute difference plus a stated allowance for drift between checks as `--no-marker --source-label 'Windows browser' --source-uncertainty-ms VALUE`. The interval does not prove the absence of clock changes between checks. Do not treat an assumed drift allowance as a measured bound. A browser marker also has a different render path from the native Mac marker.

Windows validation: the RTX 4070 Ti host running stock Sunshine now pairs and streams HEVC 1080p60 to the TV. Hardware captures decode the Windows browser pattern correctly. The first three-frame check estimated 79.4 ms, but had an old clock calibration and no end check; it is exploratory only (see `20260905-windows-steady-picture/report.json`). The new repeated calibration reaches this Mac from Windows `.139`, with 2560x1440 full-screen viewport after initial setup. TV network access failed before a full traced test. Do not claim precise Windows latency or console-ready performance yet.

### Offline clock validation

Keep the server's `--output` session file. After a Windows capture, allow the next automatic clock check to arrive. Then run:

```sh
python3 work/moonlight-capture/source_clock.py RUN_DIRECTORY SESSION.json --apply
python3 work/moonlight-capture/analyze-trace.py RUN_DIRECTORY
```

The first command requires clock checks from the Windows IP before and after the captured frames. It rejects page reloads and check gaps over 45 seconds. It calculates the maximum error against the fixed rendered clock, plus an explicit assumed drift allowance (default 100 ppm). Without `--apply`, it only prints the result. A failed validation does not change the saved run. Treat the initial capture report as provisional until this check passes. The original three-frame Windows run cannot pass with the later session file; that file is not from the same time interval.

The trace report now includes lower and upper host-to-receive bounds, including both clock errors and source timestamp rounding. Its midpoint is 0.5 ms below the old floor-timestamp estimate. This is an accounting change, not a latency improvement. An offline rerun of the saved Mac baseline still matched all 1,066 input markers and 35 output samples; TV residence was unchanged at a 79.906 ms midpoint. The original run was preserved; the check is in `20260905-offline-clock-bounds-check`.

### Browser pattern cadence

The old frame limiter produced 48 marker updates/s on a 144 Hz display. The revised limiter keeps a fixed 60 Hz time grid and skips missed slots after a pause. Six Node.js tests check 60/120/144/165 Hz source callbacks, pause recovery, and stable clock offset during repeated checks. These are synthetic tests, not proof of real browser or TV frame pacing. The corrected fixture still needs a live Windows/TV run. There are also 22 passing Python tests.
