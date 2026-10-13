"""Standalone mapped-transfer tests; no ICD/broker/Gamescope/Android build."""
from pathlib import Path
import os
import re
import shlex
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent


class BulkMemoryTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix='mali-bulk-')
        cls.addClassCleanup(cls.temp.cleanup)
        build = Path(cls.temp.name)
        proxy = (ROOT / 'interop_icd.h').read_text()
        upload = proxy.split('static VkResult interop_upload_mapping', 1)[1].split('/* Only the private upload-only staging API', 1)[0]
        (build / 'upload.inc').write_text('static VkResult interop_upload_mapping' + upload)
        map_rpc = proxy.split('static VkResult interop_rpc', 1)[1].split('static void interop_publish', 1)[0]
        (build / 'map_rpc.inc').write_text('static VkResult interop_rpc' + map_rpc)
        binding = proxy.split('static VkResult interop_bind', 1)[1].split('static VKAPI_ATTR VkResult VKAPI_CALL proxy_BindBufferMemory', 1)[0]
        (build / 'profile_binding.inc').write_text('static VkResult interop_bind' + binding)
        mappings = proxy.split('static VkResult interop_map_memory', 1)[1].split('static VkResult interop_mapped_range', 1)[0]
        (build / 'mapping.inc').write_text('static VkResult interop_map_memory' + mappings)
        native = (ROOT.parents[1] / 'app/src/main/cpp/malivulkan/interop_commands.h').read_text()
        memory = native.split('    case MB_MEMORY_MAP: case MB_MEMORY_UNMAP:', 1)[1].split('    case MB_COMMAND_FILL:', 1)[0]
        (build / 'native_shared.inc').write_text((ROOT.parents[1] / 'app/src/main/cpp/malivulkan/shared_staging_commands.h').read_text())
        (build / 'native_memory.inc').write_text('    case MB_MEMORY_MAP: case MB_MEMORY_UNMAP:' + memory)
        renderer = (ROOT / 'renderer_icd.h').read_text().split('static VkResult renderer_QueueSubmit', 1)[1].split('static void renderer_Barrier', 1)[0]
        (build / 'submit.inc').write_text('static VkResult renderer_QueueSubmit' + renderer)
        # Exercise the actual device lookup that Gamescope uses, rather than a
        # hand-written lookup that can hide omissions in the ICD dispatch table.
        dispatch = 'static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL proxy_GetDeviceProcAddr' + (ROOT / 'device_icd.h').read_text().split(
            'static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL proxy_GetDeviceProcAddr', 1)[1]
        names = set(re.findall(r'proxy_(\w+)', dispatch))
        names.update(re.findall(r'DEVICE_ENTRY\((\w+)\)', dispatch))
        for table in ('submit_entries.def', 'interop_entries.def', 'renderer_entries.def'):
            names.update(re.findall(r'ENTRY\((\w+)\)', (ROOT / table).read_text()))
        real = {'n', 'GetDeviceProcAddr', 'logical', 'MapMemory', 'UnmapMemory', 'DroidDeckStagingMALI', 'DroidDeckMapStagingMALI', 'DroidDeckUploadRingMALI', 'DroidDeckFrameTraceMALI'}
        stubs = ''.join('static void proxy_' + name + '(void) {}\n' for name in sorted(names - real))
        version = next(line for line in (ROOT / 'submit_protocol.h').read_text().splitlines() if line.startswith('#define MB_SUBMIT_VERSION '))
        (build / 'device_dispatch.inc').write_text(version + '\n' + stubs + dispatch)
        metric = (ROOT / 'icd_proxy.c').read_text().split('static VkResult rpc(struct proxy_instance *s,', 1)[1].split('static void proxy_free_logical', 1)[0]
        (build / 'metric.inc').write_text('static VkResult rpc(struct proxy_instance *s,' + metric)
        cleanup = (ROOT / 'submit_icd.h').read_text().split('static void proxy_free_resources', 1)[1].split('static struct proxy_resource *submit_find', 1)[0]
        (build / 'cleanup.inc').write_text('static void proxy_free_resources' + cleanup)
        finder = (ROOT / 'submit_icd.h').read_text().split('static struct proxy_resource *submit_find', 1)[1].split('/* Logical retirement', 1)[0]
        (build / 'finder.inc').write_text('static struct proxy_resource *submit_find' + finder)
        cls.binary = build / 'bulk'
        subprocess.run(shlex.split(os.environ.get('HOST_CC', 'cc')) + [
            '-std=c11', '-D_POSIX_C_SOURCE=200809L', '-O0', '-Wall', '-Wextra', '-Werror',
            '-I' + str(build), '-I' + str(ROOT), str(ROOT / 'tests/memory_bulk.c'),
            '-pthread', '-o', str(cls.binary)], check=True, timeout=15)

    def run_case(self, name):
        result = subprocess.run([str(self.binary), name], capture_output=True, text=True, timeout=3)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn(name + ' PASS', result.stdout)

    def test_precise_ring_empty_U_small_R_and_unchanged_SHM(self):
        self.run_case('ring_frame')

    def test_ring_U_R_epochs_merging_gaps_wrap_and_bounds_fallback(self):
        self.run_case('ring_ranges')

    def test_ring_write_ACK_submit_failures_and_newer_epoch_survive(self):
        self.run_case('ring_failures')

    def test_bulk_counts_bytes_large_ranges_and_request_reuse(self):
        self.run_case('bulk')

    def test_shared_mirror_direct_pointer_exact_frames_and_zero_pixel_payloads(self): self.run_case('shared_direct')
    def test_shared_registration_decline_retains_two_MiB_bulk(self): self.run_case('shared_fallback')
    def test_shared_local_mapping_failure_drops_region_before_bulk_fallback(self): self.run_case('shared_map_failure')
    def test_shared_connection_region_cap_falls_back_for_fourth_slot(self): self.run_case('shared_cap')
    def test_shared_notification_failure_never_submits_or_sends_bulk(self): self.run_case('shared_failure')
    def test_shared_bad_ACK_never_submits_or_sends_bulk(self): self.run_case('shared_bad_ack')
    def test_shared_native_ranges_generations_pending_and_lengths(self): self.run_case('shared_bounds')
    def test_shared_unmap_remap_restarts_generation_and_releases_regions(self): self.run_case('shared_restart')
    def test_shared_exact_subranges_leave_other_bytes_untouched(self): self.run_case('shared_ranges')
    def test_shared_oversized_mapping_uses_bounded_bulk_without_registration(self): self.run_case('shared_oversized')

    def test_staging_four_MiB_uses_two_writes_not_eight_exact_bytes_offsets_reuse(self):
        self.run_case('staging_bulk')

    def test_staging_large_allocation_ACK_and_partial_chunk_failure_prevent_submit(self):
        self.run_case('staging_bulk_errors')

    def test_staging_large_payload_bounds_modes_versions_and_unchanged_shader_cap(self):
        self.run_case('staging_bulk_bounds')

    def test_staging_two_large_writes_over_small_stream_socket_and_fragmented_ACK(self):
        self.run_case('staging_bulk_socket')

    def test_protocol_patch_applies_zero_fuzz_and_matches_canonical_headers(self):
        patches = ROOT.parent / 'gamescope/patches'
        original = (patches / '0121-mali-normal-wayland-session.patch').read_text()
        with tempfile.TemporaryDirectory(prefix='mali-bulk-patch-') as directory:
            root = Path(directory); (root / 'src').mkdir()
            for name in ('interop_protocol.h', 'renderer_protocol.h'):
                section = original.split('+++ b/src/' + name + '\n', 1)[1].splitlines()
                count = int(re.fullmatch(r'@@ -0,0 \+1,(\d+) @@', section[0])[1])
                self.assertTrue(all(line.startswith('+') for line in section[1:count+1]))
                (root / 'src' / name).write_text(''.join(line[1:]+'\n' for line in section[1:count+1]))
            result = subprocess.run(['patch', '-p1', '--batch', '--fuzz=0', '-i',
                str(patches / '0127-mali-staging-bulk-writes.patch')], cwd=root,
                capture_output=True, text=True, check=True)
            self.assertNotIn('fuzz', result.stdout.lower())
            self.assertNotIn('offset', result.stdout.lower())
            for name in ('interop_protocol.h', 'renderer_protocol.h'):
                self.assertEqual((root / 'src' / name).read_bytes(), (ROOT / name).read_bytes())

    def test_all_coherent_uploads_acknowledged_before_each_submit(self):
        self.run_case('ordering')

    def test_persistent_staging_only_uploads_for_armed_submission(self):
        self.run_case('persistent_staging')

    def test_persistent_staging_map_skips_download_and_unmaps_after_completion(self):
        self.run_case('persistent_map')

    def test_actual_device_private_api_resolution_and_availability(self):
        self.run_case('staging_dispatch')

    def test_ordinary_map_still_downloads_initial_coherent_bytes(self):
        self.run_case('ordinary_map')

    def test_three_write_purposes_ranges_bytes_and_submission_attribution(self):
        self.run_case('write_profile')

    def test_disabled_write_profiling_keeps_transport_identical(self):
        self.run_case('profile_disabled')

    def test_5000_frames_submit_discovers_only_live_persistent_mappings(self):
        self.run_case('registry_submit')

    def test_bulk_reads_exact_bytes_minimum_chunks_and_reply_reuse(self):
        self.run_case('read_bulk')

    def test_failed_partial_malformed_reads_and_allocation_failure(self):
        self.run_case('read_errors')

    def test_upload_errors_bad_ack_and_allocation_failure_prevent_submit(self):
        self.run_case('errors')

    def test_native_and_proxy_length_offset_mapping_and_overflow_bounds(self):
        self.run_case('bounds')

    def test_v6_reads_writes_keep_4k_and_old_v7_requests_remain_valid(self):
        self.run_case('legacy')


if __name__ == '__main__':
    unittest.main()
