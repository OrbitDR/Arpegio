"""Compares ASIO tap captures with the PCM of a WAV file, sample by sample.

Usage: compare_tap.py SOURCE.wav CAPTURE.raw [CAPTURE.raw ...]

Each capture needs its .info beside it. Only 16 and 24-bit WAV sources and Int32LSB captures are
handled, which is what the Topping and the MiniFuse use. Silence at the ends is skipped, since a
player starts and stops around the music. With two captures or more, each pair is also compared.
"""
import struct
import sys
from pathlib import Path

import numpy as np

INT32_LSB = 18
PROBE_FRAMES = 4096


def read_wav(path):
    data = Path(path).read_bytes()
    position = 12
    fmt = None
    while position + 8 <= len(data):
        tag = data[position:position + 4]
        size = struct.unpack_from("<I", data, position + 4)[0]
        body = position + 8
        if tag == b"fmt ":
            fmt = struct.unpack_from("<HHIIHH", data, body)
        elif tag == b"data":
            break
        position = body + size + (size & 1)
    _, channels, rate, _, _, bits = fmt
    raw = data[body:body + size]
    if bits == 16:
        samples = np.frombuffer(raw, dtype="<i2", count=len(raw) // 2).astype(np.int32) << 16
    elif bits == 24:
        count = len(raw) // 3
        bytes_ = np.frombuffer(raw, dtype=np.uint8, count=count * 3).reshape(-1, 3).astype(np.uint32)
        samples = ((bytes_[:, 0] << 8) | (bytes_[:, 1] << 16) | (bytes_[:, 2] << 24)).astype(np.int32)
    else:
        raise SystemExit(f"{bits}-bit sources are not handled")
    return samples.reshape(-1, channels), rate, bits


def read_capture(path):
    info = dict(line.split("=", 1) for line in Path(path).with_suffix(".info").read_text().split())
    if int(info["type"]) != INT32_LSB:
        raise SystemExit(f"{path}: sample type {info['type']} is not Int32LSB")
    channels = int(info["channels"])
    samples = np.fromfile(path, dtype="<i4")
    samples = samples[: len(samples) - len(samples) % channels].reshape(-1, channels)
    return samples, info


def trim_silence(samples):
    active = np.flatnonzero(np.any(samples != 0, axis=1))
    if len(active) == 0:
        return samples[:0], 0
    return samples[active[0] : active[-1] + 1], int(active[0])


def find_exact(source, capture):
    """Where the capture's middle occurs in the source, or None when it does not occur unchanged."""
    if len(capture) < PROBE_FRAMES:
        return None
    start = len(capture) // 2
    probe = capture[start : start + PROBE_FRAMES].tobytes()
    frame_bytes = 4 * source.shape[1]
    found = source.tobytes().find(probe)
    if found < 0 or found % frame_bytes:
        return None
    return found // frame_bytes - start


def find_by_correlation(source, capture):
    """Offset of the capture in the source and the correlation peak, from the mono sums."""
    a = source.astype(np.float64).sum(axis=1)
    b = capture.astype(np.float64).sum(axis=1)
    size = 1 << int(np.ceil(np.log2(len(a) + len(b))))
    spectrum = np.fft.rfft(a, size) * np.conj(np.fft.rfft(b, size))
    correlation = np.fft.irfft(spectrum, size)
    peak = int(np.argmax(np.abs(correlation)))
    if peak > size // 2:
        peak -= size
    return peak


def overlap(source, capture, offset):
    begin = max(0, -offset)
    end = min(len(capture), len(source) - offset)
    return source[offset + begin : offset + end], capture[begin:end], begin


def dbfs(value):
    return 20.0 * np.log10(max(value, 1e-30))


def report_difference(reference, capture):
    a = reference.astype(np.float64) / 2147483648.0
    b = capture.astype(np.float64) / 2147483648.0
    gain = float(np.dot(a.ravel(), b.ravel()) / np.dot(a.ravel(), a.ravel()))
    residual = b - gain * a
    difference = capture.astype(np.int64) - reference.astype(np.int64)
    equal = int(np.count_nonzero(np.all(difference == 0, axis=1)))
    print(f"  frames compared: {len(capture)}, identical: {equal} ({100.0 * equal / max(len(capture), 1):.4f} %)")
    polarity = ", polarity inverted" if gain < 0 else ""
    print(f"  best-fit gain: {gain:.9f} ({dbfs(abs(gain)):+.5f} dB{polarity})")
    print(f"  residual after that gain: {dbfs(float(np.sqrt(np.mean(residual ** 2)))):.2f} dBFS RMS")
    print(f"  largest difference: {int(np.abs(difference).max())} in 32-bit units")
    low_bits = int(np.bitwise_or.reduce(capture.ravel().astype(np.int64) & 0xFFFF))
    print(f"  lower 16 bits used: {'no' if low_bits == 0 else f'yes, mask {low_bits:#06x}'}")
    return equal == len(capture)


def main():
    if len(sys.argv) < 3:
        raise SystemExit(__doc__)
    source, source_rate, bits = read_wav(sys.argv[1])
    print(f"source: {len(source)} frames, {source_rate} Hz, {bits} bit, {source.shape[1]} channels")
    placed = []
    for path in sys.argv[2:]:
        print(f"\n{Path(path).name}")
        whole, info = read_capture(path)
        print(f"  {len(whole)} frames, {info['rate']} Hz, buffer {info['buffer']}, mute {info['mute']}")
        capture, lead = trim_silence(whole)
        print(f"  {lead} silent frames at the start, {len(capture)} with signal")
        if int(float(info["rate"])) != source_rate:
            print(f"  DIFFERENT RATE: the player resampled from {source_rate} to {info['rate']}")
            continue
        offset = find_exact(source, capture)
        if offset is None:
            offset = find_by_correlation(source, capture)
            print(f"  the music is not in the capture unchanged; correlation places it at frame {offset}")
        else:
            print(f"  found unchanged at frame {offset} of the source")
        reference, compared, begin = overlap(source, capture, offset)
        report_difference(reference, compared)
        placed.append((Path(path).name, offset, capture))

    for i, (name_a, offset_a, a) in enumerate(placed):
        for name_b, offset_b, b in placed[i + 1 :]:
            print(f"\n{name_a} against {name_b}")
            begin = max(offset_a, offset_b)
            end = min(offset_a + len(a), offset_b + len(b))
            if end <= begin:
                print("  no music in common")
                continue
            report_difference(a[begin - offset_a : end - offset_a], b[begin - offset_b : end - offset_b])


main()
