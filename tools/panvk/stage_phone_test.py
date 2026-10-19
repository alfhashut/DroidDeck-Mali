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


def guest_helper_uploads(helper, directory, angle_dir, atom_stride=None, steam_client=None):
    options = []
    if angle_dir is not None:
        options += ['--angle-dir', angle_dir]
    if atom_stride is not None:
        options += ['--atom-stride', atom_stride]
    if steam_client is not None:
        options += ['--steam-client', steam_client]
    if not options:
        return [(helper, helper.name)]
    # The installed ADB receiver executes a fixed script without arguments.
    # Pass this run's option through a pushed launcher, without changing the APK
    # or the actual guest helper. The next transfer without an override restores
    # the original script and its default auto-detection.
    implementation = 'run_panvk_phone_test_impl.py'
    launcher = directory / helper.name
    launcher.write_text(
        '#!/usr/bin/env python3\n'
        'import runpy, sys\n'
        'from pathlib import Path\n'
        f'sys.argv[1:1] = {options!r}\n'
        f'runpy.run_path(str(Path(__file__).with_name({implementation!r})), run_name="__main__")\n')
    return [(helper, implementation), (launcher, helper.name)]


def run_phone(adb, external, steam_client=None, x11_diagnostics=False, cef_diagnostics=False, perf_diagnostics=False, perf_output=None):
    request = uuid.uuid4().hex
    receiver = 'com.droiddeck.launcher/.gpu.PanvkClientProbeReceiver'
    broadcast = subprocess.run(adb + ['shell', shlex.join([
        'am', 'broadcast', '-n', receiver, '-a', 'com.droiddeck.launcher.PANVK_CLIENT_PROBE',
        '--es', 'request_id', request])], text=True, capture_output=True, check=True)
    if 'result=1' not in broadcast.stdout:
        raise RuntimeError('PanVK trigger rejected/not installed. Install the CI APK containing this helper and '
                           'start normal Mali Steam Launch first. ' + broadcast.stdout + broadcast.stderr)
    report = external + '/Download/DroidDeck/panvk-client-probe-' + request + '.log'
    label = ('Sampling live Steam performance; report=' if perf_diagnostics else
             'Collecting installed Steam CEF argv diagnostics; report=' if cef_diagnostics else
             'Collecting X11 presentation diagnostics; report=' if x11_diagnostics else
             'Configuring development Steam client; report=' if steam_client else 'Running client-only proof; report=')
    print(label + report, flush=True)
    deadline = time.monotonic() + 85
    output = ''
    while time.monotonic() < deadline:
        result = subprocess.run(adb + ['shell', shlex.join(['cat', report])],
                                text=True, capture_output=True, timeout=10)
        if result.returncode == 0:
            output = result.stdout
            if 'DROIDDECK_PANVK_PROBE_DONE exit=' in output:
                if perf_output is not None:
                    (perf_output / 'guest-report.txt').write_text(output)
                print(output, end='')
                marker = ('DROIDDECK_PERF_DIAGNOSTICS_DONE' if perf_diagnostics else
                          'DROIDDECK_CEF_DIAGNOSTICS_DONE' if cef_diagnostics else
                          'DROIDDECK_X11_DIAGNOSTICS_DONE' if x11_diagnostics else
                          'DROIDDECK_STEAM_CLIENT_PANVK_CONFIGURED enabled' if steam_client else
                          'C8B.1 PanVK CLIENT-ONLY PROBE PASS')
                success = ('DROIDDECK_PANVK_PROBE_DONE exit=0' in output.splitlines() and marker in output)
                if x11_diagnostics or cef_diagnostics or perf_diagnostics:
                    print(('Performance sample ' if perf_diagnostics else
                           'CEF argv audit ' if cef_diagnostics else 'X11 snapshot ') +
                          ('collected; no GPU/UI success claim' if success else 'FAILED'))
                elif steam_client:
                    print('Steam client PanVK configuration ' + ('enabled; real UI acceleration UNPROVEN' if success else 'FAIL'))
                else:
                    print('DroidDeck PanVK phone test ' + ('PASS' if success else 'FAIL'))
                return 0 if success else 1
        time.sleep(1)
    print(output, end='')
    raise RuntimeError('timed out waiting for client proof; inspect ' + report)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('artifact', type=Path, nargs='?', help='Actions download ZIP, raw-test tar.gz, or extracted raw directory (not needed for --steam-client disable or diagnostic modes)')
    parser.add_argument('--adb', default=os.environ.get('ADB', 'adb'))
    parser.add_argument('--serial')
    parser.add_argument('--angle-dir', help='Guest Steam ANGLE pair directory; omitted means guest auto-detection')
    parser.add_argument('--atom-stride', choices=('auto', '56', '64'),
                        help='Development GPU probe only: force PANVK_ATOM_STRIDE=56/64; auto leaves it unset')
    parser.add_argument('--steam-client', choices=('enable', 'disable'),
                        help='Development Steam integration: enable configures the next Launch with stride 56; disable restores C8A without a running session')
    parser.add_argument('--x11-diagnostics', action='store_true',
                        help='Snapshot live X11 visual/depth, Composite pixmap and GLX FBConfig compatibility; no artifact or GPU draw')
    parser.add_argument('--cef-diagnostics', action='store_true',
                        help='Read-only audit of installed Steam ARM64 CEF option strings and exact live process argv; no artifact or graphics calls')
    parser.add_argument('--perf-diagnostics', action='store_true',
                        help='Development-only 25-second CPU/XDamage/CEF trace sample of the running Steam UI; no renderer changes or artifact')
    parser.add_argument('--swap-diagnostics', action='store_true',
                        help='Development-only GPU/ANGLE swap scopes plus 5-ms per-thread scheduler sampling; no injection or rebuild (default: 10 seconds)')
    parser.add_argument('--frame-diagnostics', action='store_true',
                        help='Development CEF/X11/SurfaceFlinger capture with temporary webhelper 0xff affinity and automatic restoration; no rebuild')
    parser.add_argument('--frame-scheduler', action='store_true',
                        help='Also record system-wide scheduler events during --frame-diagnostics (material observer overhead on PRoot)')
    parser.add_argument('--client-affinity', choices=('0x7c', '0xff', '0xfc'),
                        help='Temporary webhelper-only scheduling experiment during --swap-diagnostics --run; 0x7c observes existing per-thread policy, 0xff/0xfc override it, then restore')
    parser.add_argument('--perf-output', type=Path,
                        help='Laptop directory for performance report, native broker logs and app/thread CPU (default: new temporary directory)')
    parser.add_argument('--perf-seconds', type=int, choices=range(5, 31),
                        help='Performance sample duration, 5–30 seconds (default: 25)')
    parser.add_argument('--run', action='store_true', help='Trigger the ADB-only receiver in the running Mali session and print its result')
    args = parser.parse_args()
    swap = args.swap_diagnostics or args.frame_diagnostics
    performance = args.perf_diagnostics or swap
    diagnostics = args.x11_diagnostics or args.cef_diagnostics or performance
    if args.frame_scheduler and not args.frame_diagnostics:
        parser.error('--frame-scheduler requires --frame-diagnostics')
    if args.frame_diagnostics:
        if not args.run or args.client_affinity not in (None, '0xff'):
            parser.error('--frame-diagnostics requires --run and temporary affinity 0xff')
        args.client_affinity = '0xff'
    if args.client_affinity and not (swap and args.run):
        parser.error('--client-affinity requires --swap-diagnostics --run')
    if sum((args.x11_diagnostics, args.cef_diagnostics, args.perf_diagnostics, args.swap_diagnostics, args.frame_diagnostics)) > 1:
        parser.error('select only one diagnostic mode')
    if args.perf_output and not (performance and args.run):
        parser.error('--perf-output requires --perf-diagnostics or --swap-diagnostics, with --run')
    if args.perf_seconds is not None and not performance:
        parser.error('--perf-seconds requires --perf-diagnostics or --swap-diagnostics')
    if diagnostics and (args.artifact or args.steam_client or args.atom_stride or args.angle_dir):
        parser.error('diagnostics are independent of artifact, renderer and client configuration options')
    if args.steam_client != 'disable' and not diagnostics and args.artifact is None:
        parser.error('artifact is required unless --steam-client disable or a diagnostic mode is used')
    if args.steam_client and args.atom_stride not in (None, '56'):
        parser.error('the Steam client experiment is fixed to atom stride 56')
    if args.steam_client != 'disable' and not diagnostics:
        print('Requested atom stride=' + ('56' if args.steam_client else (args.atom_stride or 'auto')), flush=True)
    try:
        serial = resolve_serial(args.adb, args.serial, log=True)
        adb = [args.adb, '-s', serial]
        external = subprocess.check_output(adb + ['shell', 'readlink -f /sdcard'], text=True).strip()
        if not external.startswith('/') or '\n' in external:
            raise RuntimeError('cannot resolve the phone shared-storage path')
        remote = external + '/Download/DroidDeckPanVKTest'
        if args.steam_client == 'disable':
            subprocess.run(adb + ['shell', shlex.join(['rm', '-f', remote + '/steam-client-panvk.json'])], check=True)
            print('Steam client PanVK acceleration disabled for the next Launch. Stop normally, then Launch to restore C8A.')
            return 0
        with tempfile.TemporaryDirectory(prefix='panvk-phone-transfer-') as directory:
            subprocess.run(adb + ['shell', shlex.join(['mkdir', '-p', remote])], check=True)
            if not diagnostics:
                staging = Path(directory) / 'artifact'
                unpack(args.artifact, staging)
                subprocess.run(adb + ['shell', shlex.join(['mkdir', '-p', remote + '/artifact'])], check=True)
                subprocess.run(adb + ['push', str(staging) + '/.', remote + '/artifact/'], check=True)
            names = ('run_panvk_phone_test.py', 'probe_steam_angle.py')
            diagnostic_script = ('diagnose_steam_perf.py' if performance else
                                 'diagnose_steam_cef.py' if args.cef_diagnostics else 'diagnose_steam_x11.py')
            if diagnostics:
                names += (diagnostic_script,)
            if performance:
                names += ('diagnose_steam_x11.py',)
            if swap:
                names += ('steam_swap_details.py',)
            if args.frame_diagnostics:
                names += ('steam_frame_details.py', 'steam_frame_journal.py')
            if args.client_affinity:
                names += ('steam_affinity.py',)
            for name in names:
                source = TOOLS / 'mali-vulkan' / name
                if diagnostics and name == 'run_panvk_phone_test.py':
                    launcher = Path(directory) / source.name
                    diagnostic_args = ['--seconds', str(args.perf_seconds or (10 if swap else 25))] if performance else []
                    if swap:
                        diagnostic_args += ['--swap-details']
                    if args.frame_diagnostics:
                        diagnostic_args += ['--frame-details']
                    if args.client_affinity:
                        diagnostic_args += ['--client-affinity', args.client_affinity]
                    launcher.write_text('import runpy, sys\nfrom pathlib import Path\n'
                                        f'sys.argv[1:1] = {diagnostic_args!r}\n'
                                        f'runpy.run_path(str(Path(__file__).with_name({diagnostic_script!r})), run_name="__main__")\n')
                    uploads = [(source, 'run_panvk_phone_test_impl.py'), (launcher, source.name)]
                else:
                    uploads = (guest_helper_uploads(source, Path(directory), args.angle_dir, args.atom_stride, args.steam_client)
                               if name == 'run_panvk_phone_test.py' else [(source, name)])
                for local, guest_name in uploads:
                    subprocess.run(adb + ['push', str(local), remote + '/' + guest_name], check=True)
        if args.run:
            if performance:
                from perf_capture import PerformanceCapture
                with PerformanceCapture(adb, args.perf_output,
                     frame_seconds=(args.perf_seconds or 10) if args.frame_diagnostics else None,
                     frame_scheduler=args.frame_scheduler) as capture:
                    return run_phone(adb, external, perf_diagnostics=True, perf_output=capture.directory)
            return run_phone(adb, external, args.steam_client, args.x11_diagnostics, args.cef_diagnostics, args.perf_diagnostics)
        print('Development files transferred. Start normal Mali Steam Launch, then repeat this command with --run.')
        print('Optional, if the existing second-screen Linux terminal is available:')
        print(shlex.join(['python3', '-B', remote + '/run_panvk_phone_test.py']))
        return 0
    except (OSError, RuntimeError, DeviceResolutionError, subprocess.SubprocessError, tarfile.TarError, zipfile.BadZipFile) as error:
        print(f'PanVK phone transfer FAIL: {error}', file=sys.stderr)
        return 1


if __name__ == '__main__':
    sys.exit(main())
