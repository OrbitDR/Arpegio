"""Counts DSD rates in a music folder by reading DSF, DFF and SACD ISO headers.

Usage: scan_dsd_library.py FOLDER [FOLDER ...]
"""
import collections
import os
import struct
import sys


def dsf_info(path):
    with open(path, "rb") as f:
        head = f.read(92)
    if head[:4] != b"DSD " or head[28:32] != b"fmt ":
        return None
    channels, rate = struct.unpack_from("<II", head, 52)
    return f"DSF {rate / 44100:.0f}x {channels}ch", path


def dff_info(path):
    with open(path, "rb") as f:
        head = f.read(16)
        if head[:4] != b"FRM8" or head[12:16] != b"DSD ":
            return None
        rate = channels = None
        compressed = "?"
        end = struct.unpack(">Q", head[4:12])[0] + 12
        while f.tell() < end:
            chunk = f.read(12)
            if len(chunk) < 12:
                break
            cid, size = chunk[:4], struct.unpack(">Q", chunk[4:])[0]
            if cid == b"PROP":
                f.read(4)
                prop_end = f.tell() + size - 4
                while f.tell() < prop_end:
                    sub = f.read(12)
                    sid, ssize = sub[:4], struct.unpack(">Q", sub[4:])[0]
                    body = f.read(ssize + (ssize & 1))
                    if sid == b"FS  ":
                        rate = struct.unpack(">I", body[:4])[0]
                    elif sid == b"CHNL":
                        channels = struct.unpack(">H", body[:2])[0]
                    elif sid == b"CMPR":
                        compressed = body[:4].decode("latin1").strip()
                continue
            f.seek(size + (size & 1), 1)
            if cid in (b"DSD ", b"DST "):
                break
    return f"DFF {(rate or 0) / 44100:.0f}x {channels}ch {compressed}", path


def iso_info(path):
    with open(path, "rb") as f:
        f.seek(510 * 2048)
        magic = f.read(8)
    if magic == b"SACDMTOC":
        return "SACD ISO", path
    return "ISO no SACD", path


def main():
    counts = collections.Counter()
    sample = {}
    for root in sys.argv[1:]:
        for folder, _, names in os.walk(root):
            for name in names:
                ext = os.path.splitext(name)[1].lower()
                reader = {".dsf": dsf_info, ".dff": dff_info, ".iso": iso_info}.get(ext)
                if reader is None:
                    continue
                path = os.path.join(folder, name)
                try:
                    info = reader(path)
                except (OSError, struct.error, IndexError) as error:
                    info = (f"error {type(error).__name__}", path)
                key, where = info or (f"cabecera invalida {ext}", path)
                counts[key] += 1
                sample.setdefault(key, where)
    for key, count in sorted(counts.items()):
        print(f"{count:5d}  {key:24s}  ej: {sample[key]}")


if __name__ == "__main__":
    main()
