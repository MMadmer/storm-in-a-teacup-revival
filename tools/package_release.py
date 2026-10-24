"""Packs the built mods into the release archive with its update manifest.

Usage: package_release.py [--output DIR]

The archive is stored without compression: the in-game updater reads changed files straight out of it with HTTP
range requests. The manifest is also written into the local installation, which makes it a valid update base.
"""
import argparse
import hashlib
import re
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
MANIFEST = "Mods/AutoUpdate/update-manifest.txt"
MODS_INI = "[Mods]\r\nExpandedSettings=1\r\nPerformanceOptimizer=1\r\nVideoCrashFix=1\r\nAutoUpdate=1\r\n"

# (archive path, user-editable). User-editable files are installed when missing and never overwritten.
FILES = [
    ("d3d9.dll", False),
    ("StormModLoader.dll", False),
    ("LICENSE", False),
    ("Mods/Mods.ini", True),
    ("Mods/ExpandedSettings/ExpandedSettings.dll", False),
    ("Mods/PerformanceOptimizer/PerformanceOptimizer.dll", False),
    ("Mods/PerformanceOptimizer/PerformanceOptimizer.ini", True),
    ("Mods/VideoCrashFix/VideoCrashFix.dll", False),
    ("Mods/AutoUpdate/AutoUpdate.dll", False),
    ("Mods/AutoUpdate/StormUpdater.exe", False),
]


def project_version():
    text = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
    return re.search(r"project\(\w+ VERSION ([\d.]+)", text).group(1)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path, default=ROOT / "build" / "release")
    args = parser.parse_args()

    version = project_version()
    contents = {}
    for path, _ in FILES:
        contents[path] = MODS_INI.encode() if path == "Mods/Mods.ini" else (ROOT / path).read_bytes()

    # A stale AutoUpdate.dll would keep offering the release it belongs to on every launch.
    if bytes(1) + version.encode() + bytes(1) not in contents["Mods/AutoUpdate/AutoUpdate.dll"]:
        raise SystemExit(f"AutoUpdate.dll is not built for version {version}; rebuild before packaging")

    lines = ["storm-revival-manifest 1", f"version {version}"]
    for path, config in FILES:
        data = contents[path]
        lines.append(f"file {hashlib.sha256(data).hexdigest()} {len(data)} {'config' if config else '-'} {path}")
    manifest = ("\r\n".join(lines) + "\r\n").encode()
    (ROOT / MANIFEST).write_bytes(manifest)

    args.output.mkdir(parents=True, exist_ok=True)
    archive = args.output / f"storm-in-a-teacup-revival-{version}.zip"
    with zipfile.ZipFile(archive, "w", zipfile.ZIP_STORED) as package:
        for path, _ in FILES:
            package.writestr(zipfile.ZipInfo(path, date_time=(2026, 1, 1, 0, 0, 0)), contents[path])
        package.writestr(zipfile.ZipInfo(MANIFEST, date_time=(2026, 1, 1, 0, 0, 0)), manifest)
    print(f"{archive} ({archive.stat().st_size} bytes, version {version})")


if __name__ == "__main__":
    main()
