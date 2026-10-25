#!/usr/bin/env python3
"""Prepare an isolated Debian Bookworm ARM64 QtWebKit *build* sysroot.

Uses apt with private state/sources (never installs packages on the host).
Requires apt-get, dpkg-deb and internet access. Cache defaults to
.cache/qtwebkit-bookworm-arm64, override with ZERO_BROWSER_QT_SYSROOT.
"""
import os
from pathlib import Path
import subprocess
import sys
import re
from urllib.request import urlopen, Request
from urllib.parse import unquote

ROOT = Path(__file__).resolve().parents[1]
TARGET = Path(os.environ.get('ZERO_BROWSER_QT_SYSROOT', ROOT / '.cache/qtwebkit-bookworm-arm64')).resolve()
PKGS = ['qtbase5-dev:arm64', 'libqt5webkit5-dev:arm64']

def run(*cmd):
    print('+', ' '.join(map(str, cmd)), flush=True)
    subprocess.run([str(x) for x in cmd], check=True)

def official_keyring(apt: Path) -> Path:
    """Use a host Debian keyring if present; otherwise extract the official .deb.

    Bootstrap download is HTTPS only. Subsequent APT package indexes remain
    authenticated with Debian repository GPG signatures.
    """
    requested = os.environ.get('DEBIAN_ARCHIVE_KEYRING')
    if requested:
        key = Path(requested).expanduser().resolve()
        if not key.is_file():
            raise SystemExit(f'DEBIAN_ARCHIVE_KEYRING does not exist: {key}')
        return key
    host = Path('/usr/share/keyrings/debian-archive-keyring.gpg')
    if host.is_file():
        return host
    extracted = apt / 'debian-keyring'
    cached = extracted / 'usr/share/keyrings/debian-archive-keyring.gpg'
    if cached.is_file():
        return cached

    base = 'https://deb.debian.org/debian/pool/main/d/debian-archive-keyring/'
    print('Host Debian keyring absent; downloading official Debian keyring over HTTPS...', flush=True)
    try:
        listing = urlopen(Request(base, headers={'User-Agent': 'ZeroBrowser-Build/1.0'}), timeout=30).read().decode('utf-8')
        filenames = {unquote(name) for name in re.findall(r'href=["\']([^"\']+\.deb)["\']', listing)}
        candidates = [n for n in filenames if re.fullmatch(r'debian-archive-keyring_[^/]+_all\.deb', n)]
        if not candidates:
            raise RuntimeError('No debian-archive-keyring _all.deb found on official Debian mirror')
        # All current Debian keyring packages include the Bookworm archive keys.
        # Debian filenames sort chronologically by the date-based package version.
        name = sorted(candidates)[-1]
        archive = apt / name
        if not archive.exists():
            with urlopen(base + name, timeout=90) as stream, archive.open('wb') as out:
                import shutil
                shutil.copyfileobj(stream, out)
        extracted.mkdir(parents=True, exist_ok=True)
        run('dpkg-deb', '-x', archive, extracted)
    except Exception as exc:
        raise SystemExit('Could not retrieve official Debian keyring via HTTPS: ' + str(exc) +
                         '\nProvide an official local keyring with DEBIAN_ARCHIVE_KEYRING=/path/to/keyring.gpg') from exc
    if not cached.is_file():
        raise SystemExit(f'Debian keyring package did not contain {cached}')
    return cached

def main():
    pc = TARGET / 'usr/lib/aarch64-linux-gnu/pkgconfig/Qt5WebKitWidgets.pc'
    if pc.is_file() and (TARGET / 'usr/include/aarch64-linux-gnu/qt5').is_dir():
        print('QtWebKit ARM64 SDK already prepared:', TARGET)
        return
    if os.geteuid() == 0:
        print('Note: run as normal user; root is not required.', file=sys.stderr)
    apt = TARGET / '.apt'
    for sub in ['etc/apt/sources.list.d', 'var/lib/apt/lists/partial',
                'var/cache/apt/archives/partial', 'var/lib/dpkg']:
        (apt / sub).mkdir(parents=True, exist_ok=True)
    # Debian keys are not normally installed on Ubuntu hosts.
    keyring = official_keyring(apt)
    source_opts = f'arch=arm64 signed-by={keyring}'
    (apt / 'etc/apt/sources.list').write_text(
        f'deb [{source_opts}] http://deb.debian.org/debian bookworm main\n'
        f'deb [{source_opts}] http://deb.debian.org/debian-security bookworm-security main\n'
        f'deb [{source_opts}] http://deb.debian.org/debian bookworm-updates main\n')
    status = apt / 'var/lib/dpkg/status'
    status.touch()
    common = ['apt-get', '-o', f'Dir={apt}',
              '-o', 'APT::Architecture=arm64', '-o', 'APT::Architectures=arm64',
              '-o', 'Debug::NoLocking=true', '-o', 'APT::Get::Assume-Yes=true',
              '-o', 'APT::Get::AllowUnauthenticated=false',
              '-o', f'Dir::State::status={status}',
              '-o', f'Dir::Etc::sourceparts={apt / "etc/apt/sources.list.d"}',
              '-o', f'Dir::Etc::sourcelist={apt / "etc/apt/sources.list"}']
    run(*common, 'update')
    run(*common, '-d', 'install', *PKGS)
    debs = sorted((apt / 'var/cache/apt/archives').glob('*.deb'))
    if not debs:
        raise SystemExit('apt did not download any ARM64 development packages')
    for archive in debs:
        arch = subprocess.check_output(['dpkg-deb', '-f', str(archive), 'Architecture'], text=True).strip()
        if arch not in ('arm64', 'all'):
            raise SystemExit(f'Unexpected architecture {arch}: {archive}')
        run('dpkg-deb', '-x', archive, TARGET)
    if not pc.exists():
        raise SystemExit(f'Missing {pc}; check Bookworm package availability')
    print('QtWebKit SDK ready:', TARGET)
    print('Now run: python3 scripts/build-arm64.py')

if __name__ == '__main__':
    main()
