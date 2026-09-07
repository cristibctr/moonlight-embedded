# Experimental VIDAA port

This is work in progress for an ARMv7 NEI VIDAA TV. It is not a general VIDAA
installer. Audio and controller input are disabled in the tested launcher.
Do not treat it as ready for games. See [STATUS.md](STATUS.md) for measured results.

## Contents

- `../../src/video/vidaa.c`: hardware video backends, verified ABI layouts,
  timestamps, bounded traces, and optional RAM-only tuning.
- `launcher/`: TV-remote UI, stream lifecycle bridge, client wrapper, display
  probes, tuning fixtures, and an app metadata example.
- `capture/`: automatic TV capture, clock calibration, moving markers, trace
  analysis, and regression tests. See its README for measurement limits.
- `vidaa-clock/`: earlier process-local timing experiments. Not the active
  INJPLAY solution. Do not preload it globally.
- `inspect-dwarf.py`: read-only ABI inspection of locally supplied ELF files.
- `windows-benchmark.ps1` and its JSON builder: launch the installed Vivaldi
  browser for the test. Set `MOONLIGHT_BENCHMARK_BASE` in the launched process
  environment to the fresh TV server URL. Inspect the generated Sunshine app
  index before adding it; do not overwrite another app. No API password is used.

The original workspace copies remain local. Use these repository copies for
future source changes. Private session notes, pairing data, credentials, dumps,
recordings, generated binaries, TV firmware libraries, and third-party source
archives are deliberately not published. No Sunshine source was changed.

## Build

Clone this branch with `--recurse-submodules`. The common library points to the
matching fork commit, not its current upstream master. This preserves the tested
protocol version.

The native build needs Zig, CMake, and separately supplied ARM32 dependencies.
Set `VIDAA_ZIG` if Zig is not on PATH. The current dependency layout is:

- `VIDAA_SYSROOT`: a local TV library tree; firmware files are not distributed.
- `VIDAA_VENDOR_ROOT`: OpenSSL 1.1.1i, curl 7.77.0, Opus 1.3.1, libevdev 1.13.1,
  expat 2.2.10, and prepared `alsa-include`, `vidaa-include` headers.
- OpenSSL static libraries reside in its source tree. The ARM curl library is
  `VIDAA_VENDOR_ROOT/../curl-build-vidaa/lib/libcurl.a`.
- `VIDAA_OPUS_LIBRARY`: the ARM static Opus library.

These old dependencies reproduce the experiment; they are not a recommendation
for a new public network service. Dependency preparation is not automated yet.

```sh
cmake -S . -B build-vidaa \
  -DCMAKE_TOOLCHAIN_FILE=tools/vidaa/vidaa-toolchain.cmake \
  -DENABLE_VIDAA=ON \
  -DVIDAA_SYSROOT=/path/to/tv-sysroot \
  -DVIDAA_VENDOR_ROOT=/path/to/vendor \
  -DVIDAA_OPUS_LIBRARY=/path/to/libopus.a
cmake --build build-vidaa --parallel
```

Python tests need NumPy and Pillow. Analysis also needs FFmpeg on PATH, or
`MOONLIGHT_FFMPEG`. The Mac-only marker needs Swift.

```sh
python3 -m unittest discover -s tools/vidaa/capture -p 'test_*.py'
node --test tools/vidaa/capture/test_web_marker.js
```

## Deployment safety

Deployment is manual and firmware-specific. Back up each exact target first.
Do not overwrite the TV's complete app database with the metadata example.
The current bridge binds port 47985 on all interfaces without authentication.
Use only an isolated trusted LAN; do not expose or forward this port. Host
addresses in examples must be adapted. Pair each host using Moonlight's normal
pairing flow; do not put pairing keys or Sunshine passwords in Git.

No root-access installer, SSH keys, reverse-shell service, firmware patch, or
firewall change is included. Existing authorized shell access is a prerequisite.
The optional display probes can alter picture state; read their arguments and
ownership checks before use. Never force undocumented panel capability flags.
