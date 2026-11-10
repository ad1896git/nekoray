#!/usr/bin/env python3
"""Run in the core module, with the same GOOS/GOARCH/tags as go build."""
import json
from pathlib import Path
import re
import shutil
import subprocess
import sys

from validate_licenses import (LICENSE_NAME, LIBNEKO, LIBNEKO_NOTICE, LOCAL_VERSION,
                               exception, require, validate_go)

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
            comments = re.findall(r'/\*.*?\*/|(?m:^[ \t]*//[^\n]*(?:\n[ \t]*//[^\n]*)*)',
                                  source.read_text(errors='replace'), re.S)
            notices = [comment for comment in comments if re.search(r'copyright|licen[cs]e|SPDX', comment, re.I)]
            if notices:
                headers.setdefault(module['Path'], []).append((package['ImportPath'] + '/' + filename, '\n'.join(notices)))
legal = re.compile(r'^(licen[cs]e|copying|copyright|notice|authors|patents)([._-].*)?$', re.I)
inventory = []
for name, module in sorted(modules.items()):
    actual = module.get('Replace', module)
    root = Path(actual['Dir'])
    # grpc_server is project code and uses the accompanying project LICENSE.
    if name == 'grpc_server':
        continue
    files = sorted(p for p in root.rglob('*') if p.is_file() and legal.match(p.name) and '.git' not in p.parts)
    source_name = actual['Path']
    version = actual.get('Version', LOCAL_VERSION)
    special = exception(name, source_name, version)
    if not any(LICENSE_NAME.match(p.name) for p in files) and not special:
        raise SystemExit('Missing license documents (LICENSE/COPYING) for ' + name)
    target = out / name
    target.mkdir(parents=True, exist_ok=True)
    generated = []
    if special and name == LIBNEKO:
        (target / 'NOTICE.txt').write_text(LIBNEKO_NOTICE)
        generated.append('NOTICE.txt')
    elif special:
        supplemental = Path(__file__).resolve().parent / 'licenses/common/cloudflare-go.txt'
        shutil.copyfile(require(supplemental), target / 'LICENSE.txt')
        shutil.copyfile(require(root / 'README.md'), target / 'UPSTREAM.md')
        generated.extend(('LICENSE.txt', 'UPSTREAM.md'))
    for source in files:
        dest = target / source.relative_to(root)
        dest.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(require(source), dest)
    if headers.get(name):
        (target / 'SOURCE-NOTICES.txt').write_text(''.join('\n===== ' + p + ' =====\n' + h + '\n' for p, h in sorted(headers[name])))
        generated.append('SOURCE-NOTICES.txt')
    inventory.append({'module': name, 'source': actual['Path'], 'version': actual.get('Version', LOCAL_VERSION),
                      'documents': [str(p.relative_to(root)) for p in files],
                      'generated_documents': generated,
                      'supplemental_notice': ('See LICENSE.txt and UPSTREAM.md; license from the named cloudflare/go commit'
                          if name == 'github.com/sagernet/cloudflare-tls' else
                          'No upstream license document; see NOTICE.txt' if name == 'github.com/matsuridayo/libneko' else None)})
# The Go runtime/standard library is linked too.
goroot = Path(subprocess.check_output(['go', 'env', 'GOROOT'], text=True).strip())
shutil.copyfile(require(goroot / 'LICENSE'), out / 'Go-LICENSE')
shutil.copyfile(require(goroot / 'PATENTS'), out / 'Go-PATENTS')
(out / 'inventory.json').write_text(json.dumps(inventory, indent=2) + '\n')
print('Collected license documents for', len(inventory), 'Go dependency modules plus Go')
platform = {'darwin': 'macos', 'windows': 'windows', 'linux': 'linux'}[subprocess.check_output(['go', 'env', 'GOOS'], text=True).strip()]
binary = out.parents[1] / ('nekobox_core.exe' if platform == 'windows' else 'nekobox_core')
validate_go(out, binary, platform)
