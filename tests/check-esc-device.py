#!/usr/bin/env python3
"""Run on Cardputer: real NetSurf/labwc, isolated profile, no physical input/display.
Usage: python3 tests/check-esc-device.py /path/to/zero-browser
The companion libzero-netsurf-gtk.so must be beside the binary or installed.
"""
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import time

binary = str(Path(sys.argv[1] if len(sys.argv) > 1 else 'build-arm64/zero-browser').resolve())
root = Path(tempfile.mkdtemp(prefix='zero-esc-device-'))
print('Evidence:', root, flush=True)

def check_session(name, shortcut, title):
    folder = root / name
    for item in ('config', 'cache', 'state', 'run'):
        (folder / item).mkdir(parents=True, mode=0o700)
    env = dict(os.environ)
    for key, item in [('XDG_CONFIG_HOME', 'config'), ('XDG_CACHE_HOME', 'cache'),
                      ('XDG_STATE_HOME', 'state'), ('XDG_RUNTIME_DIR', 'run')]:
        env[key] = str(folder / item)
    with (folder / 'session.log').open('w') as log:
        app = subprocess.Popen([binary, '--mode', 'lite', '--no-input', '--frame-file',
                                str(folder / 'frame.ppm')], env=env, stdout=log, stderr=log)
        def ctl(command):
            with socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET) as client:
                client.settimeout(5)
                client.connect(str(folder / 'run/zero-browser/control'))
                client.sendall(command.encode())
                return client.recv(16384).decode().strip()
        def wait(predicate, timeout=25):
            until = time.monotonic() + timeout
            while time.monotonic() < until:
                assert app.poll() is None, 'Browser exited unexpectedly'
                try:
                    value = predicate()
                    if value: return value
                except (OSError, TimeoutError):
                    pass
                time.sleep(.1)
            raise AssertionError('Timed out in ' + name)
        try:
            wait(lambda: 'state=2 ' in ctl('status') and 'active=1' in ctl('windows'))
            time.sleep(.8)
            # Orphan release/repeat must not toggle the overlay.
            assert ctl('ui-key 1 0 0') == 'OK'
            assert ctl('ui-key 1 0 2') == 'OK'
            assert 'view=1 ' in ctl('status')
            assert ctl('ui-key 1') == 'OK'
            wait(lambda: 'view=3 ' in ctl('status'))
            time.sleep(.8)  # let the native location snapshot finish
            ctl('ui-key 1')
            wait(lambda: 'view=1 ' in ctl('status'))
            # Existing suppressed shortcuts and explicit overlay shortcut.
            before = ctl('windows')
            ctl('ui-key 64'); ctl('ui-key 67')  # F6/F9
            time.sleep(.2)
            assert ctl('windows') == before
            ctl('ui-key 88')  # F12
            wait(lambda: 'view=3 ' in ctl('status'))
            time.sleep(.8)
            ctl('ui-key 88')
            wait(lambda: 'view=1 ' in ctl('status'))
            ctl(shortcut)
            wait(lambda: any('active=1' in line and title in line for line in ctl('windows').splitlines()))
            ctl('ui-key 1')
            wait(lambda: title not in ctl('windows'))
            assert 'view=1 ' in ctl('status'), 'Secondary ESC also opened overlay'
            ctl(shortcut)
            wait(lambda: any('active=1' in line and title in line for line in ctl('windows').splitlines()))
            engine = int(ctl('status').split('pid=')[1].split()[0])
            started = time.monotonic()
            ctl('ui-key 1 0 1')  # press without release
            time.sleep(1)
            assert app.poll() is None, 'Long ESC exited before three seconds'
            app.wait(timeout=5)
            duration = time.monotonic() - started
            assert 2.9 <= duration < 6 and app.returncode == 0, (duration, app.returncode)
            assert not Path('/proc', str(engine)).exists(), 'Engine survived browser exit'
            print('PASS', name, 'short ESC; shortcuts; hold exit', round(duration, 2), 's', flush=True)
        finally:
            if app.poll() is None:
                try: ctl('quit')
                except OSError: app.terminate()
                app.wait(timeout=10)

check_session('downloads', 'tap 36 4', 'Downloads - NetSurf')
check_session('dialog', 'tap 24 4', 'Open File')
