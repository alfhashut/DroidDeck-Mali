"""Actual Gamescope Wayland backend/renderer/client; host GPU/Android callback MODELS."""
import contextlib
import os
from pathlib import Path
import re
import shlex
import signal
import subprocess
import time
import test_renderer
ROOT=Path(__file__).resolve().parent
class NormalSessionTests(test_renderer.GamescopeRendererFixture):
    @classmethod
    def setUpClass(cls):
        super().setUpClass()
        build=cls.directory/'renderer-build'; wlr=Path(os.environ['WLR_BUILD'])
        protocols=Path(os.environ['WAYLAND_PROTOCOLS_DIR'] if 'WAYLAND_PROTOCOLS_DIR' in os.environ else subprocess.check_output(['pkg-config','--variable=pkgdatadir','wayland-protocols'],text=True).strip())
        xmls=list((cls.source/'protocol').glob('*.xml'))
        for suffix in ('stable/linux-dmabuf/linux-dmabuf-v1.xml','stable/viewporter/viewporter.xml','stable/xdg-shell/xdg-shell.xml','stable/presentation-time/presentation-time.xml','staging/single-pixel-buffer/single-pixel-buffer-v1.xml','unstable/pointer-constraints/pointer-constraints-unstable-v1.xml','unstable/relative-pointer/relative-pointer-unstable-v1.xml','unstable/primary-selection/primary-selection-unstable-v1.xml','staging/fractional-scale/fractional-scale-v1.xml','staging/linux-drm-syncobj/linux-drm-syncobj-v1.xml'):
            xmls.append(protocols/suffix)
        cc=shlex.split(os.environ.get('HOST_CC','cc')); cxx=shlex.split(os.environ.get('HOST_CXX','c++'))
        protocol_objects=[]
        for xml in xmls:
            for mode,suffix in (('client-header','-client-protocol.h'),('server-header','-protocol.h'),('private-code','-protocol.c')):
                subprocess.run(['wayland-scanner',mode,str(xml),str(build/(xml.stem+suffix))],check=True,capture_output=True)
            obj=build/(xml.stem+'-protocol.o');protocol_objects.append(str(obj))
            subprocess.run(cc+['-ffunction-sections','-fdata-sections','-c',str(build/(xml.stem+'-protocol.c')),'-o',str(obj)],check=True)
        includes=[build,Path(os.environ.get('VULKAN_HEADERS','/usr/include')),Path(os.environ['WLR_HEADERS']),wlr/'protocol',wlr/'include',Path(os.environ.get('WAYLAND_PROTOCOLS_HEADERS','/usr/include')),cls.source/'src',Path('/usr/include/pixman-1'),Path('/usr/include/libdrm'),Path('/usr/include/libdecor-0')]
        flags=['-std=c++20','-O0','-fno-exceptions','-ffunction-sections','-fdata-sections','-DWLR_USE_UNSTABLE','-DHAVE_DRM=1']+['-I'+str(p) for p in includes]
        objects=[str(build/'rendervulkan.o')]
        for src in [cls.source/'src'/name for name in ('backend.cpp','Backends/WaylandBackend.cpp','mali_normal.cpp','mali_interactive.cpp')]+[ROOT/'tests/normal_host.cpp']:
            obj=build/(src.stem+'.o');objects.append(str(obj));subprocess.run(cxx+flags+['-c',str(src),'-o',str(obj)],check=True)
        cls.binary=build/'normal-session-test'
        libraries=['-L'+str(wlr),'-Wl,-rpath,'+str(wlr),'-lwlroots-0.20','-lwayland-server','-lwayland-client','-lwayland-cursor','-lxkbcommon','-ldecor-0','-ldl','-pthread','-ldrm']
        subprocess.run(cxx+['-Wl,--gc-sections',*objects,*protocol_objects,*libraries,'-o',str(cls.binary)],check=True)
        java=Path(os.environ.get('JAVA_HOME','/usr/lib/jvm/default'))
        cls.outer=build/'normal-outer'
        cflags=['-std=c11','-O2','-Wall','-Wextra','-Werror','-Wno-unused-parameter','-DWLR_USE_UNSTABLE','-DMB_NORMAL_RELEASE_WAIT_MS=50']+['-I'+str(p) for p in includes+[ROOT,ROOT/'tests',java/'include',java/'include/linux']]
        subprocess.run(cc+cflags+[str(ROOT/'tests/normal_outer.c'),str(ROOT/'../../app/src/main/cpp/malivulkan/normal_ownership.c'),*[str(build/(name+'-protocol.o')) for name in ('banner-ahb-v1','linux-dmabuf-v1','presentation-time')],'-L'+str(wlr),'-Wl,-rpath,'+str(wlr),'-lwlroots-0.20','-lwayland-server','-lxkbcommon','-lpixman-1','-pthread','-o',str(cls.outer)],check=True)

    @contextlib.contextmanager
    def outer_server(self,mode=72):
        # A fresh runtime socket directory, while the same server is reused by restart tests.
        import tempfile
        with tempfile.TemporaryDirectory(prefix='normal-wayland-') as directory:
            env=dict(os.environ,XDG_RUNTIME_DIR=directory,WAYLAND_DISPLAY='wayland-0',MALI_VULKAN_NORMAL_SESSION='1',MALI_VULKAN_BROKER_SOCKET=directory+'/broker.sock',VK_DRIVER_FILES=str(self.manifest),VK_ICD_FILENAMES=str(self.manifest),VK_LOADER_LAYERS_DISABLE='*')
            with open(directory+'/server.log','w+') as errors:
                server=subprocess.Popen([str(self.outer),'--serve',env['MALI_VULKAN_BROKER_SOCKET'],str(mode)],env=env,stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=errors,text=True)
                try:
                    self.assertEqual(server.stdout.readline().strip(),'READY')
                    yield env,server
                finally:
                    output=server.communicate('\n',timeout=8)[0];errors.seek(0);self.outer_errors=errors.read();self.outer_output=output
                    self.assertEqual(server.returncode,0,output+self.outer_errors)
    def command(self):
        return [str(self.binary),'--backend','wayland','--mali-wayland-session','-f','-W','640','-H','360','--',str(self.binary),'--mali-interactive-client']
    def test_nested_input_clean_stop_and_second_launch(self):
        with self.outer_server() as (env,server):
            for repeat in range(2):
                result=subprocess.run(self.command(),env=env,capture_output=True,text=True,timeout=20)
                output=result.stdout+result.stderr
                self.assertEqual(result.returncode,0,output)
                self.assertIn('Initted Wayland backend',output)
                self.assertIn('outer WAYLAND_DISPLAY=wayland-0',output)
                self.assertIn('normal nested Wayland output: 640x360 pool=3',output)
                self.assertRegex(output,r'interactive client stopped: .*motion=[1-9]\d* click=2 key=2 exit=0')
                self.assertIn('square=146,63 colour=2',output) # actual touch + D changed the application's painted state
                self.assertIn('rendered=100 committed=100 released=100 release timeouts=0',output)
                self.assertIn('baseline devices=0 queues=0',output)
                self.assertNotIn('selected frame=',output)
                self.assertNotIn('BLIT descriptor',output)
        self.assertIn('readbacks=0 frames=200',self.outer_output)
        self.assertNotIn('TIMEOUT',self.outer_errors)
    def test_user_stop_owned_buffer_and_unexpected_child_exit(self):
        for action in ('stop','child'):
            with self.subTest(action=action),self.outer_server() as (env,server):
                process=subprocess.Popen(self.command(),env=env,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
                lines=[];child=None
                try:
                    while True:
                        line=process.stdout.readline();lines.append(line);self.assertTrue(line)
                        match=re.search(r'normal Wayland client pid=(\d+)',line)
                        if match:child=int(match[1])
                        if line.startswith('interactive client:'):break
                    time.sleep(.25)
                    os.kill(child if action=='child' else process.pid,signal.SIGKILL if action=='child' else signal.SIGTERM)
                    out,err=process.communicate(timeout=8);output=''.join(lines)+out+err
                    self.assertEqual(process.returncode,1 if action=='child' else 0,output)
                    self.assertIn('release timeouts=0; clean teardown',output)
                finally:
                    if process.poll() is None:process.kill();process.communicate()
        self.assertIn('baseline zero',self.outer_output)
    def test_missing_release_fails_and_quarantines(self):
        with self.outer_server(71) as (env,server):
            process=subprocess.Popen(self.command(),env=env,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
            lines=[]
            try:
                while True:
                    line=process.stdout.readline();lines.append(line);self.assertTrue(line)
                    if line.startswith('interactive client:'):break
                time.sleep(.3);process.send_signal(signal.SIGTERM)
                out,err=process.communicate(timeout=8);output=''.join(lines)+out+err
                self.assertEqual(process.returncode,1,output)
                self.assertIn('state=ANDROID_OWNED; retained',output)
                self.assertNotIn('normal session stopped:',output)
                self.assertNotIn('normal final native children:',out)
            finally:
                if process.poll() is None:process.kill();process.communicate()
        self.assertIn('Android release wait TIMEOUT: buffer token=',self.outer_errors)
        self.assertIn('quarantine retained; no fake release',self.outer_output)
    def test_wrong_outer_socket_and_missing_normal_optin_fail(self):
        with self.outer_server() as (env,server):
            for variable,value in (('WAYLAND_DISPLAY','wayland-wrong'),('MALI_VULKAN_NORMAL_SESSION','0')):
                with self.subTest(variable=variable):
                    result=subprocess.run(self.command(),env=dict(env,**{variable:value}),capture_output=True,text=True,timeout=3)
                    self.assertEqual(result.returncode,1)
                    self.assertIn('requires its isolated environment and outer wayland-0',result.stderr)

    def test_broker_disconnect_and_android_compositor_loss(self):
        for mode in (73,74):
            with self.subTest(mode=mode),self.outer_server(mode) as (env,server):
                result=subprocess.run(self.command(),env=env,capture_output=True,text=True,timeout=10)
                output=result.stdout+result.stderr
                self.assertEqual(result.returncode,1,output)
                self.assertNotIn('normal session stopped: rendered=100',output)
                self.assertIn('FAILED',output)
            self.assertIn('baseline zero',self.outer_output)
