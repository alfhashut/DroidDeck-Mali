"""Client-only PanVK host contracts; never build Android, Gamescope, Mesa or PanVK."""
import argparse
import os
from pathlib import Path

from run_phase1_tests import HERE, run_suite


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--artifact-only', action='store_true', help='Pure Python contracts for the isolated PanVK builder')
    parser.add_argument('--gamescope-source', type=Path)
    args = parser.parse_args()
    if not args.artifact_only and args.gamescope_source is None:
        parser.error('--gamescope-source is required unless --artifact-only is selected')
    if args.gamescope_source:
        os.environ['GAMESCOPE_SOURCE'] = str(args.gamescope_source.resolve())
    names = ('test_panvk_artifact', 'test_steam_angle_probe', 'test_steam_client_panvk',
             'test_steam_cef_evidence', 'test_steam_cef_diagnostics', 'test_phase5')
    if not args.artifact_only:
        names += ('test_steam_launch',)
    if not run_suite(HERE, names, {}):
        return 1
    return 0 if run_suite(HERE.parent / 'panvk', ('test_artifact', 'test_perf'), {}) else 1


if __name__ == '__main__':
    raise SystemExit(main())
