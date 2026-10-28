#!/usr/bin/env python3
"""Verify the built package and the files resolved by Cardputer APPLaunch."""
import argparse
import configparser
import io
from pathlib import Path
import struct
import subprocess
import tarfile


def check_package(package):
    name = subprocess.check_output(['dpkg-deb', '-f', str(package), 'Package'], text=True).strip()
    assert name == 'zero-browser', 'Expected the single application package'
    dependencies = subprocess.check_output(['dpkg-deb', '-f', str(package), 'Depends'], text=True).strip()
    assert '-dev' not in dependencies and 'webengine' not in dependencies and 'chromium' not in dependencies
    assert 'cog' not in [item.split(' ')[0].strip() for item in dependencies.split(',')], 'Cog must remain optional'
    data = subprocess.check_output(['dpkg-deb', '--fsys-tarfile', str(package)])
    with tarfile.open(fileobj=io.BytesIO(data)) as archive:
        members = {entry.name.removeprefix('./').rstrip('/'): entry
                   for entry in archive.getmembers()}
        for path in members:
            assert not any(path == root or path.startswith(root + '/')
                           for root in ('usr/share/applications', 'usr/share/icons')), \
                f'Unsupported desktop integration path: {path}'
        if 'usr/lib/zero-browser/qtwebkit-runtime/zero-qtwebkit' in members:
            root = 'usr/lib/zero-browser/'
            wrapper = archive.extractfile(members[root + 'zero-qtwebkit']).read()
            assert wrapper.startswith(b'#!/bin/sh\n') and b'/tmp/' not in wrapper
            assert b'LD_LIBRARY_PATH="$root/lib"' in wrapper
            binary = members[root + 'qtwebkit-runtime/zero-qtwebkit']
            elf = archive.extractfile(binary).read()
            assert elf[:6] == b'\x7fELF\x02\x01' and struct.unpack_from('<H', elf, 18)[0] == 183
            assert b'libQt5WebKitWidgets.so.5' in elf and b'libQt5WebEngine' not in elf
            assert binary.mode == 0o755 and binary.uid == binary.gid == 0
            for required in ('lib/libQt5WebKit.so.5', 'lib/libQt5WebKitWidgets.so.5',
                             'lib/libQt5Core.so.5', 'plugins/platforms/libqwayland-generic.so'):
                assert root + 'qtwebkit-runtime/' + required in members, required
            docs = 'usr/share/doc/zero-browser/qtwebkit-runtime'
            assert docs + '/runtime/libqt5webkit5/copyright' in members
            assert not any(p.startswith('usr/include/') or p.endswith('.pc') for p in members)
            assert not any(p.startswith('usr/lib/aarch64-linux-gnu/') for p in members)
            print('Isolated QtWebKit package checks OK:', package)
            assert 'zero-browser' not in dependencies and 'libqt5webkit5' not in dependencies
            conflicts = subprocess.check_output(['dpkg-deb', '-f', str(package), 'Conflicts'], text=True)
            assert 'zero-browser-qtwebkit-bundled' in conflicts, 'Old companion must be replaced on upgrade'
        app = 'usr/share/APPLaunch'
        for required in ('usr/bin/zero-browser', 'usr/bin/zero-browserctl',
                         'usr/lib/zero-browser/libzero-netsurf-gtk.so',
                         'usr/share/doc/zero-browser/copyright',
                         'usr/share/doc/zero-browser/UiFont.LICENSE',
                         f'{app}/applications/zero-browser.desktop',
                         f'{app}/share/images/zero-browser.png'):
            assert required in members and members[required].isfile(), required
        license_text = archive.extractfile(members['usr/share/doc/zero-browser/copyright']).read()
        assert license_text == (Path(__file__).resolve().parents[1] / 'LICENSE').read_bytes(), \
            'Package must include the project license'
        module = members['usr/lib/zero-browser/libzero-netsurf-gtk.so']
        elf = archive.extractfile(module).read(20)
        assert elf[:6] == b'\x7fELF\x02\x01' and struct.unpack_from('<H', elf, 18)[0] == 183
        assert module.mode == 0o644
        for entry in members.values():
            assert entry.uid == 0 and entry.gid == 0, entry.name
            if entry.isdir():
                assert entry.mode == 0o755, entry.name

        desktop = configparser.ConfigParser(interpolation=None)
        desktop.read_string(archive.extractfile(
            members[f'{app}/applications/zero-browser.desktop']).read().decode('utf-8'))
        entry = desktop['Desktop Entry']
        assert entry['Type'] == 'Application'
        assert entry.getboolean('Terminal') is False
        assert not entry.getboolean('Hidden', fallback=False)
        assert not entry.getboolean('NoDisplay', fallback=False)
        assert entry['Name'].strip()
        for key in ('Exec', 'Icon'):
            value = entry[key]
            assert value.startswith('/'), f'{key} must use an absolute path'
            target = members.get(value.lstrip('/'))
            assert target is not None and target.isfile(), f'{key} target missing: {value}'
            if key == 'Exec':
                assert target.mode == 0o755, 'Browser must be executable'
                elf = archive.extractfile(target).read(20)
                assert elf[:6] == b'\x7fELF\x02\x01' and struct.unpack_from('<H', elf, 18)[0] == 183, \
                    'Browser must be little-endian ELF64 AArch64'
            else:
                assert target.mode == 0o644, 'Icon must be readable'
                png = archive.extractfile(target).read(24)
                assert png[:8] == b'\x89PNG\r\n\x1a\n', 'APPLaunch needs a PNG icon'
                assert struct.unpack_from('>II', png, 16) == (256, 256), 'Icon must be 256x256'
        assert members[entry['Path'].lstrip('/')].isdir(), 'Working directory missing'
    print('Package checks OK: ARM64 binary, APPLaunch entry, PNG icon, paths and permissions:', package)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('package', nargs='?', type=Path, help='Package to verify (default: latest build)')
    args = parser.parse_args()
    if args.package is None:
        root = Path(__file__).resolve().parents[1]
        packages = list((root / 'dist').glob('zero-browser_*_arm64.deb'))
        if not packages:
            raise SystemExit('No .deb yet: run make deb to package the ARM64 build')
        args.package = max(packages, key=lambda path: path.stat().st_mtime_ns)
    check_package(args.package)


if __name__ == '__main__':
    main()
