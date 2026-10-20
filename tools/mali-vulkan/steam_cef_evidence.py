"""Live Steam/CEF renderer evidence; requests and mapped libraries alone never pass."""
import argparse
import importlib.machinery
import importlib.util
import json
import os
from pathlib import Path
import re
import time

from probe_steam_angle import file_identity, SOFTWARE


def process(pid, proc=Path('/proc')):
    directory = proc / str(pid)
    if directory.stat().st_uid != os.getuid():
        raise RuntimeError('process belongs to another UID')
    status = (directory / 'status').read_text()
    mappings = []
    for row in (directory / 'maps').read_text().splitlines():
        fields = row.split(maxsplit=5)
        if len(fields) == 6 and 'x' in fields[1] and fields[5].startswith('/'):
            major, minor = (int(value, 16) for value in fields[3].split(':'))
            mappings.append(dict(path=fields[5], identity=[major, minor, int(fields[4])]))
    env = dict(item.split('=', 1) for item in (directory / 'environ').read_bytes()
               .decode(errors='replace').split('\0') if '=' in item)
    return dict(pid=pid, ppid=int(re.search(r'^PPid:\s*(\d+)', status, re.M)[1]),
                argv=(directory / 'cmdline').read_bytes().decode(errors='replace').rstrip('\0').split('\0'),
                environment={name: env.get(name) for name in ('DISPLAY', 'DROIDDECK_STEAM_CLIENT_PANVK_RUN',
                    'BL_CDP_PORT', 'PANVK_ATOM_STRIDE', 'ANGLE_DEFAULT_PLATFORM', 'VK_DRIVER_FILES',
                    'MALI_VULKAN_NORMAL_SESSION', 'MALI_VULKAN_BROKER_SOCKET')},
                mappings=mappings)


def descendant(pid, parent, proc):
    seen = set()
    while pid > 1 and pid not in seen and len(seen) < 256:
        if pid == parent:
            return True
        seen.add(pid)
        directory = proc / str(pid)
        if directory.stat().st_uid != os.getuid():
            return False
        pid = int(re.search(r'^PPid:\s*(\d+)', (directory / 'status').read_text(), re.M)[1])
    return False


def evaluate(evidence):
    steam = evidence['steam']
    gpu = evidence['gpu']
    attributes = gpu.get('auxAttributes', {})
    renderer = str(attributes.get('glRenderer', ''))
    active = (renderer + ' ' + str(attributes.get('glVendor', ''))).lower()
    implementation = str(attributes.get('glImplementationParts', '')).lower()
    helpers = evidence['helpers']
    gpu_ids = set(evidence['gpu_process_ids'])
    expected = evidence['expected_libraries']
    correlated = [helper for helper in helpers if helper['pid'] in gpu_ids]
    libraries = any(all(any(mapping['identity'] == identity and not mapping['path'].endswith(' (deleted)')
                            for mapping in helper['mappings']) for identity in expected.values())
                    for helper in correlated)
    client_environment = any(
        helper['environment'].get('PANVK_ATOM_STRIDE') == '56' and
        helper['environment'].get('ANGLE_DEFAULT_PLATFORM') == 'vulkan' and
        helper['environment'].get('VK_DRIVER_FILES') == evidence['expected_icd'] and
        not any(helper['environment'].get(key) for key in ('MALI_VULKAN_NORMAL_SESSION', 'MALI_VULKAN_BROKER_SOCKET'))
        for helper in correlated)
    coherent = any(
        helper['environment'].get('PANVK_ATOM_STRIDE') == '56' and
        helper['environment'].get('ANGLE_DEFAULT_PLATFORM') == 'vulkan' and
        helper['environment'].get('VK_DRIVER_FILES') == evidence['expected_icd'] and
        not any(helper['environment'].get(key) for key in ('MALI_VULKAN_NORMAL_SESSION', 'MALI_VULKAN_BROKER_SOCKET')) and
        all(any(mapping['identity'] == identity and not mapping['path'].endswith(' (deleted)')
                for mapping in helper['mappings']) for identity in expected.values()) for helper in correlated)
    checks = dict(
        steam_cef_use_vulkan='-cef-use-vulkan' in steam['argv'],
        legacy_cef_flags_absent=not any(arg.startswith(('-cef-use-gl', '-cef-use-angle', '--use-gl', '--use-angle'))
                                        or arg in ('-no-linux-angle', '-cef-disable-gpu') for arg in steam['argv']),
        live_gpu_child=bool(correlated),
        helper_angle_vulkan=any('--use-angle=vulkan' in helper['argv'] for helper in helpers),
        renderer_panvk_mali_g52=all(token in active for token in ('angle', 'vulkan', 'panvk', 'mali-g52')),
        software_renderer_absent=not any(token in active for token in SOFTWARE),
        angle_vulkan=bool(re.search(r'\bangle=vulkan\b', implementation)),
        display_angle_vulkan=attributes.get('displayType') == 'ANGLE_VULKAN',
        vulkan_enabled_on=gpu.get('featureStatus', {}).get('vulkan') == 'enabled_on',
        gpu_maps_selected_panvk_and_bundled_angle=bool(expected) and libraries,
        gpu_client_environment=client_environment,
        same_gpu_child_has_selected_environment_and_libraries=coherent,
    )
    return dict(proven=all(checks.values()), checks=checks,
                meaning='live CEF renderer evidence; standalone readback and physical display timing remain separate')


