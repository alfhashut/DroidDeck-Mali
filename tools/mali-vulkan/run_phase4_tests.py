"""Run final normal-path host contracts; never build Android, drivers or Gamescope."""
import argparse
import os
from pathlib import Path
import sys

from run_phase1_tests import HERE, run_suite


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--gamescope-source', type=Path, required=True)
    parser.add_argument('--vulkan-headers', type=Path)
    args = parser.parse_args()
    os.environ['GAMESCOPE_SOURCE'] = str(args.gamescope_source.resolve())
    if args.vulkan_headers:
        os.environ['VULKAN_HEADERS'] = str(args.vulkan_headers.resolve())
    names = ('test_bulk', 'test_registry', 'test_staging', 'test_upload_ring', 'test_wait',
             'test_frame_cost', 'test_perf', 'test_frame_trace', 'test_upload_trace',
             'test_steam_launch', 'test_steam_cef_diagnostics', 'test_steam_x11_diagnostics', 'test_phase4')
    if not run_suite(HERE, names, {}):
        return 1
    return 0 if run_suite(HERE.parent / 'panvk', ('test_frames', 'test_shm_decomposition',
                                               'test_upload_queue', 'test_upload_sampling'), {}) else 1


if __name__ == '__main__':
    sys.exit(main())
