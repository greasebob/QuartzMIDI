"""Pin SHA-256 hashes in tools/mp3-to-midi/requirements.txt and its CUDA files.

    python tools/pin-hashes.py

Each `name==version` line gets a `--hash=sha256:` for every file of that release
installable on CPython 3.12, win_amd64 (plus the sdist for SOURCE packages).
Digests come from PyPI's JSON API, or for +cpu/+cu126/+cu130 versions from the
`#sha256=` fragments in PyTorch's package index. setup.ps1 installs with
--require-hashes. Rerun after changing a version; only index metadata is fetched.
"""
import json
import os
import re
import sys
import urllib.request

here = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), 'tools', 'mp3-to-midi')
PYTORCH = 'https://download.pytorch.org/whl/'
# Packages without a wheel; setup.ps1 builds them from source.
SOURCE = {'proxy-tools'}


def fetch(url):
    with urllib.request.urlopen(urllib.request.Request(url, headers={'User-Agent': 'pin-hashes'}), timeout=60) as reply:
        return reply.read().decode('utf-8')


def normal(name):
    return re.sub(r'[-_.]+', '-', name).lower()


def installable(filename):
    if not filename.endswith('.whl'):
        return False
    python, abi, platform = filename[:-4].split('-')[-3:]
    if not any(p in ('any', 'win_amd64') for p in platform.split('.')):
        return False
    for tag in python.split('.'):
        if tag in ('py3', 'py2', 'cp312'):
            return True
        found = re.fullmatch(r'cp3(\d+)', tag)
        if found and 'abi3' in abi.split('.') and int(found.group(1)) <= 12:
            return True
    return False


def hashes(name, version):
    if '+' in version:
        page = fetch(PYTORCH + version.split('+')[1] + '/' + normal(name) + '/')
        found = []
        for filename, digest in re.findall(r'href="[^"]*?([^/"#]+\.whl)#sha256=([0-9a-f]{64})"', page):
            filename = urllib.request.unquote(filename)
            if filename.split('-')[1] == version and installable(filename):
                found.append(digest)
        return found
    release = json.loads(fetch('https://pypi.org/pypi/{}/{}/json'.format(name, version)))
    wanted = (lambda f: f.endswith(('.tar.gz', '.zip'))) if normal(name) in SOURCE else installable
    return [item['digests']['sha256'] for item in release['urls'] if wanted(item['filename'])]


def pin(path):
    lines = open(path, encoding='utf-8').read().split('\n')
    out = []
    index = 0
    while index < len(lines):
        line = lines[index]
        index += 1
        # Replace a pinned line and its existing --hash continuation lines.
        found = re.fullmatch(r'([A-Za-z0-9_.-]+)==(\S+?)(\s*\\)?', line.strip())
        if not found:
            out.append(line)
            continue
        while index < len(lines) and lines[index].lstrip().startswith('--hash='):
            index += 1
        name, version = found.group(1), found.group(2)
        digests = sorted(set(hashes(name, version)))
        if not digests:
            sys.exit('No installable file of {}=={} was found.'.format(name, version))
        print('{:24} {:16} {}'.format(name, version, len(digests)))
        out.append('{}=={} \\'.format(name, version))
        for number, digest in enumerate(digests):
            out.append('    --hash=sha256:' + digest + (' \\' if number + 1 < len(digests) else ''))
    open(path, 'w', encoding='utf-8', newline='\n').write('\n'.join(out))


for name in ('requirements.txt', 'requirements-cu126.txt', 'requirements-cu130.txt'):
    pin(os.path.join(here, name))