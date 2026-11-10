#!/usr/bin/env python3
"""Exercise the generated maintainer script without touching system files."""
from pathlib import Path
import os
import stat
import subprocess
import tempfile

repo = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory() as tmp:
    root = Path(tmp)
    payload = root / 'linux64'
    payload.mkdir()
    for name in ('LICENSE', 'THIRD-PARTY-NOTICES.txt'):
        (payload / name).write_text(name)
    (payload / 'licenses').mkdir()
    tools = root / 'tools'
    tools.mkdir()
    def stub(name, body):
        p = tools / name
        p.write_text('#!/bin/sh\n' + body + '\n')
        p.chmod(0o755)
    stub('sudo', 'exec "$@"')
    stub('dpkg-deb', 'exit 0')
    env = dict(os.environ, PATH=str(tools) + os.pathsep + os.environ['PATH'])
    subprocess.run(['bash', str(repo / 'libs/package_debian.sh'), 'v5.0.1'], cwd=root, env=env, check=True, timeout=15)
    postinst = root / 'nekoray/DEBIAN/postinst'
    assert stat.S_IMODE(postinst.stat().st_mode) == 0o755
    text = postinst.read_text()
    assert text.startswith('#!/bin/sh\nset -e\n')
    assert 'libcap2-bin' in (root / 'nekoray/DEBIAN/control').read_text()
    assert (root / 'nekoray/usr/share/doc/nekoray/copyright').is_file()
    # Redirect only the desktop path in a test copy; no system file is touched.
    desktop = root / 'nekoray.desktop'
    desktop.write_text('existing desktop\n')
    postinst.write_text(text.replace('/usr/share/applications/nekoray.desktop', str(desktop)))
    env['PATH'] = str(tools)
    for capability, desktop_status, expected in [(0, 0, 0), (127, 0, 1), (0, 1, 1)]:
        stub('setcap', 'exit ' + str(capability))
        stub('update-desktop-database', 'exit ' + str(desktop_status))
        result = subprocess.run(['/bin/sh', str(postinst), 'configure'], env=env, capture_output=True, text=True, timeout=15)
        assert result.returncode == expected, result
        if capability:
            assert 'WARNING:' in result.stderr and 'TUN is unavailable' in result.stderr
        print('setcap=%s desktop=%s -> exit %s' % (capability, desktop_status, result.returncode))
    (tools / 'setcap').unlink()
    result = subprocess.run(['/bin/sh', str(postinst), 'configure'], env=env, capture_output=True, text=True, timeout=15)
    assert result.returncode == 1 and 'WARNING:' in result.stderr
    print('missing setcap -> exit 1 with warning')
    for action in ('abort-upgrade', 'abort-remove', 'abort-deconfigure'):
        subprocess.run(['/bin/sh', str(postinst), action], env=env, check=True, timeout=15)
    print('Debian script generation, modes, dependency and error handling passed')
