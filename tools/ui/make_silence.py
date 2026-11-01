"""Digital silence with tags, so the application can run through real outputs without a sound.

    python tools/ui/make_silence.py FOLDER [COVER.jpg]

Writes an album of four tracks: two CD-quality WAV, one 24/96 WAV and one quiet DSD64 file. The DSD file holds
only the modulator's noise, which sits above the audible band. Needs numpy for its noise shaper. Without a cover
it draws a plain one, large enough to get a thumbnail of its own as a real cover would.
"""
import shutil
import struct
import subprocess
import sys
import zlib
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
ALBUM = "Pruebas de silencio"
SECONDS = 40
COVER_SIZE = 640
COVER_COLOR = bytes((0x3A, 0x6E, 0x8F))


def write_cover(path):
    def chunk(tag, data):
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data))

    header = struct.pack(">IIBBBBB", COVER_SIZE, COVER_SIZE, 8, 2, 0, 0, 0)
    rows = (b"\0" + COVER_COLOR * COVER_SIZE) * COVER_SIZE
    path.write_bytes(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", header) + chunk(b"IDAT", zlib.compress(rows, 9)) + chunk(b"IEND", b""))


def info_chunk(title, number):
    def field(tag, text):
        data = text.encode("utf-8") + b"\0"
        if len(data) % 2:
            data += b"\0"
        return tag + struct.pack("<I", len(data)) + data

    body = b"INFO" + field(b"INAM", title) + field(b"IART", "Arpegio") + field(b"IPRD", ALBUM) + field(b"ITRK", str(number))
    return b"LIST" + struct.pack("<I", len(body)) + body


def write_wav(path, rate, bits, title, number):
    frame = 2 * bits // 8
    data = bytes(SECONDS * rate * frame)
    header = b"fmt " + struct.pack("<IHHIIHH", 16, 1, 2, rate, rate * frame, frame, bits)
    body = b"WAVE" + header + info_chunk(title, number) + b"data" + struct.pack("<I", len(data)) + data
    path.write_bytes(b"RIFF" + struct.pack("<I", len(body)) + body)


def main():
    folder = Path(sys.argv[1]) / ALBUM
    folder.mkdir(parents=True, exist_ok=True)
    if len(sys.argv) > 2:
        shutil.copy(sys.argv[2], folder / "cover.jpg")
    else:
        write_cover(folder / "cover.png")

    write_wav(folder / "01.wav", 44100, 16, "Silencio CD uno", 1)
    write_wav(folder / "02.wav", 44100, 16, "Silencio CD dos", 2)
    write_wav(folder / "03.wav", 96000, 24, "Silencio 24 bits", 3)

    noise_shaper = folder.parent / "ntf64.txt"
    with noise_shaper.open("w") as out:
        subprocess.run([sys.executable, str(REPO / "tools" / "measure" / "design_ntf.py"), "64"], stdout=out, check=True)
    generator = REPO / "build" / "release" / "arpegio-dsdgen.exe"
    subprocess.run([str(generator), str(folder / "04.dsf"), str(noise_shaper), "2822400", str(SECONDS)], check=True)
    noise_shaper.unlink()


if __name__ == "__main__":
    main()
