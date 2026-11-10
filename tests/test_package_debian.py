#!/usr/bin/env python3
"""Exercise the generated maintainer script without touching system files."""
from pathlib import Path
import os
import stat
import shutil
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
    # Redirect the prefix in test copies; no system file is touched.
    prefix = root / 'prefix'
    prefix.mkdir()
    (prefix / 'nekobox_core').write_text('fixture')
    postinst.write_text(text.replace('/opt/nekoray', str(prefix)))
    preinst = root / 'nekoray/DEBIAN/preinst'
    assert stat.S_IMODE(preinst.stat().st_mode) == 0o755
    preinst.write_text(preinst.read_text().replace('/opt/nekoray', str(prefix)))
    real_find = shutil.which('find')
    stub('find', 'exec "' + real_find + '" "$@"')
    # Supply GNU stat semantics on macOS as well as Linux.
    stub('stat', 'exec "' + shutil.which('python3') + '" -c \'import os,sys; print(os.stat(sys.argv[-1]).st_dev)\' "$@"')
    stub('chown', 'exit 0')
    stub('chmod', 'exit 0')
    env['PATH'] = str(tools)
    for capability, desktop_status, expected in [(0, 0, 0), (127, 0, 1), (0, 1, 1)]:
        stub('setcap', 'exit ' + str(capability))
        stub('update-desktop-database', 'exit ' + str(desktop_status))
        result = subprocess.run(['/bin/sh', str(postinst), 'configure'], env=env, capture_output=True, text=True, timeout=15)
        assert result.returncode == expected, result
        if capability:
            assert 'WARNING:' in result.stderr and 'TUN is unavailable' in result.stderr
        print('setcap=%s desktop=%s -> exit %s' % (capability, desktop_status, result.returncode))
    # Ownership/mode/traversal errors must fail before capability assignment.
    marker = root / 'setcap-called'
    stub('setcap', 'echo called > "' + str(marker) + '"')
    for command in ('chown', 'chmod', 'find', 'stat'):
        original = (tools / command).read_text()
        stub(command, 'exit 1')
        result = subprocess.run(['/bin/sh', str(postinst), 'configure'], env=env, capture_output=True, text=True, timeout=15)
        assert result.returncode != 0 and not marker.exists(), result
        (tools / command).write_text(original)
    # Both maintainer scripts reject a symlink root, a non-directory root,
    # and symlinks inside the tree without touching their external target.
    sentinel = root / 'sentinel'
    sentinel.write_text('unchanged')
    for script, action in ((preinst, 'upgrade'), (postinst, 'configure')):
        (prefix / 'nekobox_core').unlink()
        prefix.rmdir()
        for kind in ('symlink-root', 'file-root', 'symlink-child', 'hardlink-child', 'fifo-child'):
            if kind == 'symlink-root':
                prefix.symlink_to(sentinel)
            elif kind == 'file-root':
                prefix.write_text('not a directory')
            else:
                prefix.mkdir()
                if kind == 'symlink-child':
                    (prefix / 'link').symlink_to(sentinel)
                elif kind == 'hardlink-child':
                    os.link(sentinel, prefix / 'link')
                else:
                    os.mkfifo(prefix / 'link')
            result = subprocess.run(['/bin/sh', str(script), action], env=env, capture_output=True, text=True, timeout=15)
            assert result.returncode != 0 and 'refusing' in result.stderr, result
            assert sentinel.read_text() == 'unchanged' and not marker.exists()
            if kind in ('symlink-child', 'hardlink-child', 'fifo-child'):
                (prefix / 'link').unlink()
                prefix.rmdir()
            else:
                prefix.unlink()
        prefix.mkdir()
        (prefix / 'nekobox_core').write_text('fixture')
    # A device change detected during traversal also fails before setcap.
    original_stat = (tools / 'stat').read_text()
    stub('stat', 'if [ "$4" = "' + str(prefix) + '" ] && [ "$#" -eq 4 ]; then echo 1; else echo 2; fi')
    result = subprocess.run(['/bin/sh', str(postinst), 'configure'], env=env, capture_output=True, text=True, timeout=15)
    assert result.returncode != 0 and 'nested mount' in result.stderr and not marker.exists(), result
    (tools / 'stat').write_text(original_stat)
    stub('update-desktop-database', 'exit 0')
    subprocess.run(['/bin/sh', str(postinst), 'configure'], env=env, check=True, timeout=15)
    assert marker.exists(), 'capability setup must follow successful ownership repair'
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
