"""Unpack libNOVA.so from a N.O.V.A. Legacy 5.8.4a APK.

The APK ships the game library packed by Gameloft's DecRawso: assets/datv8 (arm64) is a 7z archive with
its first 6 bytes replaced by "CORIAJ", then every byte XORed with 0x61. This undoes that and runs 7-Zip.

    python tools/extract_lib.py <game.apk> [out_dir]     (out_dir defaults to the current directory)

Needs 7-Zip (7z.exe on PATH or in Program Files).
"""
import os
import shutil
import subprocess
import sys
import tempfile
import zipfile

SEVEN_ZIP_MAGIC = bytes([0x37, 0x7A, 0xBC, 0xAF, 0x27, 0x1C])


def find_7z():
    for name in ("7z", "7z.exe", "7za"):
        path = shutil.which(name)
        if path:
            return path
    for base in (os.environ.get("ProgramFiles", ""), os.environ.get("ProgramFiles(x86)", "")):
        path = os.path.join(base, "7-Zip", "7z.exe")
        if base and os.path.exists(path):
            return path
    sys.exit("7-Zip not found: install it from https://www.7-zip.org/ or put 7z.exe on PATH")


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    apk, out_dir = sys.argv[1], (sys.argv[2] if len(sys.argv) > 2 else ".")
    with zipfile.ZipFile(apk) as z:
        packed = bytearray(z.read("assets/datv8"))
    data = bytearray(b ^ 0x61 for b in packed)
    if bytes(data[:6]) != b"CORIAJ":
        sys.exit("assets/datv8 does not decode to CORIAJ: not the expected APK (5.8.4a)")
    data[:6] = SEVEN_ZIP_MAGIC
    with tempfile.TemporaryDirectory() as tmp:
        archive = os.path.join(tmp, "datv8.7z")
        with open(archive, "wb") as f:
            f.write(data)
        subprocess.run([find_7z(), "x", "-y", "-o" + tmp, archive], check=True, stdout=subprocess.DEVNULL)
        found = False
        for root, _, files in os.walk(tmp):
            if "libNOVA.so" in files:
                os.makedirs(out_dir, exist_ok=True)
                shutil.copy2(os.path.join(root, "libNOVA.so"), os.path.join(out_dir, "libNOVA.so"))
                found = True
                break
    if not found:
        sys.exit("libNOVA.so not found in assets/datv8")
    print("wrote", os.path.join(out_dir, "libNOVA.so"))


if __name__ == "__main__":
    main()
