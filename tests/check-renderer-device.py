#!/usr/bin/env python3
"""Compare private labwc renderers on-device with isolated profiles and SHM capture.

No package installation, physical input, system configuration or CMA changes.
Artifacts are retained; CPU percentages use one core = 100%, RAM is in KiB.
"""
import argparse
import http.server
import json
import os
from pathlib import Path
import re
import shlex
import socket
import statistics
import subprocess
import tempfile
import threading
import time


def memory():
    values = {}
    for line in Path('/proc/meminfo').read_text().splitlines():
        key, value = line.split(':', 1)
        if key in ('MemAvailable', 'CmaTotal', 'CmaFree', 'SwapFree'):
            values[key] = int(value.split()[0])
    return values


def process_table():
    table = {}
    for path in Path('/proc').glob('[0-9]*/stat'):
        try:
            text = path.read_text()
            fields = text[text.rfind(')') + 2:].split()
            table[int(path.parent.name)] = dict(
                name=text[text.find('(') + 1:text.rfind(')')], state=fields[0],
                parent=int(fields[1]), start=int(fields[19]),
                ticks=int(fields[11]) + int(fields[12]),
                rss_kib=int(fields[21]) * os.sysconf('SC_PAGE_SIZE') // 1024)
        except (OSError, ValueError, IndexError):
            pass  # Process exited during the sample.
    return table


def sample(root_pid, owned):
    table = process_table()
    if root_pid in table and root_pid not in owned:
        owned[root_pid] = table[root_pid]['start']
    changed = True
    while changed:
        changed = False
        for pid, info in table.items():
            parent = info['parent']
            if pid not in owned and parent in owned and parent in table and \
                    table[parent]['start'] == owned[parent]:
                owned[pid] = info['start']
                changed = True
    processes = {}
    for pid, start in owned.items():
        if pid not in table or table[pid]['start'] != start:
            continue
        info = table[pid]
        try:
            rollup = Path(f'/proc/{pid}/smaps_rollup').read_text()
            match = re.search(r'^Pss:\s+(\d+)', rollup, re.M)
            info['pss_kib'] = int(match[1]) if match else None
            match = re.search(r'^SwapPss:\s+(\d+)', rollup, re.M)
            info['swap_pss_kib'] = int(match[1]) if match else None
        except OSError:
            info['pss_kib'] = 0 if info['state'] in ('Z', 'X') else None
            info['swap_pss_kib'] = info['pss_kib']
        processes[str(pid)] = info
    return dict(time=time.monotonic(), memory=memory(), processes=processes)


def control(folder, command):
    with socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET) as client:
        client.settimeout(1)
        client.connect(str(folder / 'run/zero-browser/control'))
        client.sendall(command.encode())
        return client.recv(32768).decode().strip()


def status(folder):
    return dict(item.split('=', 1) for item in shlex.split(control(folder, 'status')))


def summarize(samples):
    cpu, compositor_cpu, capture_rates = [], [], []
    for before, after in zip(samples, samples[1:]):
        elapsed = after['time'] - before['time']
        ticks = compositor_ticks = 0
        for pid, info in after['processes'].items():
            previous = before['processes'].get(pid)
            if previous and previous['start'] == info['start']:
                delta = info['ticks'] - previous['ticks']
                ticks += delta
                if info['name'] == 'labwc': compositor_ticks += delta
        factor = 100 / os.sysconf('SC_CLK_TCK') / elapsed
        cpu.append(ticks * factor)
        compositor_cpu.append(compositor_ticks * factor)
        if 'status' in before and 'status' in after:
            capture_rates.append((int(after['status']['frames']) - int(before['status']['frames'])) / elapsed)
    pss = [sum(p['pss_kib'] for p in s['processes'].values()) for s in samples
           if s['processes'] and all(p['pss_kib'] is not None for p in s['processes'].values())]
    labwc_pss = [p['pss_kib'] for s in samples for p in s['processes'].values()
                if p['name'] == 'labwc' and p['pss_kib'] is not None]
    swap_pss = [sum(p['swap_pss_kib'] for p in s['processes'].values()) for s in samples
                if s['processes'] and all(p['swap_pss_kib'] is not None for p in s['processes'].values())]
    return dict(cpu_percent_mean=statistics.mean(cpu) if cpu else None,
                labwc_cpu_percent_mean=statistics.mean(compositor_cpu) if compositor_cpu else None,
                capture_fps_mean=statistics.mean(capture_rates) if capture_rates else None,
                tree_pss_kib_max=max(pss, default=None),
                tree_swap_pss_kib_max=max(swap_pss, default=None),
                tree_rss_kib_max=max((sum(p['rss_kib'] for p in s['processes'].values())
                                      for s in samples), default=None),
                labwc_pss_kib_max=max(labwc_pss, default=None),
                cma_free_kib_min=min((s['memory'].get('CmaFree', 0) for s in samples), default=None))


