#!/usr/bin/env python3
"""Compatibility entry point for the complete Wayland recovery suite."""
import argparse
from pathlib import Path
import subprocess
import sys

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('binary', type=Path)
parser.add_argument('--browser', type=Path)
parser.add_argument('--input-shim')
parser.add_argument('--bridge-driver')
args = parser.parse_args()
binary = args.browser or args.binary
raise SystemExit(subprocess.call([sys.executable, str(Path(__file__).with_name('check-recovery.py')), str(binary)]))
