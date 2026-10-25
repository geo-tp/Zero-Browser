#!/usr/bin/env python3
"""Minimal Wayland wire peer, fake labwc/randr/app. Test-only; no GPU or disks.

Advertises real globals and emits real output, toplevel and screencopy events.
The client must allocate/pass its SHM/keymap fds. Captures are valid black frames.
"""
import array
import json
import os
from pathlib import Path
import select
import signal
import socket
import struct
import subprocess
import sys
import time

ROOT = Path(os.environ['ZERO_RECOVERY_TEST'])
RUNTIME = Path(os.environ['XDG_RUNTIME_DIR'])
ROLE = Path(sys.argv[0]).name
FAULT = os.environ.get('ZERO_TEST_FAULT', 'normal')

def record(kind, **fields):
    with (ROOT / 'events.jsonl').open('a') as out:
        out.write(json.dumps(dict(kind=kind, pid=os.getpid(), runtime=str(RUNTIME), **fields)) + '\n')

def sleep_forever():
    while True: time.sleep(1)

if ROLE == 'dbus-daemon':
    record('bus')
    sleep_forever()
if ROLE == 'wlr-randr':
    record('configure')
    fault = (RUNTIME / 'fault').read_text()
    if fault == 'config': sys.exit(44)
    if fault == 'config-hang': sleep_forever()
    if fault != 'wrong-mode':
        (RUNTIME / 'mode').write_text(sys.argv[sys.argv.index('--custom-mode') + 1])
    sys.exit(0)
if ROLE in ('netsurf-gtk', 'zero-qtwebkit', 'gtk-test'):
    record('engine', role=ROLE)
    fault = (RUNTIME / 'fault').read_text()
    if fault == 'engine': sys.exit(42)
    if fault != 'no-window': (RUNTIME / 'app').touch()
    if ROLE == 'zero-qtwebkit':
        print('ZERO_QT_EVENT ready 0 "" "ready"', flush=True)
    sleep_forever()

assert ROLE == 'labwc', ROLE
counter = ROOT / 'attempts'
attempt = int(counter.read_text()) + 1 if counter.exists() else 1
counter.write_text(str(attempt))
# Suffix -once fails only the first attempt; -twice fails attempts 1 and 2.
fault = FAULT
if FAULT.endswith('-once'): fault = FAULT[:-5] if attempt == 1 else 'normal'
if FAULT.endswith('-twice'): fault = FAULT[:-6] if attempt <= 2 else 'normal'
(RUNTIME / 'fault').write_text(fault)
record('compositor', attempt=attempt, fault=fault)
if fault == 'crash': sys.exit(12)
# A descendant in its own session must also be stopped, without broad pkill.
child = subprocess.Popen([sys.executable, '-c', 'import signal,time; signal.signal(signal.SIGTERM,signal.SIG_IGN); time.sleep(300)'], start_new_session=True)
record('descendant', child=child.pid)
if fault == 'socket': sleep_forever()
server = socket.socket(socket.AF_UNIX)
server.bind(str(RUNTIME / 'wayland-0'))
server.listen()
server.setblocking(False)
started = time.monotonic()

# Wayland strings/arrays include their lengths and 4-byte padding.
def u(*args): return struct.pack('<' + 'I' * len(args), *args)
def string(value):
    b = value.encode() + b'\0'
    return u(len(b)) + b + bytes((-len(b)) % 4)
def send(c, obj, op, body=b''):
    c.sendall(u(obj, (len(body) + 8) << 16 | op) + body)
def get_string(data, at):
    length, = struct.unpack_from('<I', data, at)
    return data[at+4:at+4+length-1].decode(), at + 4 + (length+3)//4*4

GLOBALS = [
    ('wl_shm', 1), ('wl_output', 2), ('wl_seat', 1),
    ('zwlr_screencopy_manager_v1', 1), ('zwp_virtual_keyboard_manager_v1', 1),
    ('zwlr_virtual_pointer_manager_v1', 1), ('zwlr_data_control_manager_v1', 1),
    ('zwlr_foreign_toplevel_manager_v1', 3)]
clients = {}
def output(c, obj, width, height):
    send(c, obj, 0, u(0, 0, 0, 0, 0) + string('test') + string('headless') + u(0))
    send(c, obj, 1, u(1, width, height, 60000))
    send(c, obj, 3, u(1))
    send(c, obj, 2)

