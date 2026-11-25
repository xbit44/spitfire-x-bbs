#!/usr/bin/env python3
"""Prepare/check the qualified user-local Pop!_OS 22.04 AMD64 DOS profile."""
import argparse
import ctypes
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import stat
import subprocess
import sys
import tempfile
import urllib.request

PROFILE = Path(__file__).with_name('linux_dos_pop22_amd64.json')
RECEIPT = 'sfx-runtime.json'


def require(ok, message):
    if not ok:
        raise RuntimeError(message)


def refuse_mixed_owned_roots(board):
    owned = {'bin', 'web', 'docs', 'display', 'doors', 'botgate', 'config', 'data',
             'files', 'ftn', 'logs', 'backup', 'work', 'node', 'management', 'games', 'service'}
    for entry in board.iterdir():
        require(entry.name.lower() not in owned or entry.name == entry.name.lower(),
                'Legacy or mixed-case SFX layout is not supported; nothing was changed')


def digest(path):
    h = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(65536), b''):
            h.update(block)
    return h.hexdigest()


def inventory(root):
    rows = {}
    for path in sorted(root.rglob('*')):
        rel = path.relative_to(root).as_posix()
        if rel == RECEIPT:
            continue
        mode = path.lstat().st_mode
        require(not mode & (stat.S_ISUID | stat.S_ISGID | 0o022),
                'Unsafe writable/privileged runtime entry: ' + rel) if not path.is_symlink() else None
        if path.is_symlink():
            target = os.readlink(path)
            require(not os.path.isabs(target), 'Absolute runtime link refused: ' + rel)
            resolved = path.resolve(strict=True)
            require(root.resolve() in resolved.parents, 'Runtime link leaves its folder: ' + rel)
            rows[rel] = {'link': target}
        elif path.is_file():
            rows[rel] = {'sha256': digest(path), 'mode': stat.S_IMODE(mode)}
        else:
            require(path.is_dir(), 'Special runtime file refused: ' + rel)
            rows[rel] = {'directory': True, 'mode': stat.S_IMODE(mode)}
    return rows


def check(runtime, profile):
    require(runtime.is_dir() and not runtime.is_symlink(), 'No prepared runtime at ' + str(runtime))
    receipt = runtime / RECEIPT
    require(receipt.is_file() and not receipt.is_symlink(), 'Existing runtime has no SFX receipt; kept unchanged')
    saved = json.loads(receipt.read_text())
    require(saved.get('profile_sha256') == digest(PROFILE), 'Different runtime profile; kept unchanged')
    require(saved.get('files') == inventory(runtime), 'Runtime files changed; kept unchanged')
    for package in profile['packages']:
        path = runtime / '.packages' / package['url'].rsplit('/', 1)[1]
        require(path.stat().st_size == package['bytes'] and digest(path) == package['sha256'],
                'Package verification failed; kept unchanged')
    print('Verified DOS runtime: ' + str(runtime))


