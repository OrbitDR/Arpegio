"""Downloads pinned third-party sources into third_party/ and verifies their SHA-256.

Nothing is executed: every entry is source code or a development package.
Usage: python scripts/fetch_deps.py [name ...]
"""
import hashlib
import io
import pathlib
import shutil
import sys
import tarfile
import urllib.request
import zipfile

ROOT = pathlib.Path(__file__).resolve().parent.parent
THIRD_PARTY = ROOT / "third_party"
CACHE = THIRD_PARTY / ".cache"

GITHUB = "https://github.com"

# name, url, sha256, leading path components to drop when extracting
DEPS = [
    ("asiosdk", "https://download.steinberg.net/sdk_downloads/ASIO-SDK_2.3.4_2025-10-15.zip",
     "d5ebf0c20dd2c5f43771fd0c1418f4b361bf52434ee670097cfa6b3a335e2eca", 1),
    ("r8brain", f"{GITHUB}/avaneev/r8brain-free-src/archive/cb2abb9977efe2471979b380ed95daa56ab4fdb9.tar.gz",
     "bc8abccc844025101936969524cc2f1d650cdf0d8edc0ff00261edf40f0da12f", 1),
    ("soxr", f"{GITHUB}/chirlu/soxr/archive/refs/tags/0.1.3.tar.gz", "db6ca1b1e8405c6ef92f8294fc123d910abf0a114003b3f0f13fa57a95fd62d0", 1),
    ("ogg", f"{GITHUB}/xiph/ogg/archive/refs/tags/v1.3.6.tar.gz", "95b643da661155d79db9de2fca55daed3a8d491039829def246aacb3d9201c81", 1),
    ("flac", f"{GITHUB}/xiph/flac/archive/refs/tags/1.5.0.tar.gz", "aea54ed186ad07a34750399cb27fc216a2b62d0ffcd6dc2e3064a3518c3146f8", 1),
    ("vorbis", f"{GITHUB}/xiph/vorbis/archive/refs/tags/v1.3.7.tar.gz", "270c76933d0934e42c5ee0a54a36280e2d87af1de3cc3e584806357e237afd13", 1),
    ("wavpack", f"{GITHUB}/dbry/WavPack/archive/refs/tags/5.9.0.tar.gz", "2a53e50aefd8c9f04a0828a0e1ef77b6f4c17b0ef6964ad234ab295f313b7d6d", 1),
    ("monkeysaudio", "https://monkeysaudio.com/files/MAC_1345_SDK.zip", "b12bda0eb9e31dbe01d54a970d387add3a8c1079901141021d257de17a6a9106", 0),
    ("minimp3", f"{GITHUB}/lieff/minimp3/archive/ea99364f61c14656440e8d77e9c233ccf3124633.tar.gz", "5628166eb82a9bb581317918a334c317a2c0a30278bb14a20381307976768f34", 1),
    ("json", f"{GITHUB}/nlohmann/json/archive/refs/tags/v3.12.0.tar.gz", "4b92eb0c06d10683f7447ce9406cb97cd4b453be18d7279320f7b2f025c10187", 1),
    ("sqlite", "https://www.sqlite.org/2026/sqlite-amalgamation-3540100.zip", "0a9c8e34352dad46006aa82836d5383bff9dc6bc9ce5a60553c05aa30e6ad1f9", 1),
    ("taglib", f"{GITHUB}/taglib/taglib/archive/refs/tags/v2.3.2.tar.gz", "cda2d6dac7f12a84e46fbb0907bb1a1ec0679e4722b5237576be5563e295244e", 1),
    ("utfcpp", f"{GITHUB}/nemtrif/utfcpp/archive/refs/tags/v4.2.1.tar.gz", "6d6a5493a111884cc085ee31babfe6d9960c8fb08fc80a64852eaeea8323dbc1", 1),
    ("webview2", "https://api.nuget.org/v3-flatcontainer/microsoft.web.webview2/1.0.4314.35/"
     "microsoft.web.webview2.1.0.4314.35.nupkg", "90778deb1b77e36292828b4b5833c9255c27533f40e0ddd55891ae718944edc4", 0),
]


def download(name, url):
    CACHE.mkdir(parents=True, exist_ok=True)
    target = CACHE / (name + "-" + url.rsplit("/", 1)[1])
    if not target.exists():
        print(f"downloading {url}")
        request = urllib.request.Request(url, headers={"User-Agent": "arpegio-fetch-deps"})
        with urllib.request.urlopen(request, timeout=300) as response:
            data = response.read()
        target.write_bytes(data)
    return target


def inside(destination, relative):
    path = (destination / relative).resolve()
    if destination.resolve() not in path.parents:
        raise RuntimeError(f"archive entry escapes the target: {relative}")
    return path


def extract(archive, destination, strip):
    if destination.exists():
        shutil.rmtree(destination)
    destination.mkdir(parents=True)
    data = archive.read_bytes()
    if tarfile.is_tarfile(io.BytesIO(data)):
        with tarfile.open(fileobj=io.BytesIO(data)) as tar:
            members = [(m.name, m) for m in tar.getmembers() if m.isfile()]
            for name, member in members:
                parts = pathlib.PurePosixPath(name).parts[strip:]
                if parts:
                    path = inside(destination, pathlib.Path(*parts))
                    path.parent.mkdir(parents=True, exist_ok=True)
                    path.write_bytes(tar.extractfile(member).read())
    else:
        with zipfile.ZipFile(io.BytesIO(data)) as package:
            for info in package.infolist():
                if info.is_dir():
                    continue
                parts = pathlib.PurePosixPath(info.filename.replace("\\", "/")).parts[strip:]
                if parts:
                    path = inside(destination, pathlib.Path(*parts))
                    path.parent.mkdir(parents=True, exist_ok=True)
                    path.write_bytes(package.read(info))


def main():
    wanted = set(sys.argv[1:])
    failures = 0
    for name, url, expected, strip in DEPS:
        if wanted and name not in wanted:
            continue
        archive = download(name, url)
        digest = hashlib.sha256(archive.read_bytes()).hexdigest()
        if expected and digest != expected:
            print(f"{name}: SHA-256 MISMATCH {digest}")
            failures += 1
            continue
        extract(archive, THIRD_PARTY / name, strip)
        print(f"{name}: ok {digest}{'' if expected else '  (not pinned yet)'}")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
