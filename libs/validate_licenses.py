#!/usr/bin/env python3
"""Fail closed on incomplete notices; inspect Go build metadata, never run the core."""
import argparse
import json
from pathlib import Path, PurePosixPath
import re
import subprocess
import sys

REPO = Path(__file__).resolve().parents[1]
LICENSE_NAME = re.compile(r'^(licen[cs]e|copying)([._-].*)?$', re.I)
LIBNEKO = 'github.com/matsuridayo/libneko'
LIBNEKO_SOURCE = 'github.com/ad1896git/libneko'
LIBNEKO_VERSION = 'v0.0.0-20261108075722-45e350171b7b'
CLOUDFLARE = 'github.com/sagernet/cloudflare-tls'
CLOUDFLARE_VERSION = 'v0.0.0-20231208171750-a4483c1b7cd1'
LOCAL_VERSION = 'local pinned source; see SOURCES.txt'
LIBNEKO_NOTICE = (
    'libneko: Golang base helper library for NekoRay & NekoBox.\n'
    'Source: https://github.com/ad1896git/libneko/tree/'
    '45e350171b7b623860dd60ebce61a8e7c78de87e\n'
    'No upstream license document was present at this revision.\n'
    'This notice does not grant or infer a license.\n')


def require(path):
    if not path.is_file() or not path.stat().st_size:
        raise ValueError('Missing or empty required notice: ' + str(path))
    return path


def relative(root, name):
    p = PurePosixPath(name)
    if p.is_absolute() or '..' in p.parts or not p.parts or '\\' in name:
        raise ValueError('Invalid notice path: ' + name)
    return root.joinpath(*p.parts)


def read_json(path):
    return json.loads(require(path).read_text())


def exception(name, source, version):
    if (name, source, version) == (LIBNEKO, LIBNEKO_SOURCE, LIBNEKO_VERSION):
        return ['NOTICE.txt']
    if (name, source, version) == (CLOUDFLARE, CLOUDFLARE, CLOUDFLARE_VERSION):
        return ['LICENSE.txt', 'UPSTREAM.md']
    return []


def native_documents(platform):
    manifest = REPO / 'libs/licenses/inventory.json'
    entries = read_json(manifest)
    documents = {row['document'] for row in entries
                 if row['document'].split('/')[0] in ('common', platform)}
    # These aggregate project/rolling-resource notices have no single upstream ref.
    documents.update(('common/vendored-source-notices.txt', 'common/geodata-attribution.txt'))
    return entries, sorted(documents)


def validate_native(root, platform, source=False):
    entries, documents = native_documents(platform)
    if source:
        require(REPO / 'LICENSE')
        require(root / 'SOURCES.txt')
        require(REPO / 'libs/patches/quic-go-v0.47.0-beta.2-congestion-lock.patch')
        licenses = root
    else:
        require(root / 'LICENSE')
        require(root / 'THIRD-PARTY-NOTICES.txt')
        licenses = root / 'licenses'
        if read_json(licenses / 'runtime-inventory.json') != entries:
            raise ValueError('Runtime notice inventory differs from the source manifest')
        require(licenses / 'quic-go-v0.47.0-beta.2-congestion-lock.patch')
    for name in documents:
        require(relative(licenses, name))


def compiled_modules(binary, platform):
    require(binary)
    result = subprocess.check_output(['go', 'version', '-m', str(binary.resolve())],
                                     text=True, timeout=30)
    modules, settings = {}, {}
    current = None
    for line in result.splitlines():
        fields = line.split()
        if not fields:
            continue
        if fields[0] == 'dep':
            current = fields[1]
            modules[current] = (fields[1], fields[2])
        elif fields[0] == '=>' and current:
            modules[current] = (fields[1], LOCAL_VERSION if fields[2] == '(devel)' else fields[2])
        elif fields[0] == 'build' and len(fields) == 2 and '=' in fields[1]:
            key, value = fields[1].split('=', 1)
            settings[key] = value
    expected = {'macos': 'darwin', 'windows': 'windows', 'linux': 'linux'}[platform]
    if not modules or settings.get('GOOS') != expected:
        raise ValueError('Missing or wrong-target Go build metadata: ' + str(binary))
    # grpc_server is project-owned and covered by the required project LICENSE.
    modules.pop('grpc_server', None)
    return modules


def validate_go(root, binary, platform):
    modules = compiled_modules(binary, platform)
    inventory = read_json(root / 'inventory.json')
    if not isinstance(inventory, list) or not inventory:
        raise ValueError('Empty Go notice inventory')
    indexed = {row['module']: row for row in inventory}
    if len(indexed) != len(inventory) or set(indexed) != set(modules):
        raise ValueError('Go notice inventory does not match compiled modules; missing=' +
                         str(sorted(set(modules) - set(indexed))) + '; extra=' +
                         str(sorted(set(indexed) - set(modules))))
    for name, row in sorted(indexed.items()):
        if (row['source'], row['version']) != modules[name]:
            raise ValueError('Go notice source/version differs from compiled module: ' + name)
        documents = row['documents']
        special = exception(name, row['source'], row['version'])
        if not any(LICENSE_NAME.match(PurePosixPath(p).name) for p in documents) and not special:
            raise ValueError('Missing license document (LICENSE/COPYING) for ' + name)
        generated = row['generated_documents']
        for document in documents + generated + special:
            require(relative(relative(root, name), document))
        if name == LIBNEKO and special:
            if (root / name / 'NOTICE.txt').read_text() != LIBNEKO_NOTICE:
                raise ValueError('libneko exception requires the exact factual pinned NOTICE')
    require(root / 'Go-LICENSE')
    require(root / 'Go-PATENTS')
    print('Validated notices for', len(modules), 'compiled Go modules plus Go (' + platform + ')')


def validate_package(root, platform):
    validate_native(root, platform)
    binary = root / ('nekobox_core.exe' if platform == 'windows' else 'nekobox_core')
    validate_go(root / 'licenses/go', binary, platform)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('mode', choices=('native-source', 'native', 'go', 'package'))
    parser.add_argument('root', type=Path)
    parser.add_argument('platform', choices=('linux', 'windows', 'macos'))
    parser.add_argument('--binary', type=Path)
    args = parser.parse_args()
    try:
        if args.mode in ('native-source', 'native'):
            validate_native(args.root, args.platform, args.mode == 'native-source')
        elif args.mode == 'go':
            validate_go(args.root / 'licenses/go', args.binary or args.root / 'nekobox_core', args.platform)
        else:
            validate_package(args.root, args.platform)
    except (ValueError, KeyError, TypeError, OSError, subprocess.SubprocessError) as error:
        parser.exit(1, 'Notice validation failed: ' + str(error) + '\n')


if __name__ == '__main__':
    main()