def release_lock(path, held):
    try:
        current = path.lstat()
        if (current.st_dev, current.st_ino) == (held.st_dev, held.st_ino) and stat.S_ISREG(current.st_mode):
            path.unlink()
        else:
            print('DOS setup: changed preparation lock kept at ' + str(path), file=sys.stderr)
    except FileNotFoundError:
        print('DOS setup: preparation lock was already absent', file=sys.stderr)
    except OSError as error:
        print('DOS setup: could not retire preparation lock: ' + str(error), file=sys.stderr)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('board', type=Path, help='existing SFX test/board folder, owned by you')
    parser.add_argument('--check', action='store_true', help='verify an existing prepared runtime without writing')
    parser.add_argument('--package-cache', type=Path, help='optional folder of the original downloaded .deb files')
    args = parser.parse_args()
    require(sys.platform == 'linux' and platform.machine() == 'x86_64' and os.geteuid() != 0,
            'Use a normal account on Pop!_OS 22.04 AMD64; do not use sudo')
    release = dict(line.split('=', 1) for line in Path('/etc/os-release').read_text().splitlines() if '=' in line)
    require(release.get('ID', '').strip('"') == 'pop' and release.get('VERSION_ID', '').strip('"') == '22.04',
            'This profile is checked only on Pop!_OS 22.04 AMD64; other systems are not enabled')
    board = args.board.absolute()
    require(board.resolve(strict=True) == board and board.is_dir(), 'Use an existing board folder without aliases')
    require(not any(c.isspace() or c in '"\\' for c in str(board)), 'Use a board path without spaces, quotes or backslashes')
    require(board.stat().st_uid == os.geteuid() and not board.stat().st_mode & 0o022,
            'Board folder must be owned by you and not writable by other accounts')
    profile = json.loads(PROFILE.read_text())
    print('Checking DOS support prerequisites...', flush=True)
    refuse_mixed_owned_roots(board)
    config = board / 'config'
    require(config.is_dir() and not config.is_symlink() and config.stat().st_uid == os.geteuid()
            and not config.stat().st_mode & 0o077, 'The lowercase config folder must be private and owned by you')
    marker = config / 'linux-layout.v1'
    require(marker.is_file() and not marker.is_symlink() and marker.stat().st_uid == os.geteuid()
            and not marker.stat().st_mode & 0o077 and marker.stat().st_nlink == 1
            and marker.read_bytes() == b'SFX-LINUX-LAYOUT-1\nlinux-lowercase-v1\n',
            'A matching fresh lowercase SFX layout marker is required')
    bindir = board / 'bin'
    runtime = bindir / 'dosemu'
    require(not bindir.is_symlink(), 'bin must not be a link')
    if bindir.exists():
        require(bindir.is_dir() and bindir.stat().st_uid == os.geteuid() and not bindir.stat().st_mode & 0o022,
                'bin must be owned by you and not writable by other accounts; kept unchanged')
    if args.check or os.path.lexists(runtime):
        check(runtime, profile)
        return
    require(shutil.which('dpkg-deb') is not None, 'dpkg-deb is needed; nothing was installed')
    require(shutil.disk_usage(board).free > 100_000_000, 'Keep at least 100 MB free before preparation')
    bindir.mkdir(mode=0o700, exist_ok=True)
    require(bindir.stat().st_uid == os.geteuid() and not bindir.stat().st_mode & 0o022, 'bin must be owned and private to you')
    lock = bindir / '.sfx-dosemu-prepare.lock'
    lock_fd = os.open(str(lock), os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
    lock_identity = os.fstat(lock_fd)
    stage = None
    try:
        require(not os.path.lexists(runtime), 'Runtime already exists; kept unchanged')
        stage = Path(tempfile.mkdtemp(prefix='.dosemu-prepare-', dir=str(bindir)))
        cache = stage / '.packages'
        cache.mkdir(mode=0o700)
        for package in profile['packages']:
            name = package['url'].rsplit('/', 1)[1]
            print('Preparing ' + name + '...', flush=True)
            dest = cache / name
            source = args.package_cache / name if args.package_cache else None
            if source is not None and source.is_file():
                require(source.stat().st_size == package['bytes'] and digest(source) == package['sha256'],
                        'Cached package differs: ' + name)
                shutil.copyfile(source, dest)
            else:
                require(package['url'].startswith('https://'), 'HTTPS package URL required')
                with urllib.request.urlopen(package['url'], timeout=45) as response, dest.open('xb') as out:
                    data = response.read(package['bytes'] + 1)
                    require(len(data) == package['bytes'], 'Unexpected package size: ' + name)
                    out.write(data)
            dest.chmod(0o600)
            require(digest(dest) == package['sha256'], 'Package checksum failed: ' + name)
            print('Verified ' + name + '; extracting...', flush=True)
            subprocess.run(['dpkg-deb', '--extract', str(dest), str(stage)], check=True, timeout=60)
        rows = inventory(stage)
        print('Checking prepared DOS files...', flush=True)
        binary = stage / 'usr/libexec/dosemu2/dosemu2.bin'
        require(binary.is_file() and os.access(binary, os.X_OK), 'Prepared emulator executable is missing')
        require((stage / 'usr/share/doc/dosemu2/copyright').is_file(), 'Official emulator notices are missing')
        (stage / RECEIPT).write_text(json.dumps({'profile_sha256': digest(PROFILE), 'files': rows}, indent=2) + '\n')
        (stage / RECEIPT).chmod(0o600)
        # Publish without replacing even an empty existing directory.
        libc = ctypes.CDLL(None, use_errno=True)
        rename = libc.renameat2
        rename.argtypes = [ctypes.c_int, ctypes.c_char_p, ctypes.c_int, ctypes.c_char_p, ctypes.c_uint]
        require(rename(-100, os.fsencode(stage), -100, os.fsencode(runtime), 1) == 0,
                'Cannot publish runtime; existing folder kept, prepared files retained at ' + str(stage))
        stage = None
        check(runtime, profile)
        print('Ready for the Linux DOS Door adapter. Existing board settings were not changed.')
    finally:
        release_lock(lock, lock_identity)
        os.close(lock_fd)
        if stage is not None:
            print('Incomplete preparation retained for inspection: ' + str(stage), file=sys.stderr)


if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
        print('DOS setup: ' + str(error), file=sys.stderr)
        sys.exit(1)
