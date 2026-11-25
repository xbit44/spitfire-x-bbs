#!/usr/bin/env python3
"""Normal-user SFX installation from a verified local package. Python 3.8+."""
import argparse
import fcntl
import hashlib
import json
import os
import re
from pathlib import Path, PurePosixPath
import shutil
import shlex
import stat
import sys
import uuid


LAYOUT_SCHEMA = 'linux-lowercase-v1'
LAYOUT_MARKER = 'config/linux-layout.v1'
LAYOUT_BYTES = b'SFX-LINUX-LAYOUT-1\nlinux-lowercase-v1\n'
OWNED_ROOTS = {'bin', 'web', 'docs', 'display', 'doors', 'botgate', 'config', 'data',
               'files', 'ftn', 'logs', 'backup', 'work', 'node', 'management', 'games', 'service'}
ROOT_FILES = {'spitfire-x', 'prepare-sfx', 'readme.txt'}
PAYLOAD_ROOTS = {'bin', 'web', 'docs', 'display', 'doors', 'botgate'}


# Exact predecessor artwork, frozen before the rc10 corrections. Both members
# must match; customized or mixed pairs are preserved together.
RC10_STOCK_PAIRS = {'main': {'ans': 'e39de603a583bbb83d247de204e5e6d0b5b0fae8c10d578b7f0e4442c8ae22f5', 'asc': '90802c5a5c4952f2db68542c9ef70f158f8cb8bd012dfd9a3c85a57bfcb8fef3'}, 'main999': {'ans': '891219a9a83f25b1242ac8c7e290c7f9454f13fab7e4a1b05aa5348753f343f6', 'asc': '3110891784b05228637039191a00c9b5cf6439d8ef8070a3676f2d5107599a0a'}, 'your': {'ans': '9469aa64c8b98da19384141b3736afbe6df401f8717aeff6541af24d7e5fc8f0', 'asc': 'f86fdf230a54fc734d4a5cb1012d1e635b08e3499672e10c6d3761e4481c86b8'}, 'bulletin': {'ans': 'd13ba46ebaac9524e7164553e9281275e49347b8ac073e373e83e3a8d6bf6b1b', 'asc': '87d8495875e2ac2a5fb37a5da620bcc8497a60fa55cba8933fbba58b26f3c6f2'}, 'file': {'ans': '716207d9ed4d39941192a527c55c4b21ecbcb96039d1d19cd8182bdba945ade3', 'asc': 'c5841fb734ce00cf37abbc0b6c65f3b5b6e1737663ff8accf37f0c19df090b7e'}, 'sysop': {'ans': '6c975b2a24c480bf08eca89b32078f1cf3f1e7af2421337beec3a1398d8bd07a', 'asc': 'a6b90431a324776e6fe4fbfd1e81a6a9aefd47dce6980105f47d3f2a5f445d67'}}


def require(ok, message):
    if not ok:
        raise RuntimeError(message)


def digest(path):
    h = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            h.update(block)
    return h.hexdigest()


def relative(value):
    require(isinstance(value, str) and value and '\\' not in value and '\0' not in value,
            'Invalid package path.')
    path = PurePosixPath(value)
    require(not path.is_absolute() and path.as_posix() == value and
            all(x not in ('', '.', '..') for x in value.split('/')), 'Invalid package path.')
    return Path(*path.parts)


def checked_path(path, directory=False, owned=True):
    # Pinning the install lock excludes SFX; reject aliases before publication.
    # A malicious process with this same Unix identity is outside this boundary.
    for ancestor in [*reversed(path.parents), path]:
        if ancestor.exists() or ancestor.is_symlink():
            require(not ancestor.is_symlink(), 'A selected path uses a symbolic link: ' + str(ancestor))
    info = path.lstat()
    require(stat.S_ISDIR(info.st_mode) if directory else stat.S_ISREG(info.st_mode),
            'Unexpected file type: ' + str(path))
    require(not owned or info.st_uid == os.geteuid(), 'File belongs to another account: ' + str(path))
    require(not info.st_mode & 0o022, 'Group or other users can write to: ' + str(path) +
            '. Choose a private SFX folder, or review permissions on this exact path and retry.')
    require(directory or info.st_nlink == 1, 'Hard-linked file refused: ' + str(path))
    return info


