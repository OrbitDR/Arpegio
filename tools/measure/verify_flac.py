"""Checks the engine's FLAC path against the MD5 stored in each file's STREAMINFO block.

The CLI dumps S32 samples, which this script narrows to the file's bit depth before hashing,
so a pass proves every sample left the decoder and the double conversion unchanged.
Usage: verify_flac.py CLI_EXE FILE.flac [FILE.flac ...]
"""
import hashlib
import os
import subprocess
import sys
import tempfile

import numpy as np


def stream_info(path):
    with open(path, "rb") as f:
        if f.read(4) != b"fLaC":
            raise ValueError("not FLAC")
        while True:
            header = f.read(4)
            last, kind, size = header[0] >> 7, header[0] & 127, int.from_bytes(header[1:], "big")
            body = f.read(size)
            if kind == 0:
                packed = int.from_bytes(body[10:18], "big")
                bits = ((packed >> 36) & 31) + 1
                channels = ((packed >> 41) & 7) + 1
                return bits, channels, body[18:34].hex()
            if last:
                raise ValueError("no STREAMINFO")


def narrow(block, bits):
    samples = np.frombuffer(block, dtype="<i4") >> (32 - bits)
    width = (bits + 7) // 8
    if width == 4:
        return samples.astype("<i4").tobytes()
    if width == 2:
        return samples.astype("<i2").tobytes()
    if width == 1:
        return samples.astype("i1").tobytes()
    packed = samples.astype("<i4").tobytes()
    return np.frombuffer(packed, dtype="u1").reshape(-1, 4)[:, :3].tobytes()


def main():
    cli = sys.argv[1]
    failures = 0
    checked = 0
    for path in sys.argv[2:]:
        bits, channels, expected = stream_info(path)
        with tempfile.TemporaryDirectory() as folder:
            raw = os.path.join(folder, "out.raw")
            subprocess.run([cli, "decode", path, raw], check=True, stdout=subprocess.DEVNULL)
            digest = hashlib.md5()
            with open(raw, "rb") as f:
                while True:
                    block = f.read(1 << 24)
                    if not block:
                        break
                    digest.update(narrow(block, bits))
        if expected == "0" * 32:
            status = "no MD5 in file"
        elif digest.hexdigest() == expected:
            status = "OK"
            checked += 1
        else:
            status = "MISMATCH"
            failures += 1
        print(f"{status:14s} {bits:2d} bit {channels} ch  {os.path.basename(path)[:60]}")
    print(f"{checked} verified, {failures} mismatches")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
