"""Exercise the Actions fetch step with fake registries, assets and backoff."""
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

import yaml

ROOT = Path(__file__).resolve().parents[2]
ECR = 'public.ecr.aws/docker/library/debian:bullseye'
HUB = 'debian:bullseye'


class DebianImageFetchTest(unittest.TestCase):
    def fetch(self, hosted=False, checksum=True, failures=None, tag_failure=False, load_failure=False):
        workflow = yaml.safe_load((ROOT / '.github/workflows/build.yml').read_text())
        steps = workflow['jobs']['build']['steps']
        script = next(step['run'] for step in steps if step.get('name') == 'Fetch the Debian image the preloads build in')
        build = next(step['run'] for step in steps if step.get('name') == 'Build the x86 preloads FEX guests load (glibc 2.31)')
        self.assertIn(ECR, build)
        with tempfile.TemporaryDirectory(prefix='debian-fetch-') as directory:
            root = Path(directory)
            fake = root / 'fake'
            fake.write_text('''#!/usr/bin/env python3
import json,os,sys
from pathlib import Path
name=Path(sys.argv[0]).name
args=sys.argv[1:]
log=Path(os.environ['FETCH_LOG'])
calls=json.loads(log.read_text()) if log.exists() else []
calls.append([name]+args)
log.write_text(json.dumps(calls))
if name == 'gh':
    if os.environ['HOSTED'] != '1': sys.exit(1)
    Path(args[args.index('-O')+1]).write_bytes(b'hosted image')
elif name == 'sha256sum':
    sys.stdin.read()
    sys.exit(0 if os.environ['CHECKSUM'] == '1' else 1)
elif name == 'zstd':
    sys.stdout.write('saved image')
elif name == 'docker':
    if args[0] == 'pull':
        count=sum(row == ['docker','pull',args[1]] for row in calls)
        sys.exit(1 if count <= json.loads(os.environ['PULL_FAILURES']).get(args[1],0) else 0)
    if args[0] == 'tag' and os.environ['TAG_FAILURE'] == '1': sys.exit(3)
    if args[0] == 'load':
        sys.stdin.read()
        sys.exit(1 if os.environ['LOAD_FAILURE'] == '1' else 0)
''')
            fake.chmod(0o755)
            for name in ('gh', 'sha256sum', 'zstd', 'docker', 'sleep'):
                (root / name).symlink_to(fake)
            log = root / 'calls.json'
            env = dict(os.environ, PATH=str(root) + os.pathsep + os.environ['PATH'],
                       GITHUB_REPOSITORY='test/repo', FETCH_LOG=str(log), HOSTED=str(int(hosted)),
                       CHECKSUM=str(int(checksum)), PULL_FAILURES=json.dumps(failures or {}),
                       TAG_FAILURE=str(int(tag_failure)), LOAD_FAILURE=str(int(load_failure)))
            script = script.replace('/tmp/debian-image.tar.zst', str(root / 'image.tar.zst'))
            result = subprocess.run(['bash', '-c', script], cwd=ROOT, env=env,
                                    capture_output=True, text=True, timeout=5)
            return result, json.loads(log.read_text())

    def test_hosted_asset_and_checksum_remain_first_choice(self):
        result, calls = self.fetch(hosted=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(calls[0][:3], ['gh', 'release', 'download'])
        self.assertIn(['sha256sum', '-c', '-'], calls)
        self.assertIn(['docker', 'load'], calls)
        self.assertFalse(any(row[:2] == ['docker', 'pull'] for row in calls))
        self.assertEqual(calls[-1], ['docker', 'image', 'ls', ECR])

    def test_missing_release_pulls_hub_and_tags_for_the_unchanged_build(self):
        result, calls = self.fetch()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual([row for row in calls if row[:2] == ['docker', 'pull']], [['docker', 'pull', HUB]])
        self.assertIn(['docker', 'tag', HUB, ECR], calls)
        self.assertEqual(calls[-1], ['docker', 'image', 'ls', ECR])

    def test_checksum_mismatch_uses_the_same_registry_fallback(self):
        result, calls = self.fetch(hosted=True, checksum=False)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn(['docker', 'pull', HUB], calls)
        self.assertNotIn(['docker', 'load'], calls)

    def test_transient_hub_failure_preserves_backoff_then_succeeds(self):
        result, calls = self.fetch(failures={HUB: 2})
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual([row for row in calls if row[0] == 'sleep'], [['sleep', '30'], ['sleep', '60']])
        self.assertNotIn(['docker', 'pull', ECR], calls)

    def test_exhausted_hub_retries_try_ecr_next(self):
        result, calls = self.fetch(failures={HUB: 5})
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual([row[2] for row in calls if row[:2] == ['docker', 'pull']], [HUB] * 5 + [ECR])
        self.assertEqual([row[1] for row in calls if row[0] == 'sleep'], ['30', '60', '90', '120'])
        self.assertFalse(any(row[:2] == ['docker', 'tag'] for row in calls))

    def test_both_registries_exhaust_retries_and_fail_cleanly(self):
        result, calls = self.fetch(failures={HUB: 5, ECR: 5})
        self.assertEqual(result.returncode, 1)
        self.assertEqual([row[2] for row in calls if row[:2] == ['docker', 'pull']], [HUB] * 5 + [ECR] * 5)
        self.assertEqual([row[1] for row in calls if row[0] == 'sleep'], ['30', '60', '90', '120'] * 2)
        self.assertIn('::error::could not pull Debian bullseye from any fallback registry', result.stdout)
        self.assertNotIn(['docker', 'image', 'ls', ECR], calls)

    def test_tag_or_hosted_load_failure_remains_fatal(self):
        for options in ({'tag_failure': True}, {'hosted': True, 'load_failure': True}):
            with self.subTest(options=options):
                result, calls = self.fetch(**options)
                self.assertNotEqual(result.returncode, 0)
                self.assertNotIn(['docker', 'image', 'ls', ECR], calls)


if __name__ == '__main__':
    unittest.main()
