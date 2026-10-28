#!/usr/bin/env python3
"""Exercise the complete production startup loops using a fault-injecting Wayland peer."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import json
import os
from pathlib import Path
import shutil
import signal
import struct
import subprocess
import tempfile
import time

HERE = Path(__file__).resolve().parent

def run_case(binary, shim, usb, fault, expected, failure=None, cancel=None, mode='qtwebkit', renderer=None):
    with tempfile.TemporaryDirectory(prefix='zwl-') as temporary:
        root = Path(temporary)
        bindir = root/'bin'; bindir.mkdir()
        for name in ('labwc','dbus-daemon','wlr-randr','netsurf-gtk','zero-qtwebkit','cog','cogctl','gtk-test'):
            shutil.copyfile(HERE/'helpers/recovery-peer.py', bindir/name); (bindir/name).chmod(0o755)
        for name in ('runtime','config','state','cache','home'): (root/name).mkdir()
        os.mkfifo(root/'keyboard'); keyboard = os.open(root/'keyboard', os.O_RDWR|os.O_NONBLOCK)
        (root/'fb').write_bytes(bytes(320*170*4))
        env = dict(os.environ, ZERO_RECOVERY_TEST=str(root), ZERO_TEST_FAULT=fault,
                   ZERO_BROWSER_INPUT=str(root/'keyboard'), ZERO_BROWSER_QTWEBKIT_BINARY=str(bindir/'zero-qtwebkit'),
                   LD_PRELOAD=str(shim), HOME=str(root/'home'), PATH=str(bindir)+':'+os.environ['PATH'],
                   XDG_RUNTIME_DIR=str(root/'runtime'), XDG_CONFIG_HOME=str(root/'config'),
                   XDG_STATE_HOME=str(root/'state'), XDG_CACHE_HOME=str(root/'cache'))
        env.pop('WLR_RENDERER', None)
        env.pop('WLR_RENDER_DRM_DEVICE', None)
        if renderer is not None: env['WLR_RENDERER'] = renderer
        argv = [str(binary)]
        if usb: argv += ['--fb',str(root/'fb'),'--input',str(root/'keyboard'),'--program',str(bindir/'gtk-test')]
        else: argv += ['--frame-file',str(root/'frame.ppm'),'--mode',mode]
        unrelated = subprocess.Popen(['sleep','300'])
        def key(code, val): os.write(keyboard, struct.pack('llHHi',0,0,1,code,val))
        with (root/'log').open('w+') as logfile:
            app = subprocess.Popen(argv, env=env, stdout=logfile, stderr=logfile)
            def log(): return (root/'log').read_text()
            def wait_for(predicate, timeout=65):
                end=time.monotonic()+timeout
                while time.monotonic()<end:
                    if predicate(): return
                    assert app.poll() is None, f'exited status={app.returncode}'
                    time.sleep(.02)
                raise AssertionError('deadline waiting for condition')
            try:
                if usb:
                    wait_for(lambda:'overlay.intro' in log())
                    key(28,0); key(28,1); key(28,0)
                if cancel:
                    wait_for(lambda:cancel in log())
                    started=time.monotonic(); key(1,1)
                    app.wait(timeout=5.5)
                    assert 2.8 < time.monotonic()-started < 5.5
                    assert app.returncode == 0
                elif failure:
                    wait_for(lambda:('usbimager.start_failed' if usb else 'application.start_failed') in log())
                    assert failure in log(), log()
                    assert app.poll() is None, 'failure must remain visible'
                    key(1,1); app.wait(timeout=5.5)
                else:
                    wait_for(lambda:'startup.ready' in log())
                    assert 'wayland.first_frame=READY' in log()
                    time.sleep(.15)
                    # A failure during normal operation must never enter startup recovery.
                    for e in map(json.loads,(root/'events.jsonl').read_text().splitlines()):
                        if e['kind']=='compositor' and e['attempt']==expected:
                            os.kill(e['pid'], signal.SIGTERM)
                    time.sleep(.2)
                    if app.poll() is None: key(1,1); app.wait(timeout=5.5)
                if not usb:
                    assert not list((root/'runtime/zero-browser').glob('session.*')), 'outer session directory leaked'
                attempts=int((root/'attempts').read_text())
                assert attempts==expected, (fault,attempts,expected)
                records=list(map(json.loads,(root/'events.jsonl').read_text().splitlines()))
                for e in records:
                    if e['kind']=='compositor':
                        assert not Path(e['runtime']).exists(), ('runtime leaked',e)
                        selected = renderer or ('gles2' if mode == 'full' else 'pixman')
                        assert e['renderer'] == selected, e
                        assert e['render_device'] == ('/dev/dri/renderD128' if selected == 'gles2' else None), e
                    if e['kind'] in ('compositor','bus','engine','descendant'):
                        pid=e.get('child',e['pid'])
                        assert not Path(f'/proc/{pid}').exists(), ('owned process leaked',e)
                    if e['kind']=='engine':
                        assert e['renderer'] == renderer and e['render_device'] is None, e
                        same=[x['kind'] for x in records[:records.index(e)] if x['runtime']==e['runtime']]
                        assert 'configure' in same, ('engine before output configuration',e)
                    if e['kind']=='capture': assert e['window'], 'capture before application window'
                if fault.startswith(('missing','socket','handshake','config','wrong-mode')):
                    bad=[e for e in records if e['kind']=='compositor' and e['fault']!='normal']
                    for e in bad:
                        assert not any(x['kind']=='engine' and x['runtime']==e['runtime'] for x in records)
                if fault=='missing-clipboard' and expected==3:
                    assert 'wayland.clipboard=MISSING' in log() and 'wayland.output=OK' in log()
                    assert 'wayland.retry attempt=2/3' in log() and 'wayland.retry attempt=3/3' in log()
                assert unrelated.poll() is None, 'unrelated process was terminated'
                print(f'PASS {"USB" if usb else mode} {renderer or "default"} {fault} {cancel or failure or "ready"}: {attempts} attempt(s)',flush=True)
            except BaseException:
                print(f'FAIL {usb=} {fault=} {mode=}\n'+log(),flush=True)
                raise
            finally:
                if app.poll() is None: app.terminate(); app.wait(timeout=8)
                # Test cleanup on assertion failure, strictly the fixture PIDs.
                events=root/'events.jsonl'
                if events.exists():
                    for e in map(json.loads,events.read_text().splitlines()):
                        if e['kind'] in ('compositor','bus','engine','descendant'):
                            try: os.kill(e.get('child',e['pid']),signal.SIGKILL)
                            except ProcessLookupError: pass
                unrelated.terminate(); unrelated.wait(); os.close(keyboard)

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('binary',type=Path)
    parser.add_argument('--usb',action='store_true')
    parser.add_argument('--quick',action='store_true')
    args=parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='zwl-shim-') as tmp:
        shim=Path(tmp)/'io.so'
        subprocess.run(['cc','-shared','-fPIC',str(HERE/'helpers/recovery-io.c'),'-ldl','-o',str(shim)],check=True)
        cases=[('normal',1,None,None),('delayed',1,None,None),('capture-twice',3,None,None),
               ('config-once',2,None,None),('swapchain-once',2,None,None),('engine',1,'stage=engine',None)]
        if not args.quick:
            cases += [('missing-output-once',2,None,None),('missing-clipboard',3,'stage=protocols',None),
                      ('socket-once',2,None,None),('handshake-once',2,None,None),
                      ('wrong-mode-once',2,None,None),('capture-hang-once',2,None,None),
                      ('disconnect-once',2,None,None),('crash-once',2,None,None),
                      ('no-window',1,'stage=window',None),('handshake',1,None,'stage=protocols'),
                      ('missing-clipboard',2,None,'wayland.retry attempt=2/3'),
                      ('config-hang',1,None,'stage=output')]
        with ThreadPoolExecutor(max_workers=4) as pool:
            tasks=[pool.submit(run_case,args.binary.resolve(),shim,args.usb,*case) for case in cases]
            if not args.usb:
                tasks += [pool.submit(run_case,args.binary.resolve(),shim,False,'capture-once',2,None,None,'lite')]
                tasks += [pool.submit(run_case,args.binary.resolve(),shim,False,'normal',1,None,None,mode,renderer)
                          for mode in ('lite','qtwebkit') for renderer in ('pixman','gles2')]
                tasks += [pool.submit(run_case,args.binary.resolve(),shim,False,'normal',1,None,None,'full')]
            for task in tasks: task.result()

if __name__=='__main__': main()
