#!/usr/bin/env python3
"""Archive the current source tree, including edits outside a Git checkout.

Generated binaries, build outputs, repository metadata and local handoff files
are excluded. Every archived input is recorded in SOURCE-MANIFEST.json.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import zipfile


def archive(root, destination, version):
    if not re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9._-]*', version):
        raise ValueError('invalid archive version')
    if destination.exists():
        raise FileExistsError(destination)
    payloads = {}
    for name in ('src', 'build', 'tools', 'test', 'harness'):
        directory = root / name
        if not directory.is_dir() or directory.is_symlink():
            raise ValueError('missing source directory: ' + name)
        for path in sorted(directory.rglob('*')):
            relative = path.relative_to(root)
            if any(p in ('out', 'obj', '__pycache__', '.git') for p in relative.parts):
                continue
            if path.suffix.lower() in ('.o', '.obj', '.elf', '.pyc', '.exe', '.sys', '.dll'):
                continue
            if relative.as_posix() == 'test/host/tfsdetect':
                continue
            if path.is_symlink():
                raise ValueError('source symlink is not permitted: ' + str(relative))
            if path.is_file():
                payloads[relative.as_posix()] = path.read_bytes()
    for name in ('README.md', 'CHANGELOG.md', 'LICENSE', 'RELEASE-1.3-STATUS.md'):
        path = root / name
        if path.is_symlink():
            raise ValueError('source symlink is not permitted: ' + name)
        if path.exists():
            payloads[name] = path.read_bytes()
        elif name != 'RELEASE-1.3-STATUS.md':
            raise FileNotFoundError(path)
    manifest = {name: {'bytes': len(data), 'sha256': hashlib.sha256(data).hexdigest()}
                for name, data in sorted(payloads.items())}
    payloads['SOURCE-MANIFEST.json'] = (json.dumps(manifest, indent=2) + '\n').encode()
    with zipfile.ZipFile(destination, 'x', compression=zipfile.ZIP_DEFLATED, compresslevel=9) as output:
        for name, data in sorted(payloads.items()):
            entry = zipfile.ZipInfo('usbnt-src-' + version + '/' + name, date_time=(1980, 1, 1, 0, 0, 0))
            entry.compress_type = zipfile.ZIP_DEFLATED
            entry.external_attr = (0o100755 if name.endswith('.sh') else 0o100644) << 16
            output.writestr(entry, data)
    with zipfile.ZipFile(destination) as check:
        if check.testzip() is not None:
            raise ValueError('archive CRC validation failed')
        for name, data in payloads.items():
            if check.read('usbnt-src-' + version + '/' + name) != data:
                raise ValueError('archive readback differs: ' + name)
    return manifest


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('version')
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    files = archive(Path(__file__).resolve().parents[1], args.output, args.version)
    print('Archived and verified', len(files), 'current source files:', args.output)
