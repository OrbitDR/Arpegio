"""Writes the 24-bit noise files used by `arpegio-cli loopback`, at about -41 dBFS.

Usage: make_noise.py OUTPUT_FOLDER
"""
import os
import sys

import numpy as np
import soundfile as sf

folder = sys.argv[1]
rng = np.random.default_rng(5)
for rate in (44100, 96000, 192000):
    samples = rng.integers(-70000, 70000, size=(rate * 5, 2), dtype=np.int32) * 256
    sf.write(os.path.join(folder, f"noise_{rate}_24.wav"), samples, rate, subtype="PCM_24")
