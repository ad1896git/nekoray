#!/usr/bin/env python3
"""Actual packaging entry points must reject incomplete inputs before side effects."""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import zipfile

from license_fixture import REPO, fixture, run
sys.path.insert(0, str(REPO / 'libs'))
from validate_licenses import validate_package

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--macos-real', action='store_true', help='Also sign, create and inspect a fixture DMG on macOS')
args = parser.parse_args()

(REPO / 'build-core').mkdir(exist_ok=True)
with tempfile.TemporaryDirectory(prefix='notice-tests-', dir=REPO / 'build-core') as tmp:
    root = Path(tmp)
    payloads = {}
    for platform in ('linux', 'windows', 'macos'):
        payload, source, env = fixture(root, platform)
        payloads[platform] = payload
        validate_package(payload, platform)
        print('PASS complete set:', platform, flush=True)
        # AUTHORS, PATENTS, COPYRIGHT and NOTICE are retained, but are not grants.
        (source / 'dependency/LICENSE').unlink()
        for name in ('PATENTS', 'COPYRIGHT', 'NOTICE'):
            (source / 'dependency' / name).write_text('Supplementary fixture attribution\n')
        result = subprocess.run(['python3', str(REPO / 'libs/collect_go_licenses.py'), str(root / ('bad-' + platform))],
                                cwd=source, env=env, capture_output=True, text=True, timeout=180)
        assert result.returncode and 'Missing license documents (LICENSE/COPYING)' in result.stderr, result
        print('PASS collector rejects AUTHORS/PATENTS/COPYRIGHT/NOTICE-only module:', platform, flush=True)

    app = root / 'Fixture.app'
    (app / 'Contents/MacOS').mkdir(parents=True)
    shutil.copyfile(payloads['macos'] / 'nekobox_core', app / 'Contents/MacOS/nekobox_core')
    commands = {
        'linux zip': ['bash', str(REPO / 'libs/package_zip.sh'), 'linux', str(root / 'linux.zip')],
        'windows zip': ['bash', str(REPO / 'libs/package_zip.sh'), 'windows', str(root / 'windows.zip')],
        'deb': ['bash', str(REPO / 'libs/package_debian.sh'), 'v5.0.1'],
        'AppImage': ['bash', str(REPO / 'libs/package_appimage.sh')],
        'macOS app/DMG': ['bash', str(REPO / 'libs/package_macos.sh'), str(app), str(payloads['macos']), str(root / 'fixture.dmg')],
    }
    for label, command in commands.items():
        platform = 'macos' if label.startswith('macOS') else 'windows' if label.startswith('windows') else 'linux'
        payload = payloads[platform]
        # Positive preflight for precisely the tree passed to this entry point.
        run(['python3', str(REPO / 'libs/validate_licenses.py'), 'package', str(payload), platform])
        print('PASS positive preflight:', label, flush=True)
        # All links are forbidden, even when a target exists inside the tree.
        # Directory links exercise parent components; unlisted entries must also fail.
        notice = payload / 'licenses/go/example.com/notice-test/LICENSE'
        original_notice = notice.read_bytes()
        external = root / 'external-license.txt'
        external.write_bytes(original_notice)
        defects = ['in-tree symlink', 'external symlink', 'dangling symlink']
        if hasattr(os, 'mkfifo'):
            defects.append('fifo')
        for defect in defects:
            notice.unlink()
            try:
                if defect == 'fifo':
                    os.mkfifo(notice)
                else:
                    target = 'AUTHORS' if defect == 'in-tree symlink' else str(external) if defect == 'external symlink' else 'absent-license'
                    notice.symlink_to(target)
                result = subprocess.run(command, cwd=root, capture_output=True, text=True, timeout=30)
                assert result.returncode and 'Notice validation failed:' in result.stderr and ('symlink' in result.stderr or 'regular file' in result.stderr), (label, defect, result)
                print('PASS', label, 'rejects', defect, '->', result.stderr.strip(), flush=True)
            finally:
                notice.unlink()
                notice.write_bytes(original_notice)
        module = notice.parent
        saved_module = module.with_name('saved-notice-test')
        for target_kind in ('in-tree', 'external', 'dangling'):
            module.rename(saved_module)
            external_dir = root / 'external-notices'
            if target_kind == 'external':
                shutil.copytree(saved_module, external_dir)
            try:
                target = saved_module.name if target_kind == 'in-tree' else str(external_dir) if target_kind == 'external' else 'absent-notices'
                module.symlink_to(target, target_is_directory=True)
                result = subprocess.run(command, cwd=root, capture_output=True, text=True, timeout=30)
                assert result.returncode and 'Notice validation failed:' in result.stderr and 'symlink' in result.stderr, (label, target_kind, result)
                print('PASS', label, 'rejects', target_kind, 'directory symlink', flush=True)
            finally:
                module.unlink()
                saved_module.rename(module)
                if external_dir.exists():
                    shutil.rmtree(external_dir)
        extra = payload / 'licenses/unlisted-link'
        extra.symlink_to(external)
        try:
            result = subprocess.run(command, cwd=root, capture_output=True, text=True, timeout=30)
            assert result.returncode and 'Notice validation failed:' in result.stderr and 'unlisted-link' in result.stderr, result
            print('PASS', label, 'rejects unlisted symlink', flush=True)
        finally:
            extra.unlink()
        paths = [payload / 'licenses/go/example.com/notice-test/LICENSE',
                 payload / 'licenses/go/example.com/notice-test/SOURCE-NOTICES.txt',
                 payload / 'licenses/go/Go-LICENSE', payload / 'licenses/go/Go-PATENTS',
                 payload / 'LICENSE', payload / 'THIRD-PARTY-NOTICES.txt',
                 payload / 'licenses/common/vendored-source-notices.txt', next((payload / 'licenses' / platform).glob('qtbase-*'))]
        if platform == 'macos':
            paths = paths[:4]  # Native/project notices are read from the repository, tested below.
        for path in paths:
            original = path.read_bytes()
            for defect in ('missing', 'empty'):
                if defect == 'missing':
                    path.unlink()
                else:
                    path.write_bytes(b'')
                result = subprocess.run(command, cwd=root, capture_output=True, text=True, timeout=30)
                path.write_bytes(original)
                assert result.returncode and 'Notice validation failed:' in result.stderr and path.name in result.stderr, (label, path, result)
                print('PASS', label, 'rejects', defect, path.relative_to(payload), '->', result.stderr.strip(), flush=True)
        # Retaining only inventory.json reproduces the signed-DMG defect.
        go = payload / 'licenses/go'
        saved = payload / 'licenses/go-saved'
        go.rename(saved)
        go.mkdir()
        shutil.copyfile(saved / 'inventory.json', go / 'inventory.json')
        result = subprocess.run(command, cwd=root, capture_output=True, text=True, timeout=30)
        shutil.rmtree(go)
        saved.rename(go)
        assert result.returncode and 'Notice validation failed:' in result.stderr, result
        print('PASS', label, 'rejects inventory-only Go tree', flush=True)
        inventory = go / 'inventory.json'
        original = inventory.read_bytes()
        inventory.write_text('[]\n')
        result = subprocess.run(command, cwd=root, capture_output=True, text=True, timeout=30)
        inventory.write_bytes(original)
        assert result.returncode and 'Empty Go notice inventory' in result.stderr, result
        print('PASS', label, 'rejects empty module inventory', flush=True)
        rows = json.loads(original)
        inventory.write_text(json.dumps([dict(rows[0], module='example.com/wrong-module')]))
        result = subprocess.run(command, cwd=root, capture_output=True, text=True, timeout=30)
        inventory.write_bytes(original)
        assert result.returncode and 'does not match compiled modules' in result.stderr, result
        print('PASS', label, 'rejects inventory/compiled-module mismatch', flush=True)
        inventory.write_text(json.dumps([dict(rows[0], version='v99.0.0')]))
        result = subprocess.run(command, cwd=root, capture_output=True, text=True, timeout=30)
        inventory.write_bytes(original)
        assert result.returncode and 'source/version differs' in result.stderr, result
        print('PASS', label, 'rejects wrong compiled-module version', flush=True)

    assert not any((root / name).exists() for name in ('linux.zip', 'windows.zip', 'fixture.dmg', 'nekoray', 'nekobox.AppDir'))
    # Inject only after a real copy/move to prove the final staging gate runs.
    # The original source is complete, so source-only validation cannot pass this test.
    def staged_rejection(label, command, platform, tool, trigger, staged_notice):
        case = root / ('staging-' + label.replace('/', '-').replace(' ', '-'))
        case.mkdir()
        payload = payloads[platform]
        shutil.copytree(payload, case / payload.name)
        shims = case / 'tools'
        shims.mkdir()
        shim = shims / tool
        shim.write_text('#!' + sys.executable + '\n' +
                        'import pathlib, subprocess, sys\n' +
                        'subprocess.run([' + repr(shutil.which(tool)) + '] + sys.argv[1:], check=True)\n' +
                        'if ' + trigger + ':\n' +
                        '    notice = pathlib.Path(' + staged_notice + ')\n' +
                        '    notice.unlink()\n' +
                        '    notice.symlink_to(' + repr(str(root / 'external-license.txt')) + ')\n')
        shim.chmod(0o755)
        env = dict(os.environ, PATH=str(shims) + os.pathsep + os.environ['PATH'])
        result = subprocess.run(command, cwd=case, env=env, capture_output=True, text=True, timeout=180)
        assert result.returncode and 'Notice validation failed:' in result.stderr and 'symlink' in result.stderr, (label, result)
        assert not any(case.glob('*.zip')) and not any(case.glob('*.deb')) and not any(case.glob('*.AppImage'))
        print('PASS', label, 'rejects symlink injected after final staging ->', result.stderr.strip(), flush=True)

    for label in ('linux zip', 'windows zip', 'deb', 'AppImage'):
        platform = 'windows' if label.startswith('windows') else 'linux'
        tool = 'cp' if label == 'AppImage' else 'mv'
        destination = 'nekobox.AppDir' if label == 'AppImage' else 'nekoray/usr/share/doc/nekoray/licenses' if label == 'deb' else 'nekoray'
        suffix = '/go/example.com/notice-test/LICENSE' if label == 'deb' else '/licenses/go/example.com/notice-test/LICENSE'
        staged_rejection(label, commands[label], platform, tool, 'sys.argv[-1] == ' + repr(destination), repr(destination + suffix))

    # Exercise real ZIP creation with complete fixtures after all rejection cases.
    for platform in ('linux', 'windows'):
        case = root / ('positive-' + platform)
        case.mkdir()
        shutil.copytree(payloads[platform], case / payloads[platform].name)
        output = case / 'complete.zip'
        run(['bash', str(REPO / 'libs/package_zip.sh'), platform, str(output)], cwd=case, capture_output=True)
        with zipfile.ZipFile(output) as archive:
            assert archive.read('nekoray/licenses/go/example.com/notice-test/LICENSE')
        print('PASS real complete ZIP:', platform, flush=True)
    # Source-side native manifest: remove a listed Qt notice in isolated script copies.
    isolated = root / 'isolated'
    shutil.copytree(REPO / 'libs', isolated / 'libs', ignore=shutil.ignore_patterns('deps', '__pycache__'))
    shutil.copyfile(REPO / 'LICENSE', isolated / 'LICENSE')
    for platform in ('linux', 'windows', 'macos'):
        path = next((isolated / 'libs/licenses' / platform).glob('qtbase-*'))
        path.unlink()
        result = subprocess.run(['bash', str(isolated / 'libs/package_licenses.sh'), str(root / 'bad-native'), platform],
                                capture_output=True, text=True, timeout=30)
        assert result.returncode and path.name in result.stderr, result
        print('PASS native helper rejects missing manifest entry:', platform, flush=True)

    if args.macos_real:
        assert sys.platform == 'darwin'
        (app / 'Contents/Info.plist').write_text('''<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict><key>CFBundleExecutable</key><string>nekobox_core</string>
<key>CFBundleIdentifier</key><string>test.nekobox.notices</string><key>CFBundlePackageType</key><string>APPL</string></dict></plist>''')
        (app / 'Contents/MacOS/nekobox_core').chmod(0o755)
        run(commands['macOS app/DMG'], cwd=root)
        run(['codesign', '--verify', '--deep', '--strict', str(app)])
        mount = root / 'mount'
        run(['hdiutil', 'attach', '-readonly', '-nobrowse', '-noautoopen', '-mountpoint', str(mount), str(root / 'fixture.dmg')])
        try:
            packaged = mount / 'Fixture.app'
            resources = packaged / 'Contents/Resources'
            run(['python3', str(REPO / 'libs/validate_licenses.py'), 'native', str(resources), 'macos'])
            run(['python3', str(REPO / 'libs/validate_licenses.py'), 'go', str(resources), 'macos', '--binary', str(packaged / 'Contents/MacOS/nekobox_core')])
            run(['codesign', '--verify', '--deep', '--strict', str(packaged)])
        finally:
            run(['hdiutil', 'detach', str(mount)])
        print('PASS real macOS fixture app signature and read-only DMG notices', flush=True)
        staged_rejection('macOS app/DMG', commands['macOS app/DMG'], 'macos', 'ditto',
                         'pathlib.Path(sys.argv[1]).name == "Fixture.app"',
                         'sys.argv[-1] + "/Contents/Resources/licenses/go/example.com/notice-test/LICENSE"')

print('PASS all five packaging paths reject incomplete collections; complete sets pass', flush=True)
