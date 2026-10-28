#!/usr/bin/env python3
"""Create a Debian arm64 package with Cardputer Zero APPLaunch integration."""
import os
import shutil
import subprocess
import tempfile
from pathlib import Path

from metadata import ARCHIVE_VERSION, PACKAGE, ROOT, VERSION

BINARY = ROOT / 'build-arm64/zero-browser'

def main():
    if not BINARY.is_file():
        raise SystemExit('Missing ARM64 binary; run make cross first')
    check = subprocess.run(['file', '-b', str(BINARY)], capture_output=True, text=True, check=True).stdout
    if 'ARM aarch64' not in check and 'ARM64' not in check:
        raise SystemExit('Refusing to package non-ARM64 binary: ' + check.strip())
    out = ROOT / 'dist'
    out.mkdir(exist_ok=True)
    dest = out / f'{PACKAGE}_{ARCHIVE_VERSION}_arm64.deb'
    with_qt = os.environ.get('WITH_QTWEBKIT', '1') != '0'
    if with_qt and not (ROOT / 'build-arm64/zero-qtwebkit').is_file():
        raise SystemExit('Missing QtWebKit helper; run make cross first')
    with tempfile.TemporaryDirectory(prefix='zero-browser-deb-') as td:
        stage = Path(td) / 'stage'
        stage.mkdir()
        def install(src, relative, mode=0o644):
            target = stage / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(src, target)
            target.chmod(mode)
        install(BINARY, 'usr/bin/zero-browser', 0o755)
        install(ROOT / 'build-arm64/libzero-netsurf-gtk.so', 'usr/lib/zero-browser/libzero-netsurf-gtk.so', 0o644)
        install(ROOT / 'scripts/zero-browserctl', 'usr/bin/zero-browserctl', 0o755)
        install(ROOT / 'LICENSE', 'usr/share/doc/zero-browser/copyright')
        install(ROOT / 'assets/UiFont.LICENSE', 'usr/share/doc/zero-browser/UiFont.LICENSE')
        for name in ('gtk-zoom-in', 'gtk-zoom-out', 'gtk-zoom-100', 'gtk-home', 'go-home'):
            install(ROOT / 'packaging/icons' / (name + '.png'),
                    'usr/share/zero-browser/icons/' + name + '.png')
        # APPLaunch scans its own applications directory, not the XDG menu.
        install(ROOT / 'packaging/applaunch/zero-browser.desktop',
                'usr/share/APPLaunch/applications/zero-browser.desktop')
        install(ROOT / 'packaging/assets/zero-browser.png',
                'usr/share/APPLaunch/share/images/zero-browser.png')
        control = stage / 'DEBIAN/control'
        control.parent.mkdir(parents=True)
        control.write_text(f'''Package: {PACKAGE}
Version: {VERSION}
Section: web
Priority: optional
Architecture: arm64
Maintainer: Zero-Browser Contributors
Depends: libc6, libstdc++6, libwayland-client0, libgtk-3-0t64, netsurf-gtk, labwc, dbus-daemon, wlr-randr, xdg-user-dirs
Description: Web browser for Cardputer Zero
 Browse websites, search the web and download files with your Cardputer's
 built-in keyboard. Designed for its small screen, with browsing history
 and zoom controls. Uses NetSurf for lightweight browsing without JavaScript.
''', encoding='utf-8')
        # The base uses declared system dependencies; the Qt bundle adds its
        # external runtime dependencies from the pinned Debian package metadata.
        control.chmod(0o644)
        for directory in [stage, *(path for path in stage.rglob('*') if path.is_dir())]:
            directory.chmod(0o755)
        base = Path(td) / 'base.deb'
        subprocess.run(['dpkg-deb', '--build', '--root-owner-group', str(stage), str(base)], check=True)
        if with_qt:
            archives = os.environ.get('ZERO_BROWSER_QT_DEBS', str(ROOT / '.cache/qtwebkit-debs'))
            subprocess.run(['python3', str(ROOT / 'packaging/build-qtwebkit-bundle.py'),
                            archives, '--base', str(base)], check=True)
        else:
            shutil.copyfile(base, dest)
    subprocess.run(['python3', str(ROOT / 'tests/check-package.py'), str(dest)], check=True)
    subprocess.run(['dpkg-deb', '--info', str(dest)], check=True)
    print('Created:', dest)
    print('NOTE: Test apt dependencies and store validator against official Cardputer Zero image.')

if __name__ == '__main__':
    main()