def request(c, state, obj, op, data):
    kind = state['objects'].get(obj)
    vals = struct.unpack('<' + 'I'*(len(data)//4), data) if data else ()
    if kind == 'wl_display':
        if op == 0:
            send(c, vals[0], 0, u(1)); send(c, 1, 1, u(vals[0]))
        elif op == 1:
            state['objects'][vals[0]] = 'wl_registry'; state['registry'] = vals[0]
    elif kind == 'wl_registry':
        name, end = get_string(data, 4)
        version, new = struct.unpack_from('<II', data, end)
        state['objects'][new] = name
        if name == 'wl_output': output(c, new, *state['mode'])
        elif name == 'wl_shm': send(c, new, 0, u(1))
        elif name == 'wl_seat': send(c, new, 0, u(3))
    elif kind == 'wl_shm' and op == 0:
        state['objects'][vals[0]] = 'wl_shm_pool'
        os.close(state['fds'].pop(0)) # memfd starts zeroed: black is a valid capture
    elif kind == 'wl_shm_pool' and op == 0:
        state['objects'][vals[0]] = 'wl_buffer'
    elif kind == 'zwp_virtual_keyboard_manager_v1':
        state['objects'][vals[1]] = 'keyboard'
    elif kind == 'keyboard' and op == 0:
        os.close(state['fds'].pop(0))
    elif kind == 'zwlr_virtual_pointer_manager_v1':
        state['objects'][vals[-1]] = 'pointer'
    elif kind == 'zwlr_data_control_manager_v1' and op == 1:
        state['objects'][vals[0]] = 'data_device'
    elif kind == 'zwlr_screencopy_manager_v1' and op == 0:
        new = vals[0]; state['objects'][new] = 'capture'
        record('capture', window=state['window'], attempt=attempt)
        if fault == 'capture-hang': return
        if fault == 'disconnect': c.close(); return
        if fault == 'capture': send(c, new, 3); return
        width, height = state['mode']
        send(c, new, 0, u(1, width, height, width*4))
        send(c, new, 1, u(0))
    elif kind == 'capture' and op == 0:
        send(c, obj, 2, u(0, 0, 0))

while True:
    readable, _, _ = select.select([server, *clients], [], [], .005)
    for c in readable:
        if c is server:
            conn, _ = server.accept()
            clients[conn] = dict(objects={1:'wl_display'}, data=b'', fds=[], registry=0,
                                 advertised=set(), mode=(640,480), window=False)
            continue
        state = clients[c]
        try:
            data, anc, _, _ = c.recvmsg(65536, socket.CMSG_SPACE(256))
            if not data: raise ConnectionError()
            for level, kind, content in anc:
                if level == socket.SOL_SOCKET and kind == socket.SCM_RIGHTS:
                    state['fds'].extend(array.array('i', content))
            state['data'] += data
            if fault == 'handshake': continue
            while len(state['data']) >= 8:
                obj, packed = struct.unpack_from('<II', state['data'])
                length, op = packed >> 16, packed & 65535
                if len(state['data']) < length: break
                body, state['data'] = state['data'][8:length], state['data'][length:]
                request(c, state, obj, op, body)
        except (ConnectionError, OSError):
            c.close()
            for fd in state['fds']: os.close(fd)
            del clients[c]
    for c, state in list(clients.items()):
        try:
            if state['registry']:
                for i, (name, version) in enumerate(GLOBALS, 1):
                    if i in state['advertised']: continue
                    if fault == 'missing-output' and name == 'wl_output': continue
                    if fault == 'missing-clipboard' and name == 'zwlr_data_control_manager_v1': continue
                    if fault == 'delayed' and name in ('wl_output','zwlr_data_control_manager_v1') and time.monotonic()-started < .5: continue
                    send(c, state['registry'], 0, u(i) + string(name) + u(version))
                    state['advertised'].add(i)
            if (RUNTIME / 'mode').exists():
                mode = tuple(map(int, (RUNTIME / 'mode').read_text().split('x')))
                if mode != state['mode']:
                    state['mode'] = mode
                    for obj, name in list(state['objects'].items()):
                        if name == 'wl_output': output(c, obj, *mode)
            if not state['window'] and (RUNTIME / 'app').exists():
                manager = next((o for o,n in state['objects'].items() if n == 'zwlr_foreign_toplevel_manager_v1'), None)
                display = next((o for o,n in state['objects'].items() if n == 'wl_output'), None)
                if manager and display:
                    h = 0xff000000
                    send(c, manager, 0, u(h))
                    send(c, h, 0, string('Test window')); send(c, h, 1, string('test-app'))
                    send(c, h, 2, u(display)); send(c, h, 4, u(4, 2)); send(c, h, 5)
                    state['window'] = True
                    record('window', attempt=attempt)
        except (ConnectionError, OSError):
            c.close()
            for fd in state['fds']: os.close(fd)
            del clients[c]
