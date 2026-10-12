"""Bounded real C journal and explicit identity joins; no Gamescope/APK build."""
import importlib.util
import json
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import time
import unittest
ROOT=Path(__file__).resolve().parent
spec=importlib.util.spec_from_file_location('analyze_frame_journal', ROOT.parent/'panvk/analyze_frame_journal.py')
analyzer=importlib.util.module_from_spec(spec);spec.loader.exec_module(analyzer)


class JournalTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp=tempfile.TemporaryDirectory(prefix='dd-frame-journal-');cls.addClassCleanup(cls.temp.cleanup)
        cls.root=Path(cls.temp.name);cls.binary=cls.root/'journal'
        subprocess.run(shlex.split(os.environ.get('HOST_CC','cc'))+['-std=c11','-Wall','-Wextra','-Werror',
                        '-DDD_FT_CAPACITY=64',f'-DDD_FT_ROOT="{cls.root}"',str(ROOT/'frame_trace.c'),
                        str(ROOT/'tests/frame_trace_host.c'),'-pthread','-o',str(cls.binary)],check=True)
        cls.android=cls.root/'android-callback'
        java=Path(os.environ.get('JAVA_HOME','/usr/lib/jvm/default'))
        subprocess.run(shlex.split(os.environ.get('HOST_CC','cc'))+['-std=c11','-Wall','-Wextra','-Werror',
                        '-Wno-unused-parameter','-Wno-unused-function','-ffunction-sections','-fdata-sections',
                        '-I'+str(ROOT/'tests'),'-I'+str(java/'include'),'-I'+str(java/'include/linux'),
                        f'-DDD_FT_ROOT="{cls.root}"',str(ROOT/'frame_trace.c'),str(ROOT/'tests/frame_trace_android.c'),
                        '-pthread','-Wl,--gc-sections','-o',str(cls.android)],check=True)
        subprocess.run(shlex.split(os.environ.get('HOST_CXX','c++'))+['-std=c++20','-Wall','-Wextra','-Werror',
                        '-x','c++','-fsyntax-only',str(ROOT/'frame_trace.c')],check=True)

    def setUp(self):
        for path in self.root.glob('*'):
            if path.name not in ('journal','android-callback'):path.unlink()

    def test_disabled_has_no_worker_or_output(self):
        subprocess.run([str(self.binary),'disabled'],check=True,timeout=2,capture_output=True)
        self.assertEqual(sorted(p.name for p in self.root.iterdir()),['android-callback','journal'])

    def capture(self,mode):
        (self.root/'armed').touch()
        start=time.monotonic_ns()+150_000_000;end=start+500_000_000
        (self.root/'control').write_text(f'{start} {start} {end} 8\n')
        subprocess.run([str(self.binary),mode],check=True,timeout=4,capture_output=True)
        paths=list(self.root.glob('trace-*.jsonl'));self.assertEqual(len(paths),1)
        return analyzer.load(paths[0])

    def test_future_window_ids_cpu_and_explicit_overflow(self):
        header,footer,events=self.capture('normal')
        self.assertLessEqual(header['ready_ns'],header['start_ns'])
        self.assertTrue(footer['complete']);self.assertGreater(footer['dropped'],0)
        self.assertEqual(footer['count'],64)
        self.assertEqual({(e['key'],e['frame'],e['client'],e['fence']) for e in events},{(3,8,8,5)})
        self.assertTrue(all(e['cpu_ns'] for e in events))
        self.assertTrue(analyzer.spans(events))
        self.assertFalse(list(self.root.glob('ready-*')))

    def test_real_android_callback_keeps_incoming_and_previous_frame_separate(self):
        (self.root/'armed').touch()
        start=time.monotonic_ns()+150_000_000;end=start+400_000_000
        (self.root/'control').write_text(f'{start} {start} {end} 8\n')
        subprocess.run([str(self.android)],check=True,timeout=4,capture_output=True)
        _,footer,events=analyzer.load(next(self.root.glob('trace-*.jsonl')))
        self.assertTrue(footer['complete']);self.assertEqual(footer['dropped'],0)
        identities={e['stage']:(e['key'],e['frame']) for e in events}
        self.assertEqual(identities['transaction_complete'],(2,20))
        self.assertEqual(identities['android_latch_time'],(2,20))
        self.assertEqual(identities['release_callback'],(1,10))

    def test_concurrent_overflow_is_counted_and_never_overwritten(self):
        _,footer,events=self.capture('flood')
        self.assertTrue(footer['complete']);self.assertEqual(footer['count'],64)
        self.assertGreater(footer['dropped'],0)
        self.assertTrue({e['key'] for e in events} <= {1,2})


