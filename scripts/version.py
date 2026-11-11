#!/usr/bin/env python3
"""Single-source version management for RockBLOCK-960x.

The version macros in src/rockblock960x.h are the source of truth.
CMake parses the header at configure time; the files below can't (they
are read as literal text by Arduino/PlatformIO/pip), so this script
keeps them in sync.

Usage:
    python3 scripts/version.py               # verify all files agree (CI-friendly)
    python3 scripts/version.py set 0.2.0     # bump everywhere
"""

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
HEADER = ROOT / "src" / "rockblock960x.h"

# path -> list of (regex with one capture group around the version, count)
SYNCED = {
    ROOT / "library.properties": [(r"(?m)^version=(.+)$", 1)],
    ROOT / "library.json": [(r'"version":\s*"([^"]+)"', 1)],
    ROOT / "python" / "pyproject.toml": [(r'(?m)^version = "([^"]+)"', 1)],
    ROOT / "python" / "rockblock960x" / "__init__.py": [
        (r'__version__ = "([^"]+)"', 1)
    ],
}

HEADER_PATTERNS = {
    "RB960X_VERSION_MAJOR": r"(?m)^#define RB960X_VERSION_MAJOR (\d+)$",
    "RB960X_VERSION_MINOR": r"(?m)^#define RB960X_VERSION_MINOR (\d+)$",
    "RB960X_VERSION_PATCH": r"(?m)^#define RB960X_VERSION_PATCH (\d+)$",
    "RB960X_VERSION_STRING": r'(?m)^#define RB960X_VERSION_STRING "([^"]+)"$',
}


def header_version() -> str:
    text = HEADER.read_text()
    found = {}
    for name, pattern in HEADER_PATTERNS.items():
        m = re.search(pattern, text)
        if not m:
            sys.exit(f"error: {name} not found in {HEADER}")
        found[name] = m.group(1)
    joined = ".".join(
        found[k]
        for k in ("RB960X_VERSION_MAJOR", "RB960X_VERSION_MINOR", "RB960X_VERSION_PATCH")
    )
    if joined != found["RB960X_VERSION_STRING"]:
        sys.exit(
            f"error: header inconsistent: macros say {joined}, "
            f'string says {found["RB960X_VERSION_STRING"]}'
        )
    return joined


def check() -> None:
    version = header_version()
    ok = True
    for path, patterns in SYNCED.items():
        text = path.read_text()
        for pattern, _count in patterns:
            m = re.search(pattern, text)
            got = m.group(1) if m else "<missing>"
            if got != version:
                print(f"OUT OF SYNC: {path.relative_to(ROOT)} has {got}")
                ok = False
    if not ok:
        sys.exit(f"header says {version} — run: scripts/version.py set {version}")
    print(f"all files agree: {version}")


def set_version(version: str) -> None:
    if not re.fullmatch(r"\d+\.\d+\.\d+", version):
        sys.exit(f"error: '{version}' is not X.Y.Z")
    major, minor, patch = version.split(".")

    text = HEADER.read_text()
    for name, value in (
        ("RB960X_VERSION_MAJOR", major),
        ("RB960X_VERSION_MINOR", minor),
        ("RB960X_VERSION_PATCH", patch),
    ):
        text = re.sub(rf"(?m)^#define {name} \d+$", f"#define {name} {value}", text)
    text = re.sub(
        r'(?m)^#define RB960X_VERSION_STRING "[^"]+"$',
        f'#define RB960X_VERSION_STRING "{version}"',
        text,
    )
    HEADER.write_text(text)

    for path, patterns in SYNCED.items():
        text = path.read_text()
        for pattern, count in patterns:
            text = re.sub(
                pattern,
                lambda m: m.group(0).replace(m.group(1), version),
                text,
                count=count,
            )
        path.write_text(text)
        print(f"updated {path.relative_to(ROOT)}")
    print(f"version is now {version}")


if __name__ == "__main__":
    if len(sys.argv) == 1:
        check()
    elif len(sys.argv) == 3 and sys.argv[1] == "set":
        set_version(sys.argv[2])
    else:
        sys.exit(__doc__)
