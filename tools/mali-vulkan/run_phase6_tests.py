"""Shared CPU transport host contracts; no Android, Gamescope or driver build."""
import argparse
import os
from pathlib import Path

from run_phase1_tests import HERE, run_suite


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--gamescope-source', type=Path, required=True)
    parser.add_argument('--vulkan-headers', type=Path)
    args = parser.parse_args()
    os.environ['GAMESCOPE_SOURCE'] = str(args.gamescope_source.resolve())
    if args.vulkan_headers:
        os.environ['VULKAN_HEADERS'] = str(args.vulkan_headers.resolve())
    if not run_suite(HERE, ('test_shared_staging', 'test_bulk', 'test_registry', 'test_staging',
                            'test_phase6_wire.SharedBrokerTests', 'test_normal_launch.NormalLaunchTests',
                            'test_steam_launch', 'test_phase6', 'test_phase3.PatchContractTests',
                            'test_phase5.Phase5Tests.test_phase4_bridge_sync_staging_and_all_27_patches_remain_byte_identical'), {}):
        return 1
    return 0 if run_suite(HERE.parent / 'panvk', ('test_upload_queue', 'test_upload_sampling'), {}) else 1


if __name__ == '__main__':
    raise SystemExit(main())