class JoinTests(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory();self.addCleanup(self.temp.cleanup);self.root=Path(self.temp.name)

    def event(self,stage,ns,**extra):
        return dict(type='event',event_seq=0,stage=stage,mono_ns=ns,cpu_ns=0,tid=1,
                    client=extra.pop('client',8),attempt=extra.pop('attempt',0),key=extra.pop('key',3),
                    frame=extra.pop('frame',1),sync=4,fence=5,object=0,value=0,status=0,**extra)

    def write(self,role,events,dropped=0):
        header=dict(type='header',version=1,clock='CLOCK_MONOTONIC',run=1,start_ns=1,end_ns=100,
                    ready_ns=1,pid=1 if role=='gamescope' else 2,role=role)
        for i,e in enumerate(events):e['event_seq']=i+1
        path=self.root/(role+'.jsonl')
        path.write_text('\n'.join(json.dumps(r) for r in [header,*events,dict(type='footer',count=len(events),dropped=dropped,complete=True)]))
        return path

    def test_same_timestamps_do_not_join_wrong_key(self):
        result=analyzer.analyze([self.write('gamescope',[self.event('publish',10,attempt=1)]),
                                 self.write('android',[self.event('publish',10,key=4)])])
        self.assertEqual(result['joined_frames'],0)
        self.assertEqual(len(result['unmatched_publish_ids']),2)

    def test_old_frame_on_reused_ahb_does_not_join(self):
        result=analyzer.analyze([self.write('gamescope',[self.event('publish',10,attempt=1)]),
                                 self.write('android',[self.event('publish',10,frame=2)])])
        self.assertEqual(result['joined_frames'],0)

    def test_matching_key_frame_requires_same_broker_fence_and_sync_ids(self):
        for field in ('fence','sync'):
            with self.subTest(field=field):
                native=self.event('publish',10)
                native[field]=0x9abcdef0
                result=analyzer.analyze([self.write('gamescope',[self.event('publish',10,attempt=1)]),
                                         self.write('android',[native])])
                self.assertEqual(result['joined_frames'],0)

    def test_attempt_prepublication_and_previous_release_identity(self):
        game=[self.event('shm_import_begin',5,attempt=1,frame=0,key=0),self.event('shm_import_end',9,attempt=1,frame=0,key=0),self.event('publish',20,attempt=1)]
        native=[self.event('vk_submit_begin',10,frame=0),self.event('vk_submit_end',11,frame=0),self.event('publish',19),self.event('release_callback',60),self.event('pool_return',70)]
        result=analyzer.analyze([self.write('gamescope',game),self.write('android',native)])
        self.assertEqual(result['joined_frames'],1)
        self.assertTrue(result['frames'][0]['pool_return_observed'])
        self.assertIn('gamescope/shm_import',result['stage_spans'])
        self.assertIn('android/vk_submit',result['stage_spans'])
        self.assertIsNone(result['frames'][0]['physical_present_ns'])

    def test_loss_and_duplicate_publication_remain_unproven(self):
        result=analyzer.analyze([self.write('gamescope',[self.event('publish',10,attempt=1),self.event('publish',11,attempt=1)]),self.write('android',[self.event('publish',9)],dropped=1)])
        self.assertEqual(result['joined_frames'],0)
        self.assertFalse(result['lossless_complete_window'])

    def test_equal_timeline_ids_in_different_native_scopes_do_not_alias(self):
        game=[self.event('publish',20,attempt=1)]
        submit_a=self.event('vk_submit_begin',10,frame=0);submit_a.update(scope=1,object=9,value=8)
        submit_b=self.event('vk_submit_end',11,frame=0);submit_b.update(scope=1,object=9,value=8)
        wait_a=self.event('wait_begin',25,frame=0,key=0);wait_a.update(scope=2,object=9,value=8)
        wait_b=self.event('wait_end',26,frame=0,key=0);wait_b.update(scope=2,object=9,value=8)
        paths=[self.write('gamescope',game),self.write('android',[submit_a,submit_b,self.event('publish',19),wait_a,wait_b])]
        result=analyzer.analyze(paths)
        self.assertNotIn('android/wait',result['stage_spans'])
        wait_a['scope']=wait_b['scope']=1
        paths[1]=self.write('android',[submit_a,submit_b,self.event('publish',19),wait_a,wait_b])
        result=analyzer.analyze(paths)
        self.assertIn('android/wait',result['stage_spans'])

    def test_cross_run_is_rejected(self):
        a=self.write('gamescope',[]);b=self.write('android',[])
        b.write_text(b.read_text().replace('"run": 1','"run": 2'))
        with self.assertRaisesRegex(ValueError,'different runs'):analyzer.analyze([a,b])

    def test_cpu_from_different_threads_is_not_paired(self):
        a=self.event('render_begin',10,attempt=1);b=self.event('render_end',30,attempt=1)
        a.update(pid=1,role='gamescope',cpu_ns=10);b.update(pid=1,role='gamescope',tid=2,cpu_ns=20)
        self.assertEqual(analyzer.spans([a,b]),[])


class ControlTests(unittest.TestCase):
    def test_gamescope_fence_assignment_with_opaque_vulkan_handle(self):
        # Compile the actual patched assignment against the private ABI. This
        # catches using a VkFence pointer as a uint32_t journal identity, without
        # building Gamescope. The real proxy-ID export is exercised by test_registry.
        patch=(ROOT.parent/'gamescope/patches/0126-mali-frame-trace.patch').read_text()
        assignments=[line[1:] for line in patch.splitlines()
                     if line.startswith('+') and 'g_maliFrameTrace.fence =' in line]
        self.assertEqual(len(assignments),1)
        with tempfile.TemporaryDirectory(prefix='dd-frame-fence-') as directory:
            root=Path(directory);source=root/'fence.cpp';binary=root/'fence'
            source.write_text('''#include <cassert>
#include <cstdint>
typedef struct VkFence_T *VkFence;
typedef void *VkDevice, *VkImage, *VkDeviceMemory;
typedef int VkResult;
#define VKAPI_PTR
#include "interop_test_api.h"
#include "frame_trace.h"
int main() {
    dd_interop_output producer{};
    producer.fence = reinterpret_cast<VkFence>(uintptr_t(0x9abcdef0));
    producer.token = 0x12345678;
    dd_ft_id g_maliFrameTrace{};
''' + '\n'.join(assignments) + '''
    assert(g_maliFrameTrace.fence == 0x12345678);
    assert(producer.fence == reinterpret_cast<VkFence>(uintptr_t(0x9abcdef0)));
}
''')
            subprocess.run(shlex.split(os.environ.get('HOST_CXX','c++'))+
                           ['-std=c++20','-Wall','-Wextra','-Werror','-I'+str(ROOT),
                            str(source),'-o',str(binary)],check=True,timeout=15)
            subprocess.run([str(binary)],check=True,timeout=3)

    def test_unarmed_does_not_create_control(self):
        import steam_frame_journal as controller
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory);messages=[]
            self.assertIsNone(controller.schedule(10,lambda *x:messages.append(x),root))
            self.assertFalse(list(root.iterdir()))
            self.assertEqual(messages[0][0],'frame-journal-unavailable')

    def test_missing_workers_never_arms_partial_capture(self):
        import steam_frame_journal as controller
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory);(root/'armed').touch()
            self.assertIsNone(controller.schedule(10,lambda *x:None,root))
            self.assertFalse((root/'control').exists())

    def test_patch_carries_exact_canonical_c_and_header(self):
        patch=(ROOT.parent/'gamescope/patches/0126-mali-frame-trace.patch').read_text()
        for filename,canonical in [('frame_trace.h','frame_trace.h'),('frame_trace.inc','frame_trace.c')]:
            section=patch.split('+++ b/src/'+filename+'\n',1)[1].split('\n--- ',1)[0]
            actual='\n'.join(line[1:] for line in section.splitlines() if line.startswith('+'))+'\n'
            self.assertEqual(actual,(ROOT/canonical).read_text())
