#!/usr/bin/env python3
"""Development-only laptop helper: transfer raw PanVK + probe, never build or install an APK."""
import argparse
from contextlib import ExitStack
from pathlib import Path, PurePosixPath
import os
import shlex
import shutil
import subprocess
import sys
import tarfile
import tempfile
import time
import uuid
import zipfile

TOOLS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS))
from adb_device import resolve_serial, DeviceResolutionError

FILES = ('libvulkan_panfrost.so', 'panfrost_icd.aarch64.json',
         'DEPENDENCIES.txt', 'BUILD_INFO.txt', 'SOURCE_DIFF.patch', 'Mesa-license.rst', 'SHA256SUMS')


def unpack(artifact, output):
    output.mkdir()
    if artifact.is_dir():
        for name in FILES:
            if (artifact / name).is_file():
                shutil.copyfile(artifact / name, output / name)
    else:
        with ExitStack() as stack:
            if zipfile.is_zipfile(artifact):
                archive = stack.enter_context(zipfile.ZipFile(artifact))
                members = [n for n in archive.namelist() if n.endswith('.tar.gz')]
                if len(members) != 1:
                    raise RuntimeError('expected one raw-test .tar.gz inside the Actions ZIP')
                stream = stack.enter_context(archive.open(members[0]))
                tar = stack.enter_context(tarfile.open(fileobj=stream, mode='r|gz'))
            else:
                tar = stack.enter_context(tarfile.open(artifact, mode='r|gz'))
            for member in tar:
                name = PurePosixPath(member.name).name
                if name not in FILES:
                    continue
                if not member.isfile() or (output / name).exists():
                    raise RuntimeError(f'nonregular/duplicate raw artifact member: {member.name}')
                with tar.extractfile(member) as stream, (output / name).open('wb') as target:
                    shutil.copyfileobj(stream, target)
    for name in FILES[:2]:
        if not (output / name).is_file():
            raise RuntimeError(f'raw artifact missing {name}')


def run_phone(adb, external):
    request = uuid.uuid4().hex
    receiver = 'com.droiddeck.launcher/.gpu.PanvkClientProbeReceiver'
    broadcast = subprocess.run(adb + ['shell', shlex.join([
        'am', 'broadcast', '-n', receiver, '-a', 'com.droiddeck.launcher.PANVK_CLIENT_PROBE',
        '--es', 'request_id', request])], text=True, capture_output=True, check=True)
    if 'result=1' not in broadcast.stdout:
        raise RuntimeError('PanVK trigger rejected/not installed. Install the CI APK containing this helper and '
                           'start normal Mali Steam Launch first. ' + broadcast.stdout + broadcast.stderr)
    report = external + '/Download/DroidDeck/panvk-client-probe-' + request + '.log'
    print('Running client-only proof; report=' + report, flush=True)
    deadline = time.monotonic() + 85
    output = ''
    while time.monotonic() < deadline:
        result = subprocess.run(adb + ['shell', shlex.join(['cat', report])],
                                text=True, capture_output=True, timeout=10)
        if result.returncode == 0:
            output = result.stdout
            if 'DROIDDECK_PANVK_PROBE_DONE exit=' in output:
                print(output, end='')
                success = ('DROIDDECK_PANVK_PROBE_DONE exit=0' in output.splitlines()
                           and 'C8B.1 PanVK CLIENT-ONLY PROBE PASS' in output)
                print('DroidDeck PanVK phone test ' + ('PASS' if success else 'FAIL'))
                return 0 if success else 1
        time.sleep(1)
    print(output, end='')
    raise RuntimeError('timed out waiting for client proof; inspect ' + report)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('artifact', type=Path, help='Actions download ZIP, raw-test tar.gz, or extracted raw directory')
    parser.add_argument('--adb', default=os.environ.get('ADB', 'adb'))
    parser.add_argument('--serial')
    parser.add_argument('--run', action='store_true', help='Trigger the ADB-only receiver in the running Mali session and print its result')
    args = parser.parse_args()
    try:
        serial = resolve_serial(args.adb, args.serial, log=True)
        adb = [args.adb, '-s', serial]
        external = subprocess.check_output(adb + ['shell', 'readlink -f /sdcard'], text=True).strip()
        if not external.startswith('/') or '\n' in external:
            raise RuntimeError('cannot resolve the phone shared-storage path')
        remote = external + '/Download/DroidDeckPanVKTest'
        with tempfile.TemporaryDirectory(prefix='panvk-phone-transfer-') as directory:
            staging = Path(directory) / 'artifact'
            unpack(args.artifact, staging)
            subprocess.run(adb + ['shell', shlex.join(['mkdir', '-p', remote + '/artifact'])], check=True)
            subprocess.run(adb + ['push', str(staging) + '/.', remote + '/artifact/'], check=True)
            for name in ('run_panvk_phone_test.py', 'probe_steam_angle.py'):
                subprocess.run(adb + ['push', str(TOOLS / 'mali-vulkan' / name), remote + '/' + name], check=True)
        if args.run:
            return run_phone(adb, external)
        print('Development files transferred. Start normal Mali Steam Launch, then repeat this command with --run.')
        print('Optional, if the existing second-screen Linux terminal is available:')
        print(shlex.join(['python3', '-B', remote + '/run_panvk_phone_test.py']))
        return 0
    except (OSError, RuntimeError, DeviceResolutionError, subprocess.SubprocessError, tarfile.TarError, zipfile.BadZipFile) as error:
        print(f'PanVK phone transfer FAIL: {error}', file=sys.stderr)
        return 1


if __name__ == '__main__':
    sys.exit(main())
