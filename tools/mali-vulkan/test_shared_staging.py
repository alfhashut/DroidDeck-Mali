"""Small real memfd/SCM_RIGHTS/actual exchange harness, no renderer or driver build."""
from pathlib import Path
import re
import subprocess
import tempfile
import unittest
ROOT=Path(__file__).resolve().parent
class SharedTransportTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp=tempfile.TemporaryDirectory(prefix='dd-shared-');cls.addClassCleanup(cls.temp.cleanup)
        root=Path(cls.temp.name)
        source=(ROOT/'icd_proxy.c').read_text().split('static VkResult rpc_exchange(',1)[1].split('/* Callers already hold',1)[0]
        function='static VkResult rpc_exchange('+source
        vk=sorted(set(re.findall(r'\bVK_[A-Z_]+\b',function))-{'VK_SUCCESS'})
        constants='typedef int VkResult;\nenum {VK_SUCCESS=0,'+','.join(f'{name}={-i}' for i,name in enumerate(vk,1))+'};\n'
        for i,name in enumerate(sorted(set(re.findall(r'\bMB_[A-Z_]+\b',function)))):
            constants+=f'#ifndef {name}\n#define {name} {1000+i}\n#endif\n'
        harness=(ROOT/'tests/shared_staging_transport.c').read_text().replace('CONSTANTS',constants).replace('RPC_EXCHANGE',function)
        (root/'test.c').write_text(harness);cls.binary=root/'test'
        subprocess.run(['cc','-std=c11','-O0','-Wall','-Wextra','-Werror','-I'+str(ROOT),str(root/'test.c'),'-pthread','-o',str(cls.binary)],check=True,timeout=15)
    def run_case(self,name):
        result=subprocess.run([str(self.binary),name],capture_output=True,text=True,timeout=5)
        self.assertEqual(result.returncode,0,result.stdout+result.stderr)
    def test_seals_bounds_CLOEXEC_extra_truncated_missing_FDs_and_cleanup(self):self.run_case('fds')
    def test_actual_cross_process_shared_pages_and_release_acquire_sequence(self):self.run_case('processes')
    def test_actual_proxy_exchange_receives_registered_FD(self):self.run_case('exchange')
    def test_registration_decline_has_no_FD_receive(self):self.run_case('decline')
    def test_malformed_FD_envelope_fails_exchange(self):self.run_case('malformed')
    def test_disposable_probe_old_broker_fallback(self):self.run_case('old_probe')
    def test_disposable_probe_capability_bounds(self):self.run_case('probe')
    def test_gate_is_default_off_and_client_environment_strips_it(self):
        repo=ROOT.parents[1]
        gate=(repo/'app/src/main/java/com/droiddeck/launcher/gpu/MaliNormalBroker.kt').read_text()
        self.assertIn('"shared-staging.enabled").isFile',gate)
        self.assertIn('MALI_VULKAN_SHARED_STAGING=${if (sharedStaging) "1" else "0"}',gate)
        self.assertIn('"MALI_VULKAN_SHARED_STAGING"',(repo/'tools/gamescope/patches/0124-mali-steam-xwayland-client.patch').read_text())
        self.assertIn('mb_shared_probe(path)',(ROOT/'icd_proxy.c').read_text())
if __name__=='__main__':unittest.main()