def run(args, root, iteration, renderer, mode, url):
    folder = root / f'{iteration}-{renderer}-{mode}'
    for name in ('config', 'cache', 'state', 'run'):
        (folder / name).mkdir(parents=True, mode=0o700)
    (folder / 'config/zero-browser').mkdir()
    (folder / 'config/zero-browser/settings').write_text(f'fps={args.fps}\n')
    env = dict(os.environ)
    env.pop('WLR_RENDERER', None)
    if renderer != 'default': env['WLR_RENDERER'] = renderer
    for key, name in (('XDG_CONFIG_HOME', 'config'), ('XDG_CACHE_HOME', 'cache'),
                      ('XDG_STATE_HOME', 'state'), ('XDG_RUNTIME_DIR', 'run')):
        env[key] = str(folder / name)
    # Do not inherit compositor diagnostics or DRM overrides from the caller.
    env.pop('WLR_RENDER_DRM_DEVICE', None)
    argv = [str(args.binary), '--mode', mode, '--url', url, '--no-input']
    if not args.display:
        argv += ['--frame-file', str(folder / 'frame.ppm')]
    result = dict(renderer=renderer, mode=mode, before=memory(), phases={})
    owned, startup_samples = {}, []
    logfile = folder / 'session.log'
    with logfile.open('w') as output:
        app = subprocess.Popen(argv, env=env, stdout=output, stderr=output)
        try:
            deadline = time.monotonic() + 150
            while time.monotonic() < deadline:
                startup_samples.append(sample(app.pid, owned))
                logs = logfile.read_text()
                if 'application.start_failed' in logs or app.poll() is not None:
                    raise RuntimeError('Startup failed; see session.log')
                if 'startup.ready' in logs:
                    break
                time.sleep(.5)
            else:
                raise RuntimeError('Startup timeout')
            begin = re.search(r'^(\d+\.\d+) application.start ', logs, re.M)
            ready = re.search(r'^(\d+\.\d+) startup.ready ', logs, re.M)
            result['startup_seconds'] = float(ready[1]) - float(begin[1])
            # Wait for the page load, separate from compositor/first-frame readiness.
            deadline = time.monotonic() + 30
            while True:
                current = status(folder)
                if current.get('loading') == '0': break
                if time.monotonic() >= deadline: raise RuntimeError('Page load timeout')
                time.sleep(.5)
            result['windows_before'] = control(folder, 'windows')
            result['drm_fds'] = {}
            for pid, info in sample(app.pid, owned)['processes'].items():
                if info['name'] not in ('labwc', 'netsurf-gtk', 'zero-qtwebkit'): continue
                nodes = []
                for fd in Path(f'/proc/{pid}/fd').glob('*'):
                    try:
                        target = str(fd.readlink())
                        if target.startswith('/dev/dri/'): nodes.append(target)
                    except OSError:
                        pass
                result['drm_fds'][info['name']] = nodes
            for phase in ('idle', 'scroll'):
                samples = []
                deadline = time.monotonic() + args.seconds
                step = 0
                while time.monotonic() < deadline:
                    if phase == 'scroll':
                        control(folder, 'tap ' + ('109' if step % 8 < 4 else '104'))
                    current = sample(app.pid, owned)
                    current['status'] = status(folder)
                    if current['status'].get('state') != '2' or current['status'].get('capture_error'):
                        raise RuntimeError('Engine/capture failed: ' + str(current['status']))
                    samples.append(current)
                    step += 1
                    time.sleep(.5)
                screenshot = Path(control(folder, 'screenshot'))
                (folder / f'{phase}.ppm').write_bytes(screenshot.read_bytes())
                result['phases'][phase] = summarize(samples)
                (folder / f'{phase}.json').write_text(json.dumps(samples, indent=2))
                if int(samples[-1]['status']['frames']) <= int(samples[0]['status']['frames']):
                    raise RuntimeError('SHM capture stopped progressing')
            result['windows_after'] = control(folder, 'windows')
            if args.animated and mode == 'qtwebkit':
                before = re.search(r'Renderer frame (\d+)', result['windows_before'])
                after = re.search(r'Renderer frame (\d+)', result['windows_after'])
                if not before or not after or int(after[1]) <= int(before[1]):
                    raise RuntimeError('JavaScript animation did not progress')
            result['ok'] = True
        except Exception as error:
            result.update(ok=False, error=str(error))
        finally:
            quitting = time.monotonic()
            if app.poll() is None:
                try: control(folder, 'quit')
                except OSError: app.terminate()
            try: app.wait(timeout=10)
            except subprocess.TimeoutExpired:
                app.terminate()
                app.wait(timeout=10)
            result['exit_code'] = app.returncode
            result['shutdown_seconds'] = time.monotonic() - quitting
            result['after'] = []
            for _ in range(6):
                result['after'].append(sample(app.pid, owned))
                time.sleep(.5)
            result['survivors'] = result['after'][-1]['processes']
            result['runtime_leftovers'] = [str(p) for p in (folder / 'run/zero-browser').glob('session.*')]
            result['ok'] = result.get('ok', False) and app.returncode == 0 and \
                not result['survivors'] and not result['runtime_leftovers']
    logs = logfile.read_text()
    result['output_modes'] = re.findall(r'wayland.output_mode ([^\n]+)', logs)
    result['selected_renderers'] = re.findall(r'wayland.renderer ([^\n]+)', logs)
    result['attempts'] = logs.count('wayland.renderer ')
    result['startup'] = summarize(startup_samples)
    (folder / 'startup.json').write_text(json.dumps(startup_samples, indent=2))
    (folder / 'result.json').write_text(json.dumps(result, indent=2))
    print(folder.name, json.dumps({k: v for k, v in result.items() if k != 'after'}), flush=True)
    if result['survivors']:
        raise RuntimeError('Owned processes survived shutdown; stop the experiment and inspect ' + str(folder))
    return result


