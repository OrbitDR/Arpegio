"""Designs the noise transfer function used by arpegio-dsdgen.

Prints numerator then denominator, each highest power first with a leading 1.
Usage: design_ntf.py OSR > ntf.txt
"""
import sys

import numpy as np
from scipy import signal

ORDER = 7
H_INF = 1.5


def design(osr):
    # Legendre roots are the zero positions that minimise in-band noise.
    roots = np.polynomial.legendre.leggauss(ORDER)[0]
    zeros = np.exp(1j * roots * np.pi / osr)
    numerator = np.real(np.poly(zeros))

    def denominator(cutoff):
        _, poles, _ = signal.butter(ORDER, cutoff, "high", output="zpk")
        return np.real(np.poly(poles))

    def gain(cutoff):
        return abs(np.polyval(numerator, -1) / np.polyval(denominator(cutoff), -1))

    low, high = 1e-4, 0.9
    for _ in range(80):
        mid = (low + high) / 2
        if gain(mid) > H_INF:
            high = mid
        else:
            low = mid
    return numerator, denominator(low)


if __name__ == "__main__":
    b, a = design(int(sys.argv[1]))
    print(" ".join(f"{c:.17g}" for c in b))
    print(" ".join(f"{c:.17g}" for c in a))
