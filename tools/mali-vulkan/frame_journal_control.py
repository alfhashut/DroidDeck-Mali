"""Control the private Mali journal through the shell-protected release-APK receiver."""
import argparse
from pathlib import Path
import re
import shlex
import subprocess
import sys


def command(action, package):
    if not re.fullmatch(r'[A-Za-z]\w*(?:\.[A-Za-z]\w*)+', package):
        raise ValueError('invalid Android package name')
    if action not in ('arm', 'disarm', 'status', 'pull'):
        raise ValueError('unknown journal action')
    return shlex.join(['am', 'broadcast', '-n',
                      package + '/com.droiddeck.launcher.gpu.MaliFrameJournalReceiver',
                      '--es', 'action', 'export' if action == 'pull' else action])


def receive(adb, action, package='com.droiddeck.launcher'):
    result = subprocess.run(adb + ['shell', command(action, package)],
                            capture_output=True, text=True, check=True)
    if not re.search(r'Broadcast completed: result=1\b', result.stdout):
        raise RuntimeError(result.stdout + result.stderr)
    return result.stdout


def archive_path(output):
    match = re.search(r'mali-journal-export=([^"\r\n]+)', output)
    if not match:
        raise RuntimeError('journal receiver returned no archive path')
    return match[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=('arm', 'disarm', 'status', 'pull'))
    parser.add_argument('--package', default='com.droiddeck.launcher')
    parser.add_argument('--serial')
    parser.add_argument('--adb', default='adb')
    parser.add_argument('--output', type=Path, default=Path('mali-frame-journal.zip'))
    args = parser.parse_args()
    sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
    from adb_device import resolve_serial
    adb = [args.adb, '-s', resolve_serial(args.adb, args.serial, log=True)]
    output = receive(adb, args.action, args.package)
    if args.action == 'pull':
        subprocess.run(adb + ['pull', archive_path(output), str(args.output)], check=True)
    else:
        print(output, end='')


if __name__ == '__main__':
    main()
