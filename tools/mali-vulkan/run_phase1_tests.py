"""Run isolated host contracts without Android session or Gamescope integration."""
import argparse
from pathlib import Path
import sys
import unittest


HERE = Path(__file__).resolve().parent
MALI_TESTS = (
    'test_probe', 'test_bulk', 'test_registry', 'test_shared_staging', 'test_upload_trace',
    'test_staging', 'test_wait', 'test_upload_ring', 'test_frame_cost',
    'test_perf.PerformanceTests.test_opcode_names_match_every_wire_opcode',
    'test_perf.PerformanceTests.test_snapshot_legacy_size_and_extended_attribution',
    'test_normal.NormalOwnershipTests.test_android_owned_release_timeout_and_restart',
    'test_frame_trace.JoinTests',
    'test_frame_trace.ControlTests.test_unarmed_does_not_create_control',
    'test_frame_trace.ControlTests.test_missing_workers_never_arms_partial_capture',
    'test_steam_angle_probe', 'test_steam_cef_diagnostics', 'test_steam_x11_diagnostics',
)
LOADER_TESTS = (
    'test_icd', 'test_interop', 'test_renderer.RendererTransportTests',
    'test_session.SessionBrokerTests',
)
PANVK_TESTS = (
    'test_artifact', 'test_affinity', 'test_affinity_summary', 'test_frame_journal_control',
    'test_frames', 'test_perf', 'test_shm_decomposition', 'test_swap_details',
    'test_upload_queue', 'test_upload_sampling',
)
DEFERRED_MALI = {
    'test_bulk.BulkMemoryTests.test_protocol_patch_applies_zero_fuzz_and_matches_canonical_headers':
        'Gamescope patches are deferred',
    'test_staging.StagingTests.test_normal_integration_and_unchanged_output_sync':
        'Gamescope patches are deferred',
    'test_wait.WaitAttributionTests.test_real_source_sites_and_normal_only_gate':
        'Gamescope patches are deferred',
    'test_upload_ring.UploadRingTests.test_patch_header_parity_all_writer_hooks_and_normal_only_registration':
        'Gamescope patches are deferred',
    'test_frame_cost.FrameCostTests.test_patch_parity_and_no_rendering_or_rpc_operations_added':
        'Gamescope patches are deferred',
}
DEFERRED_PANVK = {
    'test_artifact.ArtifactTests.test_phone_adb_trigger_is_shell_protected_and_not_implicitly_started':
        'Android probe receiver is deferred',
}


def cases(suite):
    for item in suite:
        if isinstance(item, unittest.TestSuite):
            yield from cases(item)
        else:
            yield item


def run_suite(directory, names, deferred):
    sys.path.insert(0, str(directory))
    try:
        suite = unittest.defaultTestLoader.loadTestsFromNames(names)
        tests = list(cases(suite))
        missing = set(deferred) - {test.id() for test in tests}
        if missing:
            raise RuntimeError('Unknown deferred contracts: ' + ', '.join(sorted(missing)))
        for name, reason in deferred.items():
            print(f'Deferred: {name}: {reason}', flush=True)
        selected = unittest.TestSuite(test for test in tests if test.id() not in deferred)
        result = unittest.TextTestRunner(verbosity=2).run(selected)
        return result.wasSuccessful()
    finally:
        sys.path.pop(0)
        for name, module in list(sys.modules.items()):
            filename = getattr(module, '__file__', None)
            if filename and Path(filename).resolve().is_relative_to(directory):
                del sys.modules[name]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    modes = parser.add_mutually_exclusive_group()
    modes.add_argument('--panvk-only', action='store_true')
    modes.add_argument('--with-loader', action='store_true',
                       help='Also compile small host mocks using Vulkan headers, libvulkan and JNI headers')
    args = parser.parse_args()
    if args.panvk_only:
        if not run_suite(HERE, ('test_steam_angle_probe',), {}):
            return 1
    elif not run_suite(HERE, MALI_TESTS + (LOADER_TESTS if args.with_loader else ()), DEFERRED_MALI):
        return 1
    return 0 if run_suite(HERE.parent / 'panvk', PANVK_TESTS, DEFERRED_PANVK) else 1


if __name__ == '__main__':
    sys.exit(main())
