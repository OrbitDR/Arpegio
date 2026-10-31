"""Measures DSD to PCM conversions: tone level, THD+N and noise per band.

Usage: dsd_quality.py FILE.wav [FILE.wav ...]   prints one row per file and channel 0
       dsd_quality.py --tone HZ FILE.wav ...    also reports the tone and in-band THD+N

Levels are in dBFS, where a full-scale sine reads 0.0.
"""
import sys

import numpy as np
import soundfile as sf
from scipy import signal

SKIP_SECONDS = 0.05
BANDS = [(20, 20_000), (20_000, 30_000), (30_000, 50_000), (50_000, 100_000), (100_000, 200_000), (200_000, 1e9)]


def spectrum(x, rate):
    """Power per bin, scaled so a full-scale sine sums to 0.5 across its main lobe."""
    n = 2 ** int(np.floor(np.log2(len(x))))
    x = x[-n:]
    window = signal.windows.kaiser(n, 38.0, sym=False)
    power = np.abs(np.fft.rfft(x * window)) ** 2 / (n * np.sum(window**2))
    power[1:-1] *= 2
    return np.fft.rfftfreq(n, 1 / rate), power


def band_db(freq, power, low, high):
    mask = (freq >= low) & (freq < high)
    if not mask.any():
        return None
    return 10 * np.log10(np.sum(power[mask]) * 2 + 1e-40)


def analyse(path, tone_hz):
    data, rate = sf.read(path, dtype="float64", always_2d=True)
    x = data[int(SKIP_SECONDS * rate):, 0]
    freq, power = spectrum(x, rate)
    print(f"{path}  rate={rate}  seconds={len(x) / rate:.2f}")
    if tone_hz:
        bin_width = freq[1]
        lobe = int(np.ceil(12 * 1.0))  # Kaiser beta 38 main lobe half width in bins
        centre = int(round(tone_hz / bin_width))
        tone = np.sum(power[centre - lobe : centre + lobe + 1])
        residual = power.copy()
        residual[centre - lobe : centre + lobe + 1] = 0
        print(f"  tone {tone_hz} Hz: {10 * np.log10(tone * 2):.3f} dBFS")
        in_band = band_db(freq, residual, 20, 20_000)
        print(f"  noise+distortion 20 Hz-20 kHz: {in_band:.1f} dBFS, ratio {in_band - 10 * np.log10(tone * 2):.1f} dB")
        harmonics = [10 * np.log10(np.sum(power[h * centre - lobe : h * centre + lobe + 1]) * 2 + 1e-40) for h in (2, 3, 5)]
        print("  H2 H3 H5: " + "  ".join(f"{h:.1f}" for h in harmonics))
    else:
        residual = power
    row = []
    for low, high in BANDS:
        db = band_db(freq, residual, low, high)
        if db is not None:
            row.append(f"{low / 1000:g}-{min(high, rate / 2) / 1000:g}k: {db:.1f}")
    print("  bands dBFS  " + " | ".join(row))


if __name__ == "__main__":
    args = sys.argv[1:]
    tone_hz = 0.0
    if args and args[0] == "--tone":
        tone_hz = float(args[1])
        args = args[2:]
    for path in args:
        analyse(path, tone_hz)
