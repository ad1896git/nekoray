#!/usr/bin/env python3
"""Exercise the generated maintainer script without touching system files."""
from pathlib import Path
import os
import stat
import subprocess
import tempfile

from license_fixture import fixture
from test_debian_archive import write_deb

repo = Path(__file__).resolve().parents[1]
(repo / 'build-core').mkdir(exist_ok=True)
with tempfile.TemporaryDirectory(dir=repo / 'build-core') as tmp:
    root = Path(tmp)
    fixture(root, 'linux')
    tools = root / 'tools'
    tools.mkdir()
    def stub(name, body):
        p = tools / name
        p.write_text('#!/bin/sh\n' + body + '\n')
        p.chmod(0o755)
    stub('sudo', 'exec "$@"')
    stub('dpkg-deb', 'exit 0')
    # This test isolates script generation; CI also builds and validates the real deb.
    write_deb(root / 'nekoray.deb')
    env = dict(os.environ, PATH=str(tools) + os.pathsep + os.environ['PATH'])
    subprocess.run(['bash', str(repo / 'libs/package_debian.sh'), 'v5.0.1'], cwd=root, env=env, check=True, timeout=15)
    postinst = root / 'nekoray/DEBIAN/postinst'
    assert stat.S_IMODE(postinst.stat().st_mode) == 0o755
    text = postinst.read_text()
    assert text.startswith('#!/bin/sh\nset -e\n')
    assert 'libcap2-bin' in (root / 'nekoray/DEBIAN/control').read_text()
    assert (root / 'nekoray/usr/share/doc/nekoray/copyright').is_file()
    assert (root / 'nekoray/usr/share/applications/nekoray.desktop').is_file()
    assert '/usr/share/applications/nekoray.desktop' not in text
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
    postrm = root / 'nekoray/DEBIAN/postrm'
    assert stat.S_IMODE(postrm.stat().st_mode) == 0o755
    stub('update-desktop-database', 'exit 0')
    for action in ('remove', 'purge', 'upgrade', 'failed-upgrade', 'abort-install', 'abort-upgrade', 'disappear'):
        subprocess.run(['/bin/sh', str(postrm), action], env=env, check=True, timeout=15)
    stub('update-desktop-database', 'exit 1')
    for action in ('remove', 'purge'):
        result = subprocess.run(['/bin/sh', str(postrm), action], env=env, timeout=15)
        assert result.returncode == 1
    (tools / 'update-desktop-database').unlink()
    for action in ('remove', 'purge'):
        subprocess.run(['/bin/sh', str(postrm), action], env=env, check=True, timeout=15)
    print('Debian script generation, modes, dependency and error handling passed')
