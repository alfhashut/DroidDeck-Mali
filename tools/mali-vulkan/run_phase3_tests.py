"""Run normal-session host contracts without compiling Android or Gamescope."""
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
    if args.gamescope_source:
        os.environ['GAMESCOPE_SOURCE'] = str(args.gamescope_source.resolve())
    return 0 if run_suite(HERE, ('test_normal', 'test_normal_launch',
                                'test_session.SessionBrokerTests', 'test_phase3',
                                'test_desktop_startup'), {}) else 1


if __name__ == '__main__':
    sys.exit(main())
