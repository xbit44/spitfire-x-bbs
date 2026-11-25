#!/usr/bin/env python3
"""Stage an installable Linux tree from SFX builds, source and pinned assets."""
import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import shutil
import sys


def relative(value):
    path = PurePosixPath(value)
    if not value or '\\' in value or path.is_absolute() or any(
            part in ('', '.', '..') for part in value.split('/')) or ':' in value:
        raise ValueError('Unsafe package path: ' + value)
    return Path(*path.parts)


def ordinary_file(base, value):
    path = base / relative(value)
    for part in [path] + list(path.parents):
        if part == base:
            break
        if part.is_symlink():
            raise ValueError('Input must not be a symbolic link: ' + str(path))
    if not path.is_file():
        raise ValueError('Missing input: ' + str(path))
    return path


def stage(source, build, assets, output):
    source, build, assets = source.resolve(), build.resolve(), assets.resolve()
    output = output.absolute()
    if output.exists() or output.is_symlink():
        raise ValueError('Output must be a NEW folder: ' + str(output))
    if not output.parent.is_dir() or output.parent.resolve() != output.parent:
        raise ValueError('Output parent must exist without path aliases.')
    mapping = json.loads((source / 'distribution/linux/package-inputs.json').read_text(encoding='utf-8'))
    if mapping['format'] != 1 or mapping['platform'] != 'linux-amd64':
        raise ValueError('Unsupported package input map.')
    plans, seen = [], set()
    bases = dict(source=source, build=build, asset=assets)
    for row in mapping['files']:
        target = relative(row['path'])
        if row['path'] in seen or row['kind'] not in bases:
            raise ValueError('Duplicate path or invalid input kind.')
        seen.add(row['path'])
        path = ordinary_file(bases[row['kind']], row['input'])
        data = path.read_bytes()
        if row.get('transform') == 'ascii-crlf':
            data = data.decode('ascii').replace('\r\n', '\n').replace('\n', '\r\n').encode('ascii')
        sha = hashlib.sha256(data).hexdigest()
        if row['kind'] == 'asset' and (len(data) != row['bytes'] or sha != row['sha256']):
            raise ValueError('Pinned runtime asset differs: ' + row['path'])
        if row['kind'] == 'build' and not (data[:5] == b'\x7fELF\x02' and data[5:6] == b'\x01' and data[18:20] == b'\x3e\x00'):
            raise ValueError('Expected an AMD64 ELF build: ' + row['input'])
        mode = int(row['mode'], 8)
        if mode not in (0o600, 0o644, 0o700, 0o755) or row['policy'] not in ('fresh', 'always', 'stock-menus', 'stock-docs'):
            raise ValueError('Invalid package mode or policy.')
        plans.append((target, data, dict(path=row['path'], bytes=len(data), sha256=sha,
                                        mode=row['mode'], policy=row['policy'])))
    controls = {name: ordinary_file(source, 'distribution/linux/' + name).read_bytes()
                for name in ('install.py', 'legacy-docs-rc12.json')}
    manifest = dict(format=1, version=mapping['version'], platform=mapping['platform'],
                    layout_schema=mapping['layout_schema'], files=[p[2] for p in plans])
    # Validate every source/build/asset before creating the output. A failed
    # write leaves its new staging folder for inspection; never remove a tree.
    output.mkdir(mode=0o700)
    for target, data, row in plans:
        destination = output / 'payload' / target
        destination.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
        with destination.open('xb') as stream:
            stream.write(data)
        destination.chmod(0o644)
    for name, data in controls.items():
        (output / name).write_bytes(data)
        (output / name).chmod(0o644)
    (output / 'package-manifest.json').write_text(json.dumps(manifest, indent=2) + '\n', encoding='utf-8')
    (output / 'package-manifest.json').chmod(0o644)
    print('Staged ' + str(len(plans)) + ' files in ' + str(output))
    print('Install as your normal Linux account: python3 ' + str(output / 'install.py'))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir', required=True, type=Path, help='CMake Linux build folder containing the six SFX programs')
    parser.add_argument('--output', required=True, type=Path, help='New staging folder, separate from the installed board')
    parser.add_argument('--assets', type=Path, help='Pinned runtime assets (default: linux-runtime-assets beside SourceSnapshot)')
    args = parser.parse_args()
    source = Path(__file__).resolve().parents[2]
    try:
        stage(source, args.build_dir, args.assets or source.parent / 'linux-runtime-assets', args.output)
        return 0
    except (OSError, ValueError, KeyError, TypeError) as error:
        print('Package staging stopped: ' + str(error), file=sys.stderr)
        return 1


if __name__ == '__main__':
    sys.exit(main())
