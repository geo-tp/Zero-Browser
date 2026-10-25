"""Shared release metadata for the base package and private Qt runtime bundle."""
import os
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PACKAGE = "zero-browser"
UPSTREAM_VERSION = (ROOT / "VERSION").read_text(encoding="utf-8").strip()
# The public release starts at 0.1 after development packages numbered 0.1.4-*.
# An epoch preserves normal APT upgrades while keeping the public version 0.1.
DEBIAN_EPOCH = "1"
DEBIAN_REVISION = "2"
VERSION = os.environ.get("ZERO_BROWSER_VERSION", f"{DEBIAN_EPOCH}:{UPSTREAM_VERSION}-{DEBIAN_REVISION}")
# Debian epochs belong in package metadata, not archive filenames.
ARCHIVE_VERSION = VERSION.split(":", 1)[-1]
