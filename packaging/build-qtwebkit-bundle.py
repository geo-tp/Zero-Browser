#!/usr/bin/env python3
"""Merge a private Qt runtime into the complete Zero Browser Debian package.

Called by build-deb.py; no previously installed Zero Browser package is needed.
The input directory contains <package>.deb files matching the recorded manifest.
No system Qt libraries are installed or replaced by the resulting package.
"""
import argparse
import csv
import hashlib
import math
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import urllib.request

from metadata import ARCHIVE_VERSION, PACKAGE, ROOT, VERSION


def fields(text):
    return dict(line.split(': ', 1) for line in text.splitlines()
                if line and not line[0].isspace() and ': ' in line)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('debs', type=Path)
    parser.add_argument('--base', type=Path, required=True, help='Fresh supervisor package from this build')
    parser.add_argument('--manifest', type=Path, default=
                        ROOT / 'packaging/qtwebkit-runtime.tsv')
    args = parser.parse_args()
    records = list(csv.DictReader(args.manifest.open(), delimiter='\t'))
    args.debs.mkdir(parents=True, exist_ok=True)
    base_info = fields(subprocess.check_output(['dpkg-deb', '-f', str(args.base)], text=True))
    if base_info['Package'] != PACKAGE or base_info['Version'] != VERSION:
        raise SystemExit('Base package must come from this exact build')
    with tempfile.TemporaryDirectory(prefix='zero-qt-bundle-') as folder:
        folder = Path(folder)
        source, stage = folder / 'runtime-source', folder / 'stage'
        source.mkdir()
        subprocess.run(['dpkg-deb', '-x', str(args.base), str(stage)], check=True)
        metadata = {}
        for record in records:
            name = record['Package']
            if name.endswith('-dev'):
                continue
            archive = args.debs / (name + '.deb')
            if not archive.is_file():
                url = 'https://deb.debian.org/debian/' + record['Debian archive path']
                print('Downloading pinned runtime:', name, flush=True)
                with urllib.request.urlopen(url, timeout=60) as response:
                    data = response.read()
                if hashlib.sha256(data).hexdigest() != record['SHA256']:
                    raise SystemExit('Download checksum mismatch: ' + name)
                archive.write_bytes(data)
            if hashlib.sha256(archive.read_bytes()).hexdigest() != record['SHA256']:
                raise SystemExit('Package checksum mismatch: ' + str(archive))
            info = fields(subprocess.check_output(['dpkg-deb', '-f', str(archive)], text=True))
            if info['Package'] != name or info['Version'] != record['Version']:
                raise SystemExit('Unexpected package metadata: ' + name)
            if info['Architecture'] != 'arm64':
                raise SystemExit('Expected ARM64 package: ' + name)
            metadata[name] = info
            subprocess.run(['dpkg-deb', '-x', str(archive), str(source)], check=True)

        private = stage / 'usr/lib/zero-browser/qtwebkit-runtime'
        (private / 'lib').mkdir(parents=True)
        libraries = source / 'usr/lib/aarch64-linux-gnu'
        for library in libraries.glob('*.so*'):
            resolved = library.resolve()
            if not resolved.is_relative_to(source) or not resolved.is_file():
                raise SystemExit('Invalid runtime library link: ' + str(library))
            shutil.copy2(library, private / 'lib' / library.name, follow_symlinks=False)
        shutil.copytree(libraries / 'qt5/plugins', private / 'plugins')
        shutil.copy2(ROOT / 'build-arm64/zero-qtwebkit', private / 'zero-qtwebkit')
        (private / 'zero-qtwebkit').chmod(0o755)
        wrapper = stage / 'usr/lib/zero-browser/zero-qtwebkit'
        wrapper.write_text('''#!/bin/sh
root=/usr/lib/zero-browser/qtwebkit-runtime
export LD_LIBRARY_PATH="$root/lib"
export QT_PLUGIN_PATH="$root/plugins"
export QT_QPA_PLATFORM_PLUGIN_PATH="$root/plugins/platforms"
exec "$root/zero-qtwebkit" "$@"
''')
        wrapper.chmod(0o755)

        docs = stage / 'usr/share/doc' / PACKAGE / 'qtwebkit-runtime'
        docs.mkdir(parents=True)
        shutil.copy2(args.manifest, docs / 'runtime-packages.tsv')
        for name in metadata:
            copyright_file = source / 'usr/share/doc' / name / 'copyright'
            destination = docs / 'runtime' / name
            destination.mkdir(parents=True)
            shutil.copy2(copyright_file, destination / 'copyright')
        (docs / 'README').write_text('''Private Bookworm QtWebKit runtime for Zero Browser.
Libraries and Qt plugins are scoped to the helper process, not system-wide.
No development headers or QtWebEngine are included. Package/source versions
and archive SHA-256 hashes are recorded in runtime-packages.tsv; copyright
and source/license information are retained under runtime/<package>/copyright.
The application continues to use the device's existing Wayland compositor
bridge, system GStreamer plugins, fonts and non-Qt shared dependencies.
This runtime is included in the single zero-browser package. Installation
requires no older Zero Browser release, companion package or download script.
''')

        provided = set(metadata)
        for info in metadata.values():
            for item in info.get('Provides', '').split(', '):
                provided.add(item.split(' ')[0])
        # Keep external runtime dependencies and their upstream minimums.
        # Bundled Qt ABI virtual dependencies must not pull system Qt packages.
        dependencies = {}
        for info in metadata.values():
            for item in info.get('Depends', '').split(', '):
                if not item:
                    continue
                alternatives = item.split(' | ')
                if any(part.split(' ')[0] in provided for part in alternatives):
                    continue
                name = alternatives[0].split(' ')[0]
                previous = dependencies.get(name)
                if previous:
                    a = re.search(r'\(>= ([^)]+)\)', item)
                    b = re.search(r'\(>= ([^)]+)\)', previous)
                    if not a or (b and subprocess.run(['dpkg', '--compare-versions',
                            a[1], 'le', b[1]]).returncode == 0):
                        continue
                dependencies[name] = item
        depends = sorted(set(base_info['Depends'].split(', ')) | set(dependencies.values()))
        control = stage / 'DEBIAN/control'
        control.parent.mkdir()
        size = math.ceil(sum(p.stat().st_size for p in stage.rglob('*')
                             if p.is_file() and not p.is_symlink()) / 1024)
        control.write_text(f'''Package: {PACKAGE}
Version: {VERSION}
Architecture: arm64
Section: web
Priority: optional
Maintainer: Zero-Browser Contributors
Installed-Size: {size}
Provides: zero-browser-qtwebkit (= {VERSION})
Conflicts: zero-browser-qtwebkit, zero-browser-qtwebkit-bundled
Replaces: zero-browser-qtwebkit, zero-browser-qtwebkit-bundled
Depends: {', '.join(depends)}
Description: Web browser for Cardputer Zero
 Browse websites, search the web and download files with your Cardputer's
 built-in keyboard. Choose QtWebKit for pages that use JavaScript, or
 NetSurf for lightweight browsing on simpler websites.
 .
 Designed for the small screen, with browsing history, zoom controls,
 downloads and a JavaScript on/off switch. Works best with lightweight sites.
''')
        for directory in [stage, *(p for p in stage.rglob('*') if p.is_dir())]:
            directory.chmod(0o755)
        dest = ROOT / 'dist' / f'{PACKAGE}_{ARCHIVE_VERSION}_arm64.deb'
        dest.parent.mkdir(exist_ok=True)
        subprocess.run(['dpkg-deb', '-Zxz', '-z3', '--build', '--root-owner-group',
                        str(stage), str(dest)], check=True)
    subprocess.run(['python3', str(ROOT / 'tests/check-package.py'), str(dest)], check=True)
    print('Created:', dest)


if __name__ == '__main__':
    main()
