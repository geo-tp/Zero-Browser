#!/usr/bin/env python3
"""Simulate a fresh APT install, with all Zero Browser packages absent from dpkg status.

Run on the target image: python3 check-installable-device.py /path/zero-browser.deb
This does not uninstall anything or modify the system package database.
"""
import argparse
from pathlib import Path
import subprocess
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('package', type=Path)
args = parser.parse_args()
archive = args.package.resolve()
dependencies = subprocess.check_output(['dpkg-deb', '-f', str(archive), 'Depends'], text=True)
assert 'zero-browser' not in dependencies, 'An older app or companion must not be required'
records = Path('/var/lib/dpkg/status').read_text().split('\n\n')
with tempfile.TemporaryDirectory(prefix='zero-install-check-') as folder:
    status = Path(folder) / 'status'
    status.write_text('\n\n'.join(record for record in records if not record.startswith((
        'Package: zero-browser\n', 'Package: zero-browser-qtwebkit\n',
        'Package: zero-browser-qtwebkit-bundled\n'))))
    result = subprocess.run(['apt-get', '-o', 'Dir::State::status=' + str(status),
                             '-o', 'Debug::NoLocking=1', '--simulate', '--no-install-recommends',
                             'install', str(archive)], stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, text=True)
    print(result.stdout)
    archive.with_suffix('.fresh-install.txt').write_text(result.stdout)
    assert result.returncode == 0
    assert 'Inst zero-browser ' in result.stdout and 'Inst zero-browser-qtwebkit' not in result.stdout
print('PASS dependency resolution without an older Zero Browser or companion package')