class Page(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        animation = '''<canvas id="canvas" width="280" height="40"></canvas><script>
var n=0, c=document.getElementById('canvas').getContext('2d');
setInterval(function(){n++;document.title='Renderer frame '+n;
c.fillStyle='#fff';c.fillRect(0,0,280,40);c.fillStyle='#298';
c.fillRect((n*4)%240,0,40,40);},100);</script>''' if self.path == '/animated' else ''
        body = ('<!doctype html><title>Renderer test</title><style>body{background:#fff;color:#123;'
                'font:14px sans-serif}p{padding:8px;border-bottom:2px solid #279}</style>'
                '<h1>Zero Browser</h1>' + animation + ''.join(f'<p>Row {i}: text and scrolling 0123456789</p>'
                                               for i in range(120))).encode()
        self.send_response(200)
        self.send_header('Content-Type', 'text/html; charset=utf-8')
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *args):
        pass


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('binary', type=Path)
    parser.add_argument('--iterations', type=int, default=5)
    parser.add_argument('--seconds', type=float, default=10, help='Seconds per idle/scroll phase')
    parser.add_argument('--fps', type=int, choices=range(2, 31), default=10, help='Capture/display target (default: 10 for the original renderer comparison)')
    parser.add_argument('--renderers', nargs='+', choices=('gles2', 'pixman', 'default'), default=['gles2', 'pixman'])
    parser.add_argument('--modes', nargs='+', choices=('lite', 'qtwebkit'), default=['lite', 'qtwebkit'])
    workload = parser.add_mutually_exclusive_group()
    workload.add_argument('--url', help='Optional workload; default: deterministic local static page')
    workload.add_argument('--animated', action='store_true', help='Exercise JavaScript and Canvas in the local fixture')
    parser.add_argument('--display', action='store_true', help='Use the physical LCD instead of frame-file output')
    args = parser.parse_args()
    args.binary = args.binary.resolve()
    if not args.binary.is_file() or args.iterations < 1 or args.seconds < 2:
        parser.error('Need an existing binary, positive iterations and at least two seconds per phase')
    root = Path(tempfile.mkdtemp(prefix='zr-'))
    print('Evidence:', root, flush=True)
    server = http.server.HTTPServer(('127.0.0.1', 0), Page)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    url = args.url or f'http://127.0.0.1:{server.server_port}/' + ('animated' if args.animated else '')
    versions = subprocess.run(['dpkg-query', '-W', 'labwc', 'libwlroots*', 'netsurf-gtk', 'zero-browser'],
                              stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True).stdout
    (root / 'environment.json').write_text(json.dumps(dict(kernel=os.uname().release, versions=versions,
        arguments={k: str(v) if isinstance(v, Path) else v for k, v in vars(args).items()}, url=url), indent=2))
    results = []
    try:
        for iteration in range(args.iterations):
            renderers = args.renderers if iteration % 2 == 0 else list(reversed(args.renderers))
            for mode in args.modes:
                for renderer in renderers:
                    results.append(run(args, root, iteration, renderer, mode, url))
                    (root / 'results.json').write_text(json.dumps(results, indent=2))
    finally:
        server.shutdown()
        server.server_close()
    raise SystemExit(0 if all(r['ok'] for r in results) else 1)


if __name__ == '__main__':
    main()
