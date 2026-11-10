"""Small real Go binaries/notice trees for packaging checks (no application execution)."""
import os
from pathlib import Path
import subprocess

REPO = Path(__file__).resolve().parents[1]


def run(command, **kwargs):
    return subprocess.run(command, check=True, timeout=180, **kwargs)


def fixture(root, platform):
    root = Path(root)
    source = root / ('source-' + platform)
    source.mkdir()
    (source / 'dependency').mkdir()
    (source / 'go.mod').write_text('module fixture\n\ngo 1.19\nrequire example.com/notice-test v1.0.0\nreplace example.com/notice-test => ./dependency\n')
    (source / 'main.go').write_text('package main\nimport "example.com/notice-test"\nfunc main() { dependency.Value() }\n')
    (source / 'dependency/go.mod').write_text('module example.com/notice-test\n\ngo 1.19\n')
    (source / 'dependency/value.go').write_text('// Copyright fixture authors.\npackage dependency\nfunc Value() {}\n')
    (source / 'dependency/LICENSE').write_text('Fixture license document for packaging validation.\n')
    (source / 'dependency/AUTHORS').write_text('Fixture authors\n')
    env = dict(os.environ, GOTOOLCHAIN='local', GOWORK='off', CGO_ENABLED='0',
               GOOS={'macos': 'darwin'}.get(platform, platform), GOARCH='amd64')
    payload = root / {'macos': 'core-output', 'linux': 'linux64', 'windows': 'windows64'}[platform]
    payload.mkdir()
    binary = payload / ('nekobox_core.exe' if platform == 'windows' else 'nekobox_core')
    run(['go', 'build', '-trimpath', '-o', str(binary), '.'], cwd=source, env=env)
    run(['python3', str(REPO / 'libs/collect_go_licenses.py'), str(payload)], cwd=source, env=env)
    run(['bash', str(REPO / 'libs/package_licenses.sh'), str(payload), platform])
    return payload, source, env
