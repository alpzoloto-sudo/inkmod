#!/usr/bin/env python3
from __future__ import annotations

import configparser
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def fail(msg: str) -> None:
    print(f"[release-check] ERROR: {msg}", file=sys.stderr)
    raise SystemExit(1)


def main() -> None:
    cfg = configparser.ConfigParser(interpolation=None)
    cfg.read(ROOT / "platformio.ini", encoding="utf-8")
    if not cfg.has_section("inkmod"):
        fail("platformio.ini has no [inkmod] section")

    version = cfg.get("inkmod", "version", fallback="").strip()
    firmware_version = cfg.get("inkmod", "inkmod_version", fallback="").strip()
    semver = re.compile(r"^[0-9]+(?:\.[0-9]+){2,3}$")
    if not semver.fullmatch(firmware_version):
        fail(f"invalid inkmod_version: {firmware_version!r}")
    if version != firmware_version:
        fail(f"version mismatch: version={version!r}, inkmod_version={firmware_version!r}")

    changelog = (ROOT / "CHANGELOG.md").read_text(encoding="utf-8")
    expected_heading = f"## [v{firmware_version}]"
    if expected_heading not in changelog:
        fail(f"CHANGELOG.md has no {expected_heading} release heading")

    forbidden = [
        "managed_components",
        "CHANGES.diff",
        "README.txt",
        "device-monitor.out.log",
        "device-monitor.err.log",
        "build-tiny.out.log",
        "build-tiny.err.log",
    ]
    present = [name for name in forbidden if (ROOT / name).exists()]
    if present:
        fail("generated/local artifacts present: " + ", ".join(present))


    # Production releases must use the true release environment. The OTA client
    # in a release build searches for firmware-release*.bin assets.
    release_workflow = (ROOT / ".github/workflows/release.yml").read_text(encoding="utf-8")
    if "pio run -e release" not in release_workflow:
        fail("release workflow is not building the release environment")
    if "firmware-release-v" not in release_workflow:
        fail("release workflow does not publish firmware-release-v<version>.bin")

    reader_activity = (ROOT / "src/activities/reader/ReaderActivity.cpp").read_text(encoding="utf-8")
    if "const std::string readPath = EpubChapterSplitter::resolveReadPath" in reader_activity:
        fail("unstable runtime EPUB pre-splitter is enabled in ReaderActivity")

    release_section = cfg.get("env:release", "build_flags", fallback="") if cfg.has_section("env:release") else ""
    if "-DLOG_LEVEL=-1" not in release_section:
        fail("release environment must compile verbose LOG_* output out")

    # The abandoned experimental UI waveform was observed to cause ghosting.
    # Fail release validation if those debug labels accidentally reappear.
    risky_tokens = ("ui-fast refresh", "ui-session power-down")
    roots = [ROOT / "src", ROOT / "lib", ROOT / "freeink-sdk"]
    for base in roots:
        for path in base.rglob("*"):
            if not path.is_file() or path.suffix.lower() not in {".c", ".cc", ".cpp", ".h", ".hpp"}:
                continue
            try:
                text = path.read_text(encoding="utf-8", errors="ignore")
            except OSError:
                continue
            for token in risky_tokens:
                if token in text:
                    fail(f"experimental ghosting-prone UI refresh token {token!r} found in {path.relative_to(ROOT)}")

    print(f"[release-check] OK: inkMOD v{firmware_version}")


if __name__ == "__main__":
    main()
