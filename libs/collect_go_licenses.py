#!/usr/bin/env python3
"""Run in the core module, with the same GOOS/GOARCH/tags as go build."""
import json
from pathlib import Path
import re
import shutil
import subprocess
import sys

out = Path(sys.argv[1]).resolve() / 'licenses' / 'go'
if out.exists():
    shutil.rmtree(out)
out.mkdir(parents=True, exist_ok=True)
tags = 'with_clash_api,with_gvisor,with_quic,with_wireguard,with_utls,with_ech'
result = subprocess.check_output(['go', 'list', '-mod=readonly', '-deps', '-json', '-tags', tags, '.'], text=True, timeout=180)
decoder = json.JSONDecoder()
modules = {}
headers = {}
while result.strip():
    package, end = decoder.raw_decode(result.lstrip())
    result = result.lstrip()[end:]
    module = package.get('Module')
    if module and not module.get('Main'):
        modules[module['Path']] = module
        for filename in package.get('GoFiles', []):
            source = Path(package['Dir']) / filename
            preamble = source.read_text(errors='replace').split('\npackage ', 1)[0]
            if re.search(r'copyright|licen[cs]e', preamble, re.I):
                headers.setdefault(module['Path'], []).append((package['ImportPath'] + '/' + filename, preamble))
legal = re.compile(r'^(licen[cs]e|copying|copyright|notice|authors|patents)([._-].*)?$', re.I)
inventory = []
for name, module in sorted(modules.items()):
    actual = module.get('Replace', module)
    root = Path(actual['Dir'])
    # grpc_server is project code and uses the accompanying project LICENSE.
    if name == 'grpc_server':
        continue
    files = sorted(p for p in root.rglob('*') if p.is_file() and legal.match(p.name) and '.git' not in p.parts)
    if not files and name == 'github.com/sagernet/cloudflare-tls':
        files = []
        target = out / name
        target.mkdir(parents=True, exist_ok=True)
        supplemental = Path(__file__).resolve().parent / 'licenses' / 'common' / 'cloudflare-go.txt'
        shutil.copyfile(supplemental, target / 'LICENSE.txt')
        shutil.copyfile(root / 'README.md', target / 'UPSTREAM.md')
    elif not files and name != 'github.com/matsuridayo/libneko':
        raise SystemExit('Missing license documents for ' + name)
    if not files and name == 'github.com/matsuridayo/libneko':
        # The pinned libneko upstream has no LICENSE or copyright document.
        # Preserve that fact rather than assigning an unsupported license.
        target = out / name
        target.mkdir(parents=True, exist_ok=True)
        (target / 'NOTICE.txt').write_text(
            'libneko: Golang base helper library for NekoRay & NekoBox.\n'
            'Source: https://github.com/ad1896git/libneko/tree/'
            '45e350171b7b623860dd60ebce61a8e7c78de87e\n'
            'No upstream license document was present at this revision.\n'
            'This notice does not grant or infer a license.\n')
    target = out / name
    for source in files:
        dest = target / source.relative_to(root)
        dest.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source, dest)
    if headers.get(name):
        (target / 'SOURCE-NOTICES.txt').write_text(''.join('\n===== ' + p + ' =====\n' + h + '\n' for p, h in sorted(headers[name])))
    inventory.append({'module': name, 'source': actual['Path'], 'version': actual.get('Version', 'local pinned source; see SOURCES.txt'),
                      'documents': [str(p.relative_to(root)) for p in files],
                      'supplemental_notice': ('See LICENSE.txt and UPSTREAM.md; license from the named cloudflare/go commit'
                          if name == 'github.com/sagernet/cloudflare-tls' else
                          'No upstream license document; see NOTICE.txt' if name == 'github.com/matsuridayo/libneko' else None)})
# The Go runtime/standard library is linked too.
goroot = Path(subprocess.check_output(['go', 'env', 'GOROOT'], text=True).strip())
shutil.copyfile(goroot / 'LICENSE', out / 'Go-LICENSE')
shutil.copyfile(goroot / 'PATENTS', out / 'Go-PATENTS')
(out / 'inventory.json').write_text(json.dumps(inventory, indent=2) + '\n')
print('Collected license documents for', len(inventory), 'Go dependency modules plus Go')
