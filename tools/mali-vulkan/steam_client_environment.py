"""Locate the isolated Mali Steam client shell without staging or selecting a driver."""
import os
from pathlib import Path
import re


def client_environment(proc=Path('/proc')):
    clients = {}
    for pid in proc.iterdir():
        if not pid.name.isdigit():
            continue
        try:
            if pid.stat().st_uid != os.getuid():
                continue
            argv = (pid / 'cmdline').read_bytes().decode(errors='replace').split('\0')
            if not any(Path(arg).name == 'droiddeck-session' for arg in argv) or 'steam' not in argv:
                continue
            env = dict(item.split('=', 1) for item in
                       (pid / 'environ').read_bytes().decode(errors='strict').split('\0') if '=' in item)
            if (env.get('BL_MALI_STEAM_UI') != '1' or env.get('BL_INSIDE') != '1'
                    or not re.fullmatch(r':\d+(?:\.\d+)?', env.get('DISPLAY', ''))
                    or any(env.get(key) for key in ('MALI_VULKAN_NORMAL_SESSION', 'MALI_VULKAN_BROKER_SOCKET'))):
                continue
            key = tuple(env.get(name, '') for name in ('DISPLAY', 'XDG_RUNTIME_DIR', 'XAUTHORITY'))
            clients[key] = env
        except (OSError, UnicodeError):
            continue
    if len(clients) != 1:
        raise RuntimeError(
                  'need one running Mali Steam Xwayland client; start normal Launch and use its session Terminal '
                  f'(readable client displays={len(clients)}); no guessed DISPLAY or compositor fallback')
    return next(iter(clients.values()))
