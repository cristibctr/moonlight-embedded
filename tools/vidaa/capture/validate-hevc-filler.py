"""Check the native packet helper against decoded hashes of a saved trace."""
import argparse
import csv
import ctypes
import json
import os
from pathlib import Path
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("run", type=Path)
    parser.add_argument("--frames", type=int, default=120)
    args = parser.parse_args()
    if not 2 <= args.frames <= 120:
        parser.error("frames must be 2..120")
    root = Path(__file__).resolve().parent
    ffmpeg = os.environ.get("MOONLIGHT_FFMPEG", "ffmpeg")
    with tempfile.TemporaryDirectory(prefix="moonlight-filler-") as scratch:
        temp = Path(scratch)
        library = temp / "helper.dylib"
        subprocess.run(["clang", "-Wall", "-Wextra", "-Werror", "-shared",
                        "-DVIDAA_TEST_LIBRARY", str(root / "test-vidaa-bitstream.c"),
                        "-o", str(library)], check=True)
        helper = ctypes.CDLL(str(library)).append_filler
        helper.argtypes = [ctypes.POINTER(ctypes.c_uint8), ctypes.c_size_t, ctypes.c_size_t]
        helper.restype = ctypes.c_size_t
        with (args.run / "incoming.csv").open() as source:
            rows = list(csv.DictReader(source))[:args.frames]
        if len(rows) != args.frames:
            raise ValueError("Not enough traced frames")
        original = bytearray()
        changed = bytearray()
        with (args.run / "incoming.hevc").open("rb") as source:
            for row in rows:
                source.seek(int(row["offset"]))
                packet = source.read(int(row["length"]))
                if len(packet) != int(row["length"]):
                    raise ValueError("Short packet")
                original.extend(packet)
                buffer = (ctypes.c_uint8 * (len(packet) + 7))()
                ctypes.memmove(buffer, packet, len(packet))
                length = helper(buffer, len(packet), len(buffer))
                if length != len(packet) + 7:
                    raise ValueError("Helper failed")
                changed.extend(bytes(buffer[:length]))
        hashes = []
        for name, payload in (("original", original), ("filler", changed)):
            path = temp / (name + ".hevc")
            path.write_bytes(payload)
            result = subprocess.run([str(ffmpeg), "-v", "error", "-xerror",
                "-i", str(path), "-map", "0:v:0", "-f", "framemd5", "-"],
                capture_output=True, text=True, check=True)
            if result.stderr.strip():
                raise ValueError(result.stderr)
            hashes.append([line.split(",")[-1].strip()
                           for line in result.stdout.splitlines() if not line.startswith("#")])
        if len(hashes[0]) != args.frames or hashes[0] != hashes[1]:
            raise ValueError("Decoded frame count or pixels changed")
        print(json.dumps({"frames": len(hashes[0]), "pixel_hashes_identical": True,
                          "bytes_added_per_frame": 7, "decoder_errors": 0}))


if __name__ == "__main__":
    main()