def flush_folder(path):
    fd = os.open(str(path), os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
    try:
        os.fsync(fd)
    finally:
        os.close(fd)


def normalize_outer_directory(path):
    # Only the explicitly selected, owned outer folder may start group writable.
    # Pin its identity before changing mode; all internal checked_path guards stay.
    require(path.resolve() == path and not path.is_symlink(), 'Choose an SFX folder without links.')
    fd = os.open(str(path), os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
    try:
        info = os.fstat(fd)
        current = path.lstat()
        require(stat.S_ISDIR(info.st_mode) and info.st_uid == os.geteuid() and
                (info.st_dev, info.st_ino) == (current.st_dev, current.st_ino),
                'Installation folder ownership or identity is unsafe.')
        if stat.S_IMODE(info.st_mode) != 0o700:
            os.fchmod(fd, 0o700)
        checked = checked_path(path, directory=True)
        require((info.st_dev, info.st_ino) == (checked.st_dev, checked.st_ino),
                'Installation folder changed.')
    finally:
        os.close(fd)


def stock_menu_refresh(root):
    selected = set()
    directory = root / 'display'
    if not directory.exists():
        return selected
    checked_path(directory, directory=True)
    names = [p.name for p in directory.iterdir()]
    previous_pairs = dict(RC10_STOCK_PAIRS, msg={
        'ans': '6cf06a76921da2c0f4f36418015970da9ece642fdf5e4613600bc0d9b7f27cc0',
        'asc': '18d6e5e6b46015a2bde44272bad5f259ceee5dcee9af02c663cbcc231d5e403a'})
    for stem, hashes in previous_pairs.items():
        pair = [stem + '.' + ext for ext in ('ans', 'asc')]
        found = [name for name in names if name.lower() in pair]
        if not found:
            continue
        matched = sorted(found) == sorted(pair)
        if matched:
            try:
                actual = {}
                for ext in ('ans', 'asc'):
                    path = directory / (stem + '.' + ext)
                    checked_path(path)
                    actual[ext] = digest(path)
                matched = actual == hashes or (stem == 'bulletin' and actual == {
                    'ans': '0eca8eca68e26bc65346e7d34ad3cad06df8507155add2acefdb36b18682f23f',
                    'asc': '8043eb04f820eccf2f91057ae6b1a1ee42136576f0fe6c271e8a75260cb619e1'})
            except (OSError, RuntimeError):
                matched = False
        if matched:
            selected.update('display/' + name for name in pair)
            print('Refreshing unchanged stock menu pair: ' + stem)
        else:
            print('Keeping existing menu pair: ' + stem + ' (custom, mixed, or already current).')
    return selected


def write_new(path, data, mode=0o600):
    fd = os.open(str(path), os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, mode)
    with os.fdopen(fd, 'wb') as stream:
        stream.write(data)
        stream.flush()
        os.fsync(stream.fileno())


def make_folders(path, root):
    current = root
    for part in path.relative_to(root).parts:
        current = current / part
        if not current.exists():
            current.mkdir(mode=0o700)
        checked_path(current, directory=True)


def valid_hash(value):
    return isinstance(value, str) and len(value) == 64 and all(c in '0123456789abcdef' for c in value)


def payload_path(name):
    rel = relative(name)
    require((rel.parts[0] in PAYLOAD_ROOTS and len(rel.parts) > 1) or
            (name in ROOT_FILES) or name == LAYOUT_MARKER, 'Invalid package destination: ' + name)
    # These two upstream trees contain required original vendor/IANA spellings.
    # Integrated first-party help (including Door help) has lowercase aliases.
    vendor = name.startswith('bin/tzdata/') or name.startswith('docs/thirdparty/clamav/')
    require(name.isascii() and (name == name.lower() or vendor),
            'Non-lowercase SFX package destination: ' + name)
    return rel


def receipt_file(path):
    if not path.exists() and not path.is_symlink():
        return None
    require(not checked_path(path).st_mode & 0o077, 'Installed receipt is not private.')
    saved = json.loads(path.read_text(encoding='utf-8'))
    require(isinstance(saved, dict) and saved.get('format') == 1 and
            saved.get('layout_schema') == LAYOUT_SCHEMA, 'Installed receipt is not the current lowercase layout.')
    hashes = saved.get('stock_docs')
    require(isinstance(hashes, dict), 'Invalid installed documentation inventory.')
    seen = set()
    for name, value in hashes.items():
        require(payload_path(name).parts[0] == 'docs' and valid_hash(value) and name.casefold() not in seen,
                'Invalid installed documentation inventory.')
        seen.add(name.casefold())
    return saved


def installed_receipt(root):
    return receipt_file(root / '.sfx-installed.json')


def previous_stock_docs(package, root):
    # Older uppercase inventories must never be silently adopted as this layout.
    saved = installed_receipt(root)
    return dict(saved['stock_docs']) if saved else {}


def checked_target(root, rel):
    current = root
    for part in rel.parts:
        if current.exists():
            for entry in current.iterdir():
                require(entry.name.casefold() != part.casefold() or entry.name == part,
                        'Existing path has a different spelling: ' + str(entry))
        current = current / part
        if current.exists() or current.is_symlink():
            checked_path(current, directory=current != root / rel)


def refuse_mixed_layout(root):
    names = OWNED_ROOTS | ROOT_FILES | {'.sfx-installed.json', '.sfx-runtime.lock'}
    for entry in root.iterdir():
        folded = entry.name.lower()
        require(folded not in names or entry.name == folded,
                'Legacy or mixed-case SFX layout refused. Choose a new empty folder.')
        if entry.name in OWNED_ROOTS:
            checked_path(entry, directory=True)
    fixed = {
        'config': {'sfx.conf', 'sfxsecurity.conf', 'sfxscheduler.conf', 'sfxmessageretention.conf',
                   'sfxmessageintegrity.conf', 'sfxnodes.state', 'joker.dat', 'linux-layout.v1'},
        'data': {'sfx.db', 'sfxscheduler.state', 'sfxmessageretention.state', 'sfxmessageintegrity.state',
                 'fileintake', 'fileremoved', 'clamav'},
        'bin': {'sfxruntime', 'sfxweb', 'sfxhostlookup', 'sfxbinkd', 'tools', 'clamav', 'dosemu', 'tzdata'},
        'bin/tools': {'sfxfiletransfer', 'sfxqwktransfer'},
        'docs': {'sysop', 'doors', 'thirdparty'},
        'work': {'clamav'}, 'backup': {'scheduled', 'upgrades', 'recovery', 'restorefiles'},
        'files': {'general', 'uploads', 'uploadtosysop'},
        'ftn': {'inbound', 'outbound', 'partial', 'nonsecure', 'bad', 'processed', 'conflicts', 'external'},
        'ftn/external': {'inbound', 'temp', 'records', 'consumed', 'runs', 'tics'},
        'ftn/external/inbound': {'secure', 'nonsecure', 'links'},
        'logs': {'callers.log', 'heysysop.log', 'sfxruntime.log'}
    }
    for directory, expected in fixed.items():
        path = root / directory
        if not path.exists():
            continue
        checked_path(path, directory=True)
        for entry in path.iterdir():
            folded = entry.name.lower()
            generated = (directory == 'config' and folded.startswith('sfxnode') and folded.endswith('.state')) or (
                directory == 'data' and (folded.startswith('sfx.db-') or folded.startswith('sfx.db.'))) or (
                directory == 'logs' and re.fullmatch(
                    r'(callers|heysysop|sfxruntime)-[0-9]{8}t[0-9]{9}z-[0-9a-f]{32}\.log', folded) is not None)
            require((folded not in expected and not generated) or entry.name == folded,
                    'Mixed-case SFX-owned path refused: ' + str(entry))
            known_file = directory in ('config', 'logs') and (folded in expected or generated)
            known_file = known_file or (directory == 'data' and (generated or folded in {
                'sfx.db', 'sfxscheduler.state', 'sfxmessageretention.state', 'sfxmessageintegrity.state'}))
            if known_file:
                checked_path(entry)


def read_journal(root, stage):
    require(stage.parent == root and stage.name.startswith('.sfx-update-') and
            len(stage.name) == len('.sfx-update-') + 32 and
            all(c in '0123456789abcdef' for c in stage.name[len('.sfx-update-'):]), 'Unexpected update folder.')
    checked_path(stage, directory=True)
    journal = stage / 'journal.json'
    checked_path(journal)
    plan = json.loads(journal.read_text(encoding='utf-8'))
    require(isinstance(plan, dict) and plan.get('root') == str(root) and
            plan.get('layout_schema') == LAYOUT_SCHEMA and type(plan.get('fresh_install')) is bool and
            isinstance(plan.get('files'), list) and plan['files'], 'Update journal is not for this lowercase installation.')
    seen = set()
    for row in plan['files']:
        require(isinstance(row, dict) and set(row) == {'path', 'old', 'new'}, 'Invalid update journal entry.')
        name = row.get('path')
        relative(name) if name == '.sfx-installed.json' else payload_path(name)
        require(name.casefold() not in seen and valid_hash(row.get('new')) and
                (row.get('old') is None or valid_hash(row['old'])) and
                (not plan['fresh_install'] or row['old'] is None or
                 (name == LAYOUT_MARKER and row['old'] == hashlib.sha256(LAYOUT_BYTES).hexdigest())),
                'Invalid update journal entry.')
        seen.add(name.casefold())
    require('.sfx-installed.json' in seen and (not plan['fresh_install'] or LAYOUT_MARKER in seen),
            'Update journal is missing its layout publication entries.')
    for row in plan['files']:
        if row['path'] == LAYOUT_MARKER:
            require(row['new'] == hashlib.sha256(LAYOUT_BYTES).hexdigest(), 'Update journal layout marker is invalid.')
    return plan


def layout_preflight(root, upgrade=False, recovery=None, rows=()):
    # Read-only: eligibility is established before chmod, lock creation or any
    # lowercase directory creation, then repeated under the exclusive lock.
    require(root.resolve() == root and not root.is_symlink(), 'Choose an SFX folder without links.')
    info = root.lstat()
    require(stat.S_ISDIR(info.st_mode) and info.st_uid == os.geteuid(), 'Installation folder ownership or type is unsafe.')
    refuse_mixed_layout(root)
    lock = root / '.sfx-runtime.lock'
    if lock.exists() or lock.is_symlink():
        require(not checked_path(lock).st_mode & 0o077, 'Installation ownership lock is unsafe.')
    marker = root / LAYOUT_MARKER
    marked = marker.exists() or marker.is_symlink()
    if marked:
        require(not checked_path(marker).st_mode & 0o077 and marker.read_bytes() == LAYOUT_BYTES,
                'Linux lowercase layout marker is invalid.')
    receipt = installed_receipt(root)
    pending = list(root.glob('.sfx-update-*'))
    if recovery is not None:
        require(pending == [recovery] or (len(pending) == 1 and pending[0] == recovery),
                'Choose the only retained update folder for rollback.')
        plan = read_journal(root, recovery)
        validate_rollback_files(root, recovery, plan)
        if plan['fresh_install']:
            # Before first publication the marker may not exist. Only journaled
            # fresh files and empty directories are eligible, never board state.
            allowed = {row['path'] for row in plan['files']} | {'.sfx-runtime.lock'}
            for directory, dirs, files in os.walk(str(root), followlinks=False):
                here = Path(directory)
                if here == root:
                    dirs[:] = [name for name in dirs if root / name != recovery]
                for name in dirs:
                    checked_path(here / name, directory=True)
                for name in files:
                    path = here / name
                    require(path.relative_to(root).as_posix() in allowed, 'Fresh rollback found unjournaled installation state.')
                    checked_path(path)
            return info
        retained_receipt = recovery / 'old/.sfx-installed.json'
        retained = receipt_file(retained_receipt)
        if retained is not None:
            require(any(row['path'] == '.sfx-installed.json' and row['old'] == digest(retained_receipt)
                        for row in plan['files']), 'Retained receipt does not match the update journal.')
        require(marked and (receipt is not None or retained is not None or (root / 'config/sfx.conf').is_file()),
                'Current layout marker and installed state are required for update rollback.')
        return info
    require(not pending, 'An earlier update is incomplete. Use --rollback-update with its exact folder name first.')
    if not marked:
        require(receipt is None, 'An established installation is missing its lowercase layout marker.')
        for name in ('config', 'data'):
            path = root / name
            require(not path.exists() or not list(path.iterdir()),
                    'Existing configuration or data has no lowercase layout marker. Choose a new empty folder.')
    files = []
    if not upgrade:
        for directory, dirs, leaves in os.walk(str(root), followlinks=False):
            here = Path(directory)
            for name in dirs:
                checked_path(here / name, directory=True)
            files.extend((here / name).relative_to(root).as_posix() for name in leaves)
        require(all(name in ('.sfx-runtime.lock', LAYOUT_MARKER) for name in files),
                'This folder is not empty. Use --upgrade only for an existing current-layout SFX board.')
    else:
        require(marked and (receipt is not None or (root / 'config/sfx.conf').is_file()),
                'No current lowercase SFX installation was found. Legacy uppercase upgrades are not supported.')
    for row in rows:
        if row['policy'] == 'always' or not upgrade or row['path'] == LAYOUT_MARKER:
            checked_target(root, relative(row['path']))
    return info


def package_manifest(package):
    manifest_file = package / 'package-manifest.json'
    checked_path(manifest_file, owned=False)
    manifest = json.loads(manifest_file.read_text(encoding='utf-8'))
    require(isinstance(manifest, dict) and manifest.get('format') == 1 and
            manifest.get('platform') == 'linux-amd64' and isinstance(manifest.get('version'), str) and
            manifest['version'] and manifest.get('layout_schema') == LAYOUT_SCHEMA and
            isinstance(manifest.get('files'), list), 'Unsupported Linux lowercase package manifest.')
    seen = set()
    by_name = {}
    for row in manifest['files']:
        require(isinstance(row, dict), 'Invalid package entry.')
        rel = payload_path(row.get('path'))
        require(row['path'].casefold() not in seen and row.get('policy') in ('always', 'fresh') and
                row.get('mode') in ('0600', '0700', '0644', '0755') and
                type(row.get('bytes')) is int and row['bytes'] >= 0 and valid_hash(row.get('sha256')),
                'Invalid package entry.')
        require(rel.parts[0] not in ('display', 'doors', 'botgate') or row['policy'] == 'fresh',
                'Board content may only be installed fresh.')
        seen.add(row['path'].casefold())
        by_name[row['path']] = row
        source = package / 'payload' / rel
        info = checked_path(source, owned=False)
        require(info.st_size == row['bytes'] and digest(source) == row['sha256'],
                'Package checksum mismatch: ' + row['path'])
    require(all('/'.join(PurePosixPath(name).parts[:i]) not in by_name
                for name in by_name for i in range(1, len(PurePosixPath(name).parts))),
            'Package paths overlap a file and folder.')
    for name in ('bin/sfxweb', 'spitfire-x', 'prepare-sfx', 'readme.txt'):
        require(name in by_name and by_name[name]['policy'] == 'always', 'Package is missing its normal entry points.')
    for name in ('bin/sfxweb', 'spitfire-x', 'prepare-sfx'):
        require(int(by_name[name]['mode'], 8) & 0o111, 'Package entry point is not executable: ' + name)
    row = by_name.get(LAYOUT_MARKER)
    require(row is not None and row['policy'] == 'fresh' and row['mode'] == '0600' and
            (package / 'payload' / LAYOUT_MARKER).read_bytes() == LAYOUT_BYTES,
            'Package is missing the exact fresh lowercase layout marker.')
    return manifest_file, manifest


def validate_rollback_files(root, stage, plan):
    # Inspect every current path and retained original before changing any path.
    for row in plan['files']:
        path = root / relative(row['path'])
        old = stage / 'old' / relative(row['path'])
        checked_target(root, relative(row['path']))
        if path.exists() or path.is_symlink():
            checked_path(path)
            require(digest(path) in (row['new'], row['old']), 'A file changed after the update: ' + str(path))
        if old.exists() or old.is_symlink():
            checked_path(old)
            require(row['old'] and digest(old) == row['old'], 'The retained original changed: ' + str(old))
        elif row['old']:
            require(path.exists() and digest(path) == row['old'], 'Retained original is unavailable: ' + str(path))


def rollback(root, stage):
    plan = read_journal(root, stage)
    layout_preflight(root, recovery=stage)
    for row in reversed(plan['files']):
        path = root / relative(row['path'])
        old = stage / 'old' / relative(row['path'])
        if old.exists():
            os.replace(str(old), str(path))
        elif row['old'] is None and path.exists():
            path.unlink()
        if path.parent.exists():
            flush_folder(path.parent)
    # Exact owned transaction tree, already validated; no board-data traversal.
    require(stage.resolve() == stage and stage.parent.resolve() == root, 'Update folder moved.')
    shutil.rmtree(str(stage))
    flush_folder(root)


def install(package, root, upgrade=False, recover=None):
    require(sys.platform == 'linux' and os.getuid() == os.geteuid() != 0,
            'Run this installer as your normal Linux account, without sudo.')
    root = Path(os.path.abspath(os.path.expanduser(str(root))))
    require(root != Path('/') and root != Path.home(), 'Choose a separate SFX folder, such as ~/sfx.')
    require(root.parent.is_dir() and root.parent.resolve() == root.parent, 'Choose an existing parent folder without links.')
    if not upgrade and recover is None:
        staging = Path(package).resolve()
        require(staging != root and root not in staging.parents,
                'The extracted package is inside the chosen installation folder. '
                'Keep the archive and extracted package in a separate staging folder, '
                'then choose a new or empty SFX folder.')
        if root.is_dir() and not root.is_symlink() and root.resolve() == root:
            entries = {path.name for path in root.iterdir()}
            layout_marker = root / LAYOUT_MARKER
            if (not entries.intersection({'.sfx-runtime.lock', '.sfx-installed.json'})
                    and not (layout_marker.exists() or layout_marker.is_symlink())
                    and not any(name.startswith('.sfx-update-') for name in entries)):
                require(not entries, 'The chosen installation folder is not empty: ' + str(root) +
                        '. Move the archive and extracted package to a separate staging folder, '
                        'or choose a new empty folder. Use --upgrade for an existing current-layout SFX board.')
    # Validate every package byte before creating or normalizing a destination.
    # Recovery uses its retained journal, not a possibly unavailable package.
    if recover is None:
        package = Path(package).resolve()
        manifest_file, manifest = package_manifest(package)
    stage_to_recover = root / relative(recover) if recover is not None else None
    require(stage_to_recover is None or stage_to_recover.parent == root, 'Choose the update folder name only.')
    if root.exists() or root.is_symlink():
        before = layout_preflight(root, upgrade, stage_to_recover,
                                  manifest['files'] if recover is None else ())
    else:
        require(recover is None and not upgrade, 'No existing current-layout SFX installation was found.')
        root.mkdir(mode=0o700)
        before = layout_preflight(root, rows=manifest['files'])
    normalize_outer_directory(root)
    lock_fd = os.open(str(root / '.sfx-runtime.lock'), os.O_RDWR | os.O_CREAT | os.O_NOFOLLOW, 0o600)
    try:
        info = os.fstat(lock_fd)
        require(stat.S_ISREG(info.st_mode) and info.st_uid == os.geteuid() and info.st_nlink == 1 and not info.st_mode & 0o077,
                'Installation ownership lock is unsafe.')
        try:
            fcntl.flock(lock_fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            raise RuntimeError('Stop the BBS, then run ' + shlex.join([str(root / 'spitfire-x'), '--stop-management']) + ' before updating. Closing the browser is not enough. The BBS and web host were left unchanged.')
        after = layout_preflight(root, upgrade, stage_to_recover,
                                 manifest['files'] if recover is None else ())
        require((before.st_dev, before.st_ino) == (after.st_dev, after.st_ino), 'Installation folder changed.')
        require((info.st_dev, info.st_ino) == ((root / '.sfx-runtime.lock').lstat().st_dev,
                                            (root / '.sfx-runtime.lock').lstat().st_ino), 'Installation ownership lock changed.')
        if recover is not None:
            rollback(root, stage_to_recover)
            return root
        plan = []
        stock_docs = previous_stock_docs(package, root) if upgrade else {}
        refresh_menus = stock_menu_refresh(root) if upgrade else set()
        for row in manifest['files']:
            rel = relative(row['path'])
            source = package / 'payload' / rel
            if upgrade and row['policy'] == 'fresh' and row['path'] not in refresh_menus:
                continue
            target = root / rel
            checked_target(root, rel)
            if rel.parts[0] == 'docs':
                installed_hash = digest(target) if target.exists() else None
                if upgrade and installed_hash is not None and installed_hash not in (stock_docs.get(row['path']), row['sha256']):
                    print('Keeping edited or unrecognized documentation: ' + row['path'])
                    continue
                # A preserved edit never becomes its own stock baseline.
                stock_docs[row['path']] = row['sha256']
            plan.append(dict(path=row['path'], source=source, old=digest(target) if target.exists() else None,
                             new=row['sha256'], mode=0o700 if int(row['mode'], 8) & 0o111 else 0o600))
        require(refresh_menus <= {x['path'] for x in plan}, 'Package is missing a complete stock menu pair.')
        require(any(x['path'] == 'bin/sfxweb' for x in plan) and any(x['path'] == 'spitfire-x' for x in plan), 'Package is missing its normal entry points.')
        # ftn is an owned data role, not a packaged seed file. Provision it
        # through explicit install/update; never chmod or follow an old role.
        ftn = root / 'ftn'
        if ftn.exists() or ftn.is_symlink():
            checked_path(ftn, directory=True)
        marker = json.dumps(dict(format=1, layout_schema=LAYOUT_SCHEMA, version=manifest['version'],
                                 package_sha256=digest(manifest_file), stock_docs=stock_docs), indent=2).encode('utf-8') + b'\n'
        marker_path = root / '.sfx-installed.json'
        if marker_path.exists() or marker_path.is_symlink():
            checked_path(marker_path)
        plan.append(dict(path='.sfx-installed.json', data=marker, old=digest(marker_path) if marker_path.exists() else None,
                         new=hashlib.sha256(marker).hexdigest(), mode=0o600))
        stage = root / ('.sfx-update-' + uuid.uuid4().hex)
        stage.mkdir(mode=0o700)
        write_new(stage / 'journal.json', json.dumps(dict(root=str(root), layout_schema=LAYOUT_SCHEMA,
                  fresh_install=not upgrade, files=[{k: row[k] for k in ('path', 'old', 'new')} for row in plan]), indent=2).encode('utf-8'))
        flush_folder(stage)
        for row in plan:
            rel = relative(row['path'])
            for folder in (stage / 'new' / rel.parent, stage / 'old' / rel.parent, root / rel.parent):
                make_folders(folder, root)
            data = row['data'] if 'data' in row else row['source'].read_bytes()
            require(hashlib.sha256(data).hexdigest() == row['new'], 'Package input changed during staging.')
            write_new(stage / 'new' / rel, data, row['mode'])
        flush_folder(stage)
        try:
            for row in plan:
                rel = relative(row['path']); target = root / rel
                if row['old']:
                    require(digest(target) == row['old'], 'Installed input changed during update.')
                    os.replace(str(target), str(stage / 'old' / rel))
                else:
                    require(not target.exists() and not target.is_symlink(), 'A destination appeared during update.')
                os.replace(str(stage / 'new' / rel), str(target))
                flush_folder(target.parent)
            for row in plan:
                require(digest(root / relative(row['path'])) == row['new'], 'Installed checksum verification failed.')
            make_folders(ftn, root)
            flush_folder(root)
        except Exception:
            rollback(root, stage)
            raise
        require(stage.resolve() == stage and stage.parent == root, 'Update folder moved.')
        shutil.rmtree(str(stage)); flush_folder(root)
        return root
    finally:
        os.close(lock_fd)


def choose_prefix(explicit, updating=False):
    """Ask before changing files; --prefix remains suitable for automation."""
    if explicit is not None:
        return Path(explicit)
    if not sys.stdin.isatty():
        raise ValueError('No interactive terminal. Specify the installation folder with --prefix /full/path/to/sfx.')
    default = Path.home() / 'sfx'
    print('Spitfire-X installation folder')
    if updating:
        print('Choose the folder of the existing SFX installation.')
    else:
        print('Choose a new or empty folder owned by your normal Linux account.')
    print('Keep the archive and extracted package in a separate staging folder.')
    print('The installer does not start the BBS or open a browser.')
    try:
        answer = input('Install folder [' + str(default) + ']: ').strip()
        chosen = Path(os.path.expanduser(answer)) if answer else default
        if not chosen.is_absolute():
            raise ValueError('Use a full, absolute installation path, for example ' + str(default) + '.')
        print('Install in: ' + str(chosen))
        confirm = input('Continue? [Y/n]: ').strip().lower()
        if confirm not in ('', 'y', 'yes'):
            print('Installation cancelled. No files were changed.')
            return None
        return chosen
    except (EOFError, KeyboardInterrupt):
        print('\nInstallation cancelled. No files were changed.')
        return None


def main():
    parser = argparse.ArgumentParser(description='Install Spitfire-X as your normal Linux account.')
    parser.add_argument('--prefix', help='Full SFX folder; omit to choose interactively (default: ~/sfx)')
    parser.add_argument('--upgrade', action='store_true', help='Update program files; keep existing board data and fresh-install seeds')
    parser.add_argument('--rollback-update', metavar='FOLDER', help='Restore a retained incomplete update in the chosen SFX folder')
    args = parser.parse_args()
    try:
        prefix = choose_prefix(args.prefix, args.upgrade or args.rollback_update is not None)
        if prefix is None:
            return 0
        root = install(Path(__file__).resolve().parent, prefix, args.upgrade, args.rollback_update)
        print('Update rolled back.' if args.rollback_update else 'Spitfire-X installed in ' + str(root))
        if (root / 'spitfire-x').is_file():
            print('Next: open your SFX Control Panel on the Linux desktop.')
            print('Run this exact command in a terminal on that desktop:')
            print('  ' + shlex.join([str(root / 'spitfire-x')]))
            print('It opens the local controls in your desktop browser.')
            print('For a new board, finish Set Up Your BBS and choose Create My BBS.')
            print('For an existing board, keep your saved setup and review your settings.')
            print('Choose Start BBS when ready. Installing or opening the controls does not start it.')
            print('Using SSH from another computer? Follow the separate SSH access steps in:')
            print('  ' + str(root / 'readme.txt'))
        else:
            print('Run the installer again to finish the fresh installation. The BBS was not started.')
        return 0
    except (OSError, ValueError, KeyError, TypeError, RuntimeError) as error:
        print('Install stopped: ' + str(error), file=sys.stderr)
        return 1


if __name__ == '__main__':
    sys.exit(main())