def find_steam(client, proc=Path('/proc')):
    candidates = []
    for directory in proc.iterdir():
        if not directory.name.isdigit():
            continue
        try:
            row = process(int(directory.name), proc)
            if (row['argv'] and row['argv'][0].endswith('/steamrtarm64/steam') and
                    '-cef-use-vulkan' in row['argv']):
                env = row['environment']
                if env.get('DISPLAY') == client['DISPLAY'] and env.get('DROIDDECK_STEAM_CLIENT_PANVK_RUN') == directory.name:
                    candidates.append(row)
        except (OSError, ValueError, TypeError):
            continue
    if len(candidates) != 1:
        raise RuntimeError('need exactly one live accelerated ARM64 Steam process on the proven client display')
    return candidates[0]


def collect(client, args, gpu, roles, proc=Path('/proc')):
    steam = find_steam(client, proc)
    helpers = []
    for directory in proc.iterdir():
        if not directory.name.isdigit():
            continue
        try:
            row = process(int(directory.name), proc)
            if (any(Path(arg).name == 'steamwebhelper' for arg in row['argv']) and
                    descendant(row['pid'], steam['pid'], proc)):
                helpers.append(row)
        except (OSError, ValueError, TypeError):
            continue
    libraries = dict(panvk=Path(args.panvk_library), egl=Path(args.egl), gles=Path(args.gles))
    identities = {name: file_identity(path.resolve(strict=True)) for name, path in libraries.items()}
    if not all(identities.values()):
        raise RuntimeError('selected library device/inode identities are unavailable')
    return dict(observed_monotonic_ns=time.monotonic_ns(), steam=steam, helpers=helpers, gpu=gpu,
                expected_icd=args.icd,
                gpu_process_ids=[int(row['id']) for row in roles if row.get('type', '').lower() == 'gpu'],
                expected_libraries={name: list(identity) for name, identity in identities.items()})


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--config', type=Path, default=Path.home() / '.config/droiddeck/steam-client-panvk.json')
    args = parser.parse_args()
    try:
        from steam_client_environment import client_environment
        from diagnose_steam_perf import Trace
        client = client_environment()
        loader = importlib.machinery.SourceFileLoader('panvk_client_launcher', '/usr/local/bin/droiddeck-steam-client-panvk')
        spec = importlib.util.spec_from_loader(loader.name, loader)
        launcher = importlib.util.module_from_spec(spec)
        loader.exec_module(launcher)
        root = Path(client.get('HOME', '/root')) / '.local/share/Steam'
        _, selected, _ = launcher.inputs(args.config, root, environment=client)
        steam = find_steam(client)
        port = int(steam['environment']['BL_CDP_PORT'])
        if not 1 <= port <= 65535:
            raise RuntimeError('invalid client CEF debugger port')
        trace = Trace(port)
        try:
            gpu = trace.call('SystemInfo.getInfo').get('gpu', {})
            roles = trace.call('SystemInfo.getProcessInfo').get('processInfo', [])
            evidence = collect(client, selected, gpu, roles)
            if evidence['steam']['pid'] != steam['pid']:
                raise RuntimeError('Steam restarted during evidence collection; retry')
        finally:
            trace.close()
        result = evaluate(evidence)
        attributes = gpu.get('auxAttributes', {})
        print('Steam argv=' + json.dumps(evidence['steam']['argv']), flush=True)
        print('GL_RENDERER=' + str(attributes.get('glRenderer', '<unavailable>')), flush=True)
        print('GL implementation parts=' + str(attributes.get('glImplementationParts', '<unavailable>')), flush=True)
        print('Display type=' + str(attributes.get('displayType', '<unavailable>')), flush=True)
        print('vulkan=' + str(gpu.get('featureStatus', {}).get('vulkan', '<unavailable>')), flush=True)
        print('CEF-PanVK-live-evidence=' + json.dumps(evidence, sort_keys=True), flush=True)
        print('CEF-PanVK-verification=' + json.dumps(result, sort_keys=True), flush=True)
        print('DROIDDECK_CEF_PANVK_' + ('PASS' if result['proven'] else 'UNPROVEN'), flush=True)
        return 0 if result['proven'] else 1
    except (OSError, RuntimeError, ValueError, KeyError, TypeError) as error:
        print('DROIDDECK_CEF_PANVK_UNPROVEN: ' + str(error), flush=True)
        return 1


if __name__ == '__main__':
    raise SystemExit(main())
