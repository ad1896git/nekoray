#!/usr/bin/env python3
"""Generated scripts: real traversal/modes, simulated root UID/GID, no system writes."""
from pathlib import Path
import os
import shlex
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
    write_deb(root / 'nekoray.deb')
    env = dict(os.environ, PATH=str(tools) + os.pathsep + os.environ['PATH'])
    subprocess.run(['bash', str(repo / 'libs/package_debian.sh'), 'v5.0.1'], cwd=root, env=env, check=True, timeout=30)
    control = root / 'nekoray/DEBIAN'
    assert 'libcap2-bin' in (control / 'control').read_text()
    assert (root / 'nekoray/usr/share/doc/nekoray/copyright').is_file()
    assert (root / 'nekoray/usr/share/applications/nekoray.desktop').is_file()
    assert '/usr/share/applications/nekoray.desktop' not in (control / 'postinst').read_text()

    sandbox = root / 'sandbox'
    sandbox.mkdir()
    opt = sandbox / 'opt'
    opt.mkdir()
    prefix = opt / 'nekoray'
    marker = root / 'setcap-called'
    mutation = root / 'forbidden-mutation'
    assert (control / 'postrm').read_text().split('refuse_unsafe() {', 1)[1] == (control / 'preinst').read_text().split('refuse_unsafe() {', 1)[1]
    for name in ('preinst', 'postinst', 'postrm'):
        script = control / name
        assert stat.S_IMODE(script.stat().st_mode) == 0o755
        text = script.read_text()
        assert text.startswith('#!/bin/sh\nset -e\n')
        forbidden = {'chown', 'chmod', 'rm', 'mv'} | ({'setcap'} if name != 'postinst' else set())
        assert not any(Path(t).name in forbidden for t in shlex.split(text, comments=True))
        # Translate only the test copies, including both protected ancestors.
        text = text.replace('/opt/nekoray', '__PREFIX__').replace('/opt', str(opt))
        text = text.replace('for ancestor in / ', 'for ancestor in ' + str(sandbox) + ' ')
        script.write_text(text.replace('__PREFIX__', str(prefix)))
    preinst, postinst, postrm = (control / name for name in ('preinst', 'postinst', 'postrm'))
    inspections = ((preinst, 'upgrade'), (postinst, 'configure'),
                   (postrm, 'abort-upgrade'), (postrm, 'abort-install'), (postrm, 'failed-upgrade'))

    # Native find supplies traversal, lstat, permission, link and type checks.
    # Map root ownership to the caller for unprivileged macOS/Linux fixtures;
    # optional per-path UID/GID overrides test the numeric-owner predicates.
    find_adapter = tools / 'find'
    find_adapter.write_text('#!' + shutil.which('python3') + '\n' + '''import os, sys
args = sys.argv[1:]
for predicate, actual, key in [('-uid', os.getuid(), 'NONROOT_UID_PATH'), ('-gid', os.getgid(), 'NONROOT_GID_PATH')]:
    i = 0
    while i < len(args):
        if args[i:i+3] == ['!', predicate, '0']:
            replacement = ['!', predicate, str(actual)]
            if os.environ.get(key):
                replacement = ['('] + replacement + ['-o', '-path', os.environ[key], ')']
            args[i:i+3] = replacement
            i += len(replacement)
        else:
            i += 1
if os.environ.get('REPLACE_NESTED') and args[1] == os.environ['TEST_PREFIX']:
    nested = os.environ['REPLACE_NESTED']
    if not os.path.islink(nested):
        os.rename(nested, nested + '-saved')
        os.symlink(os.environ['OUTSIDE'], nested)
os.execv(''' + repr(shutil.which('find')) + ''', ['find'] + args)
''')
    find_adapter.chmod(0o755)
    # GNU stat's default is lstat. Supply its device output on both hosts.
    stub('stat', 'exec "' + shutil.which('python3') + '" -c \'import os,sys; [print(os.lstat(p).st_dev) for p in sys.argv[4:]]\' "$@"')
    for command in ('chown', 'chmod', 'rm', 'mv'):
        stub(command, 'echo called > "' + str(mutation) + '"; exit 99')
    stub('setcap', 'echo called > "' + str(marker) + '"')
    stub('update-desktop-database', 'exit 0')
    env['PATH'] = str(tools)

    def run(script, action, extra=None):
        marker.unlink(missing_ok=True)
        result = subprocess.run(['/bin/sh', str(script), action], env=dict(env, **(extra or {})),
                                capture_output=True, text=True, timeout=15)
        assert not mutation.exists(), result
        return result

    def refused(script, action, extra=None):
        result = run(script, action, extra)
        assert result.returncode != 0 and 'refusing unsafe pre-existing install' in result.stderr, result
        assert 'Half-Installed' in result.stderr and 'docs/PACKAGING.md#administrator-recovery-a-prime' in result.stderr
        assert not marker.exists(), result

    # Absent prefix and absent /opt are both normal fresh installs.
    assert run(preinst, 'install').returncode == 0
    assert run(postrm, 'abort-install').returncode == 0
    opt.rmdir()
    assert run(preinst, 'install').returncode == 0
    opt.mkdir()
    prefix.mkdir()
    core = prefix / 'nekobox_core'
    core.write_text('fixture')
    for script, action in inspections:
        for _ in range(2):
            assert run(script, action).returncode == 0
            assert marker.exists() == (script == postinst)
        for key in ('NONROOT_UID_PATH', 'NONROOT_GID_PATH'):
            for path in (sandbox, opt, prefix, core):
                refused(script, action, {key: str(path)})
        for path in (sandbox, opt, prefix, core):
            original = stat.S_IMODE(path.stat().st_mode)
            for bits in (0o020, 0o002):
                path.chmod(original | bits)
                refused(script, action)
            path.chmod(original)
        for command in ('find', 'stat'):
            original = (tools / command).read_text()
            stub(command, 'exit 1')
            refused(script, action)
            (tools / command).write_text(original)

    sentinel_dir = root / 'outside'
    sentinel_dir.mkdir()
    sentinel = sentinel_dir / 'sentinel'
    sentinel.write_text('unchanged')
    sentinel.chmod(0o666)
    before = sentinel.stat()
    for script, action in ((preinst, 'install'),) + inspections:
        core.unlink()
        prefix.rmdir()
        for kind in ('symlink-root', 'file-root', 'symlink-child', 'hardlink-child', 'fifo-child'):
            if kind == 'symlink-root':
                prefix.symlink_to(sentinel_dir)
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
            refused(script, action)
            after = sentinel.stat()
            assert (after.st_uid, after.st_gid, after.st_mode) == (before.st_uid, before.st_gid, before.st_mode)
            assert sentinel.read_text() == 'unchanged'
            if kind.endswith('child'):
                (prefix / 'link').unlink()
                prefix.rmdir()
            else:
                prefix.unlink()
        prefix.mkdir()
        core.write_text('fixture')
        # Synchronized replacement at the traversal boundary: only reads occur,
        # and a writable nested parent plus planted symlink must be refused.
        parent = prefix / 'retained'
        nested = parent / 'nested'
        nested.mkdir(parents=True)
        parent.chmod(0o777)
        (nested / 'sentinel').write_text('local')
        refused(script, action, {'REPLACE_NESTED': str(nested), 'TEST_PREFIX': str(prefix),
                                 'OUTSIDE': str(sentinel_dir)})
        after = sentinel.stat()
        assert (after.st_uid, after.st_gid, after.st_mode) == (before.st_uid, before.st_gid, before.st_mode)
        assert sentinel.read_text() == 'unchanged'
        shutil.rmtree(parent)

    # Nested mount boundary (simulated device only) is rejected.
    original_stat = (tools / 'stat').read_text()
    stub('stat', 'if [ "$#" -eq 4 ]; then echo 1; else echo 2; fi')
    for script, action in inspections:
        refused(script, action)
    (tools / 'stat').write_text(original_stat)

    # Capability setup and desktop-refresh errors retain PR #53 behavior.
    for capability, desktop, expected in ((0, 0, 0), (127, 0, 1), (0, 1, 1)):
        stub('setcap', 'exit ' + str(capability))
        stub('update-desktop-database', 'exit ' + str(desktop))
        result = run(postinst, 'configure')
        assert result.returncode == expected, result
        if capability:
            assert 'WARNING:' in result.stderr and 'TUN is unavailable' in result.stderr
    (tools / 'setcap').unlink()
    result = run(postinst, 'configure')
    assert result.returncode == 1 and 'WARNING:' in result.stderr
    for script, actions in ((preinst, ('abort-upgrade',)),
                            (postinst, ('abort-upgrade', 'abort-remove', 'abort-deconfigure'))):
        for action in actions:
            assert run(script, action).returncode == 0
        assert run(script, 'unexpected').returncode != 0
        assert run(script, '').returncode != 0
    # Removal must remain available even when the installed tree is unsafe.
    prefix.chmod(0o777)
    assert stat.S_IMODE(postrm.stat().st_mode) == 0o755
    stub('update-desktop-database', 'exit 0')
    for action in ('remove', 'purge', 'upgrade', 'disappear'):
        assert run(postrm, action).returncode == 0
    prefix.chmod(0o755)
    for action in ('abort-install', 'abort-upgrade', 'failed-upgrade'):
        assert run(postrm, action).returncode == 0
    assert run(postrm, '').returncode != 0
    assert run(postrm, 'unexpected').returncode != 0
    stub('update-desktop-database', 'exit 1')
    for action in ('remove', 'purge'):
        assert run(postrm, action).returncode == 1
    (tools / 'update-desktop-database').unlink()
    for action in ('remove', 'purge'):
        assert run(postrm, action).returncode == 0
    print('PASS: generated scripts, static no-repair guard, read-only refusal, ancestors, UID/GID, modes, links, synchronized replacement, sentinel metadata, errors and idempotence')
