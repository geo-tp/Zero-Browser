#!/usr/bin/env python3
"""Exercise the real control socket, including delayed packets and I/O failures."""
import argparse
from contextlib import contextmanager
import os
from pathlib import Path
import socket
import shutil
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]


@contextmanager
def session(binary, shim=None, fault=None):
    with tempfile.TemporaryDirectory(prefix="zero-control-") as folder:
        folder = Path(folder)
        environment = os.environ.copy()
        for name in ("CONFIG_HOME", "STATE_HOME", "CACHE_HOME", "RUNTIME_DIR"):
            environment["XDG_" + name] = str(folder / name.lower())
        if fault:
            # The ELF loader splits LD_PRELOAD at spaces, including in repo paths.
            local_shim = folder / "control-io.so"
            shutil.copyfile(shim, local_shim)
            environment["LD_PRELOAD"] = str(local_shim)
            environment["ZERO_TEST_CONTROL_IO"] = fault
        control = folder / "runtime_dir/zero-browser/control"
        frame = folder / "frame.ppm"
        with (folder / "application.log").open("w+") as log:
            app = subprocess.Popen([str(binary.resolve()), "--no-input", "--frame-file",
                                    str(frame), "--run-seconds", "30"], env=environment,
                                   stdout=log, stderr=log)
            try:
                until = time.monotonic() + 5
                while not control.exists() or not frame.exists():
                    assert app.poll() is None, "Application exited during startup"
                    assert time.monotonic() < until, "Startup timeout"
                    time.sleep(.01)
                yield app, control, frame, log
            except BaseException:
                log.seek(0)
                print(log.read(), flush=True)
                raise
            finally:
                if app.poll() is None:
                    app.terminate()
                try:
                    app.wait(timeout=6)
                except subprocess.TimeoutExpired:
                    app.kill()
                    app.wait()


def connect(path):
    client = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
    client.settimeout(4)
    deadline = time.monotonic() + 4
    try:
        while True:
            try:
                client.connect(str(path))
                return client
            except BlockingIOError:
                # A deliberately saturated Unix listen backlog returns EAGAIN.
                # Retry connection only; never retry a sent command or lost reply.
                if time.monotonic() >= deadline:
                    raise
                time.sleep(.01)
    except BaseException:
        client.close()
        raise


def reply(client):
    data, _, flags, _ = client.recvmsg(16384)
    assert not flags & socket.MSG_TRUNC, "Reply was truncated"
    assert data.endswith(b"\n"), repr(data)
    return data.decode().rstrip("\n")


def request(path, command):
    with connect(path) as client:
        client.sendall(command.encode())
        return reply(client)


def closed(client):
    try:
        assert client.recv(16384) == b"", "Unexpected reply on discarded client"
    except ConnectionResetError:
        pass


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", nargs="?", type=Path, default=ROOT / "build/zero-browser")
    parser.add_argument("--shim", type=Path, required=True)
    args = parser.parse_args()
    with session(args.binary) as (app, control, frame, _):
        with connect(control) as delayed:
            # Regression: accept() used to close this client before it sent a packet.
            delayed.settimeout(.15)
            try:
                data = delayed.recv(16384)
                raise AssertionError(f"Server replied before receiving a request: {data!r}")
            except socket.timeout:
                pass
            before = frame.stat().st_mtime_ns
            start = time.monotonic()
            assert request(control, "status").startswith("state=0 ")
            assert time.monotonic() - start < .5, "Idle client stalled other requests"
            time.sleep(.2)
            assert frame.stat().st_mtime_ns > before, "Idle client stalled rendering"
            delayed.settimeout(2)
            delayed.sendall(b"status")
            assert "screen=awake" in reply(delayed)
            closed(delayed)
        print("PASS delayed request; concurrent requests and rendering stay responsive", flush=True)

        with connect(control) as client:
            client.sendall(b"status")
            client.shutdown(socket.SHUT_WR)
            assert "screen=awake" in reply(client), "Half-close discarded a valid request"
        with connect(control) as client:
            client.send(b"")
            assert reply(client) == "ERROR empty request"
        assert request(control, "unknown-command") == "ERROR no browser"
        assert request(control, "ui-key invalid") == "ERROR key"
        time.sleep(.05)
        descriptors = len(list(Path(f"/proc/{app.pid}/fd").iterdir()))
        for index in range(40):
            with connect(control) as client:
                if index % 2:
                    client.sendall(b"status")
        time.sleep(.15)
        assert request(control, "status").startswith("state=0 ")
        time.sleep(.05)
        assert len(list(Path(f"/proc/{app.pid}/fd").iterdir())) == descriptors, "Disconnected clients leaked fds"
        print("PASS half-close, empty/invalid commands and disconnected-client cleanup", flush=True)

        with connect(control) as idle:
            start = time.monotonic()
            closed(idle)
            assert 1.8 <= time.monotonic() - start < 3.5, "Request timeout is not bounded"
        clients = []
        try:
            for _ in range(8):
                clients.append(connect(control))
                time.sleep(.025)
            start = time.monotonic()
            assert request(control, "status").startswith("state=0 ")
            assert time.monotonic() - start < 3, "Capacity did not recover after idle timeout"
            for client in clients:
                closed(client)
        finally:
            for client in clients:
                client.close()
        with connect(control) as idle:
            start = time.monotonic()
            assert request(control, "quit") == "OK"
            assert app.wait(timeout=1) == 0
            assert time.monotonic() - start < 1, "Quit waited for an idle request"
            closed(idle)
        print("PASS request timeout, bounded capacity and quit cleanup", flush=True)

    for command in ("status", "quit"):
        with session(args.binary, args.shim, "retry") as (app, control, _, log):
            result = request(control, command)
            assert result.startswith("state=0 ") if command == "status" else result == "OK"
            if command == "quit":
                assert app.wait(timeout=2) == 0
            log.seek(0)
            evidence = log.read()
            for name in ("recv EINTR", "send EAGAIN", "send EINTR"):
                assert "injected " + name in evidence, name
        print("PASS stored reply retries after EINTR/EAGAIN:", command, flush=True)

    for fault in ("recv-error", "send-error", "blocked-send"):
        with session(args.binary, args.shim, fault) as (app, control, frame, _):
            with connect(control) as client:
                start = time.monotonic()
                client.sendall(b"status")
                time.sleep(.05)
                before = frame.stat().st_mtime_ns
                assert request(control, "status").startswith("state=0 ")
                if fault == "blocked-send":
                    time.sleep(.2)
                    assert frame.stat().st_mtime_ns > before, "Blocked send stalled rendering"
                closed(client)
                elapsed = time.monotonic() - start
                assert elapsed < 3.5
                if fault == "blocked-send":
                    assert elapsed >= 1.8, "Reply discarded before its send deadline"
            assert request(control, "quit") == "OK"
            assert app.wait(timeout=2) == 0
        print("PASS socket error/send-timeout cleanup:", fault, flush=True)

    with session(args.binary, args.shim, "blocked-send") as (app, control, _, _):
        with connect(control) as client:
            start = time.monotonic()
            client.sendall(b"quit")
            closed(client)
            assert app.wait(timeout=2) == 0
            assert 1.8 <= time.monotonic() - start < 3.5
    print("PASS undeliverable quit reply has a bounded shutdown deadline", flush=True)


if __name__ == "__main__":
    main()
