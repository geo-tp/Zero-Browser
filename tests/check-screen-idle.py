#!/usr/bin/env python3
"""Exercise the real 30s/60s idle timer and wake through the control socket."""
import argparse
import os
from pathlib import Path
import socket
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('binary', nargs='?', type=Path, default=ROOT / 'build/zero-browser')
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='zero-screen-') as temporary:
        directory = Path(temporary)
        environment = os.environ.copy()
        for name in ('CONFIG_HOME', 'STATE_HOME', 'CACHE_HOME', 'RUNTIME_DIR'):
            environment['XDG_' + name] = str(directory / name.lower())
        control = Path(environment['XDG_RUNTIME_DIR']) / 'zero-browser/control'
        frame = directory / 'frame.ppm'
        with (directory / 'application.log').open('w+') as log:
            process = subprocess.Popen(
                [str(args.binary.resolve()), '--no-input', '--frame-file', str(frame),
                 '--run-seconds', '70'], env=environment, stdout=log, stderr=log)
            try:
                def request(command):
                    with socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET) as client:
                        client.settimeout(2)
                        client.connect(str(control))
                        client.sendall(command.encode())
                        return client.recv(16384).decode().strip()

                def pixels():
                    data = frame.read_bytes().split(b'\n', 3)
                    assert data[:3] == [b'P6', b'320 170', b'255']
                    assert len(data[3]) == 320 * 170 * 3
                    # Exclude the header's live battery/Wi-Fi indicators.
                    return data[3][320 * 27 * 3:]

                deadline = time.monotonic() + 5
                while not control.exists() or not frame.exists():
                    assert process.poll() is None, 'Application stopped during startup'
                    assert time.monotonic() < deadline, 'Startup timed out'
                    time.sleep(.02)
                begun = time.monotonic()
                assert 'screen=awake' in request('status')
                original = pixels()
                assert any(original), 'Initial frame is blank'
                for state, expected_time in [('dimmed', 30), ('off', 60)]:
                    while True:
                        status = request('status')
                        elapsed = time.monotonic() - begun
                        if 'screen=' + state in status:
                            break
                        assert elapsed < expected_time + 2, f'Never reached {state}: {status}'
                        time.sleep(.1)
                    assert expected_time - .5 <= elapsed <= expected_time + 2, (state, elapsed)
                    expected = bytes(channel // 5 for channel in original) if state == 'dimmed' else bytes(len(original))
                    assert pixels() == expected, f'Wrong frame output when {state}'
                    print(f'{state} at {elapsed:.2f}s; frame verified', flush=True)

                modified = frame.stat().st_mtime_ns
                time.sleep(.25)
                assert 'screen=off' in request('status'), 'Status unexpectedly woke the screen'
                assert frame.stat().st_mtime_ns == modified, 'Still drawing while screen is off'
                wake_started = time.monotonic()
                assert request('ui-key 464') == 'OK'  # KEY_FN: wake even though the UI ignores it.
                assert 'screen=awake' in request('status')
                assert pixels() == original, 'Wake did not restore the original image'
                assert time.monotonic() - wake_started < 1, 'Wake was delayed'
                print('Fn wakes immediately; full image restored; idle polling did not postpone sleep', flush=True)
                assert request('quit') == 'OK'
                assert process.wait(timeout=6) == 0
            except BaseException:
                log.flush()
                log.seek(0)
                print(log.read(), flush=True)
                raise
            finally:
                if process.poll() is None:
                    process.terminate()
                    try:
                        process.wait(timeout=6)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait()


if __name__ == '__main__':
    main()
