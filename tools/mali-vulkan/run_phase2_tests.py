"""Run small diagnostic host contracts; never build the APK, renderer or drivers."""
import argparse
import os
from pathlib import Path
import sys

from run_phase1_tests import HERE, run_suite


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--gamescope-source', type=Path)
    parser.add_argument('--vulkan-headers', type=Path)
    args = parser.parse_args()
    if args.vulkan_headers:
        os.environ['VULKAN_HEADERS'] = str(args.vulkan_headers.resolve())
    names = ('test_diagnostics', 'test_probe', 'test_icd', 'test_interop',
             'test_renderer.RendererTransportTests')
    if not run_suite(HERE, names, {}):
        return 1
    if args.gamescope_source:
        os.environ['GAMESCOPE_SOURCE'] = str(args.gamescope_source.resolve())
        return 0 if run_suite(HERE.parent / 'gamescope', ('test_vk_enumerate_only',), {}) else 1
    print('Gamescope translation-unit tests require --gamescope-source; not run.')
    return 0


if __name__ == '__main__':
    sys.exit(main())
