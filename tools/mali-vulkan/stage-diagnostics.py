"""Stage only software-client observers and pure analysis tools into runtime assets."""
import argparse
from pathlib import Path
import shutil

ROOT = Path(__file__).resolve().parents[2]
MALI = ('diagnose_steam_perf.py', 'diagnose_steam_cef.py', 'diagnose_steam_x11.py',
        'steam_affinity.py', 'steam_frame_details.py', 'steam_frame_journal.py',
        'steam_swap_details.py', 'steam_client_environment.py', 'capture_frame_journal.py')
ANALYZERS = ('analyze_frame_journal.py', 'analyze_steam_frames.py', 'summarize_affinity.py')


def stage(output):
    target = output / 'usr/local/share/droiddeck/mali-vulkan'
    target.mkdir(parents=True, exist_ok=True)
    for directory, names in (('mali-vulkan', MALI), ('panvk', ANALYZERS)):
        for name in names:
            shutil.copyfile(ROOT / 'tools' / directory / name, target / name)
    return target


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=ROOT / 'app/src/main/assets/linuxfs')
    stage(parser.parse_args().output)
