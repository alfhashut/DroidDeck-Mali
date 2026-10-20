"""Materialize the actual patch stack for source contracts; never compile Gamescope."""
import atexit
from functools import lru_cache
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

PATCHES = Path(__file__).resolve().parents[1] / 'gamescope/patches'


@lru_cache(maxsize=None)
def source(through=None):
    pristine = os.environ.get('GAMESCOPE_SOURCE')
    if not pristine:
        raise RuntimeError('Set GAMESCOPE_SOURCE to pristine Gamescope 3.16.29')
    temporary = tempfile.TemporaryDirectory(prefix='mali-source-contract-')
    atexit.register(temporary.cleanup)
    target = Path(temporary.name) / 'source'
    shutil.copytree(pristine, target, ignore=shutil.ignore_patterns('.git', 'build', 'builddir'))
    for patch in sorted(PATCHES.glob('*.patch')):
        if through and patch.name > through:
            break
        result = subprocess.run(['patch', '-p1', '--batch', '--fuzz=0', '--no-backup-if-mismatch', '-i', str(patch)],
                                cwd=target, capture_output=True, text=True)
        if result.returncode:
            raise RuntimeError(f'{patch.name}: {result.stdout}{result.stderr}')
    return target
