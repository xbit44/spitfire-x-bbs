"""Installer preservation, refusal and interrupted-update tests; normal Linux user."""
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import sys
import fcntl
import shlex
import shutil
import stat

checks = []


def check(value, message):
    assert value, message
    checks.append(message)


def snapshot(root):
    result = {}
    if not root.exists() and not root.is_symlink():
        return result
    paths = [root]
    if root.is_dir() and not root.is_symlink():
        for directory, dirs, files in os.walk(str(root), followlinks=False):
            paths.extend(Path(directory) / name for name in dirs + files)
    for path in paths:
        info = path.lstat()
        metadata = [info.st_mode, info.st_uid, info.st_gid, info.st_nlink, info.st_size,
                    info.st_dev, info.st_ino, info.st_mtime_ns, info.st_ctime_ns]
        value = os.readlink(str(path)) if stat.S_ISLNK(info.st_mode) else (
            hashlib.sha256(path.read_bytes()).hexdigest() if stat.S_ISREG(info.st_mode) else None)
        result[path.relative_to(root).as_posix()] = [metadata, value]
    return result


def main():
    source, case = Path(sys.argv[1]), Path(sys.argv[2])
    check(sys.platform == 'linux' and os.geteuid() != 0 and case.is_absolute() and not case.exists(), 'fresh normal-user fixture')
    case.mkdir(mode=0o700)
    spec = importlib.util.spec_from_file_location('sfx_install', source)
    module = importlib.util.module_from_spec(spec); spec.loader.exec_module(module)
    check(module.LAYOUT_SCHEMA == 'linux-lowercase-v1' and module.LAYOUT_MARKER == 'config/linux-layout.v1' and
          module.LAYOUT_BYTES == b'SFX-LINUX-LAYOUT-1\nlinux-lowercase-v1\n',
          'layout schema, marker path and literal bytes match the approved contract')
    package = case / 'package'; package.mkdir(mode=0o700)
    names = {'bin/sfxweb': (b'program-r1', 'always', '0755'), 'spitfire-x': (b'launcher-r1', 'always', '0755'),
             'prepare-sfx': (b'prepare-r1', 'always', '0755'), 'readme.txt': (b'readme-r1', 'always', '0644'),
             'config/linux-layout.v1': (module.LAYOUT_BYTES, 'fresh', '0600'),
             'docs/sysop/doors.html': (b'help-r1', 'always', '0644'),
             'docs/sysop/stock.html': (b'stock-help-r1', 'always', '0644'),
             'web/app.js': (b'web-r1', 'always', '0644'), 'display/main.ans': (b'original menu', 'fresh', '0644'),
             'doors/game/config.ini': (b'original Door settings', 'fresh', '0644')}

    def payload(version='test-r1'):
        rows = []
        for name, (data, policy, mode) in names.items():
            p = package / 'payload' / name; p.parent.mkdir(parents=True, exist_ok=True); p.write_bytes(data); p.chmod(0o644)
            rows.append(dict(path=name, bytes=len(data), sha256=hashlib.sha256(data).hexdigest(), policy=policy, mode=mode))
        (package / 'package-manifest.json').write_text(json.dumps(dict(format=1, platform='linux-amd64', layout_schema=module.LAYOUT_SCHEMA, version=version, files=rows)))
        (package / 'package-manifest.json').chmod(0o644)
    payload()

    def unchanged_refusal(target, label, upgrade=False, recover=None):
        before = snapshot(target)
        try:
            module.install(package, target, upgrade, recover)
        except (RuntimeError, OSError, ValueError, KeyError, TypeError):
            check(snapshot(target) == before, label + ': bytes, names, identity and permissions unchanged')
        else:
            raise AssertionError(label + ' accepted')

    for name in sorted(module.OWNED_ROOTS | module.ROOT_FILES):
        legacy = case / ('legacy-' + name)
        legacy.mkdir(mode=0o775); legacy.chmod(0o775)
        (legacy / name.upper()).mkdir(mode=0o700)
        unchanged_refusal(legacy, 'legacy owned root ' + name)
        check(not (legacy / '.sfx-runtime.lock').exists(), 'legacy refusal creates no lock: ' + name)
    for name in ('config/sfx.conf', 'data/sfx.db'):
        unmarked = case / ('unmarked-' + name.split('/')[0]); unmarked.mkdir(mode=0o775); unmarked.chmod(0o775)
        target = unmarked / name; target.parent.mkdir(mode=0o700); target.write_bytes(b'established board'); target.chmod(0o600)
        unchanged_refusal(unmarked, 'unmarked existing ' + name, True)
    old_receipt = case / 'legacy-receipt'; old_receipt.mkdir(mode=0o775); old_receipt.chmod(0o775)
    (old_receipt / '.sfx-installed.json').write_text(json.dumps(dict(format=1, version='old', stock_docs={})))
    (old_receipt / '.sfx-installed.json').chmod(0o600)
    unchanged_refusal(old_receipt, 'old receipt without schema is not adopted', True)
    linked_root = case / 'linked root'; linked_target = case / 'linked target'
    linked_target.mkdir(mode=0o775); linked_target.chmod(0o775); linked_root.symlink_to(linked_target)
    try:
        module.install(package, linked_root)
        raise AssertionError('linked outer folder accepted')
    except RuntimeError:
        check(linked_target.stat().st_mode & 0o777 == 0o775, 'linked outer target remains unchanged')
    regular_root = case / 'regular root'; regular_root.write_bytes(b'keep')
    try:
        module.install(package, regular_root)
        raise AssertionError('regular-file outer folder accepted')
    except (OSError, RuntimeError):
        check(regular_root.read_bytes() == b'keep', 'regular-file outer target refused unchanged')
    other_root = Path('/tmp'); other_mode = other_root.stat().st_mode
    check(other_root.stat().st_uid != os.geteuid(), 'foreign owner fixture is actually foreign')
    try:
        module.normalize_outer_directory(other_root)
        raise AssertionError('foreign owned outer folder accepted')
    except RuntimeError:
        check(other_root.stat().st_mode == other_mode, 'foreign owner refused without chmod')
    unsafe_root = case / 'unsafe child'; unsafe_root.mkdir(mode=0o775); unsafe_root.chmod(0o775)
    (unsafe_root / 'web').mkdir(mode=0o775); (unsafe_root / 'web').chmod(0o775)
    try:
        module.install(package, unsafe_root)
        raise AssertionError('shared writable child accepted')
    except RuntimeError:
        check((unsafe_root / 'web').stat().st_mode & 0o777 == 0o775 and not (unsafe_root / 'bin').exists(),
              'unsafe child refused without recursive chmod or publication')
    absent_root = case / 'absent board'; module.install(package, absent_root)
    check(absent_root.stat().st_mode & 0o777 == 0o700, 'absent fresh root installs privately')
    check((absent_root / 'ftn').is_dir() and (absent_root / 'ftn').stat().st_mode & 0o777 == 0o700, 'normal fresh install provisions private FTN role')
    (absent_root / 'ftn').rmdir()
    module.install(package, absent_root, True)
    check((absent_root / 'ftn').is_dir(), 'explicit normal update provisions missing FTN role')
    held = absent_root / 'ftn/held-original.pkt'; held.write_bytes(b'keep original'); held.chmod(0o600)
    module.install(package, absent_root, True)
    check(held.read_bytes() == b'keep original', 'normal update preserves existing FTN mail')

    def current_copy(label):
        target = case / label
        shutil.copytree(str(absent_root), str(target))
        target.chmod(0o775)
        return target

    for name in ('config/SFX.conf', 'data/SFX.DB-wal', 'bin/Tools/probe', 'docs/Sysop/probe',
                 'files/UploadToSysop/probe', 'backup/Scheduled/probe', 'ftn/External/probe',
                 'logs/CALLERS.LOG', 'logs/heysysop-20261116T120000000Z-ABCDEF0123456789ABCDEF0123456789.LOG'):
        mixed = current_copy('mixed-' + str(len(checks)))
        target = mixed / name; target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(b'keep mixed state'); target.chmod(0o600)
        unchanged_refusal(mixed, 'mixed nested path ' + name, True)
    for variant in ('missing', 'wrong', 'linked', 'hard-linked'):
        bad_marker = current_copy('marker-' + variant)
        target = bad_marker / module.LAYOUT_MARKER
        if variant == 'missing': target.unlink()
        elif variant == 'wrong': target.write_bytes(b'not the current layout\n')
        elif variant == 'linked':
            target.unlink(); target.symlink_to(absent_root / module.LAYOUT_MARKER)
        else:
            target.unlink(); os.link(str(absent_root / module.LAYOUT_MARKER), str(target))
        unchanged_refusal(bad_marker, 'invalid marker ' + variant, True)
        if variant == 'hard-linked': target.unlink()
    for variant in ('missing-schema', 'wrong-schema', 'uppercase-doc-inventory'):
        bad_receipt = current_copy('receipt-' + variant)
        target = bad_receipt / '.sfx-installed.json'; saved = json.loads(target.read_text())
        if variant == 'missing-schema': saved.pop('layout_schema')
        elif variant == 'wrong-schema': saved['layout_schema'] = 'other-layout'
        else: saved['stock_docs'] = {'Docs/sysop/doors.html': '0' * 64}
        target.write_text(json.dumps(saved))
        unchanged_refusal(bad_receipt, 'invalid receipt ' + variant, True)

    manifest_path = package / 'package-manifest.json'
    good_manifest = manifest_path.read_bytes()
    for variant in ('missing-schema', 'wrong-schema', 'wrong-platform', 'uppercase-entry', 'other-config-seed',
                    'marker-always', 'marker-public', 'invalid-mode', 'duplicate-entry', 'invalid-hash'):
        bad = json.loads(good_manifest)
        if variant == 'missing-schema': bad.pop('layout_schema')
        elif variant == 'wrong-schema': bad['layout_schema'] = 'other-layout'
        elif variant == 'wrong-platform': bad['platform'] = 'other-platform'
        elif variant == 'uppercase-entry': bad['files'][0]['path'] = 'Bin/SFXWeb'
        elif variant == 'other-config-seed': bad['files'][0]['path'] = 'config/sfx.conf'
        elif variant == 'marker-always': next(row for row in bad['files'] if row['path'] == module.LAYOUT_MARKER)['policy'] = 'always'
        elif variant == 'marker-public': next(row for row in bad['files'] if row['path'] == module.LAYOUT_MARKER)['mode'] = '0644'
        elif variant == 'invalid-mode': bad['files'][0]['mode'] = '0777'
        elif variant == 'duplicate-entry': bad['files'].append(dict(bad['files'][0]))
        else: bad['files'][0]['sha256'] = '0' * 64
        manifest_path.write_text(json.dumps(bad))
        untouched = case / ('bad-package-' + variant); untouched.mkdir(mode=0o775); untouched.chmod(0o775)
        unchanged_refusal(untouched, 'bad package ' + variant)
        unchanged_refusal(case / ('absent-bad-package-' + variant), 'bad package creates no root ' + variant)
    manifest_path.write_bytes(good_manifest)
    original_marker = names[module.LAYOUT_MARKER]
    names[module.LAYOUT_MARKER] = (b'wrong schema bytes\n', 'fresh', '0600'); payload()
    unchanged_refusal(case / 'bad-marker-content', 'manifest-matched wrong marker content creates no root')
    names[module.LAYOUT_MARKER] = original_marker; payload()

    vendor = dict(names)
    names['bin/tzdata/America/Los_Angeles'] = (b'IANA fixture', 'always', '0644')
    names['docs/thirdparty/clamav/COPYING/COPYING'] = (b'vendor notice fixture', 'always', '0644')
    payload(); vendor_root = case / 'vendor islands'; module.install(package, vendor_root)
    check((vendor_root / 'bin/tzdata/America/Los_Angeles').is_file() and
          (vendor_root / 'docs/thirdparty/clamav/COPYING/COPYING').is_file(), 'only explicit vendor and IANA islands retain required spelling')
    names.clear(); names.update(vendor); payload()
    for kind in ('linked', 'shared', 'file'):
        bad = case / ('unsafe FTN ' + kind); bad.mkdir(mode=0o700)
        role = bad / 'ftn'
        if kind == 'linked': role.symlink_to(absent_root / 'ftn', target_is_directory=True)
        elif kind == 'shared': role.mkdir(mode=0o775); role.chmod(0o775)
        else: role.write_bytes(b'keep role file')
        try:
            module.install(package, bad)
            raise AssertionError('unsafe FTN role accepted')
        except RuntimeError:
            check(not (bad / 'bin').exists() and held.read_bytes() == b'keep original', 'unsafe FTN role refused before program publication: ' + kind)
    root = case / 'fresh board'; root.mkdir(mode=0o775); root.chmod(0o775)
    module.install(package, root)
    check(root.stat().st_mode & 0o777 == 0o700, 'normal copied outer permissions become private')
    check((root / 'bin').stat().st_mode & 0o777 == 0o700 and (root / 'web').stat().st_mode & 0o777 == 0o700, 'every newly created component folder private')
    check((root / 'bin/sfxweb').read_bytes() == b'program-r1' and os.access(root / 'spitfire-x', os.X_OK), 'normal program and launcher installed')
    check(not (root / 'data').exists() and not (root / 'config/sfx.conf').exists() and
          (root / module.LAYOUT_MARKER).read_bytes() == module.LAYOUT_BYTES,
          'fresh installer ships exact schema marker without initialized database or setup state')
    (root / 'display/main.ans').write_bytes(b'owner artwork'); (root / 'doors/game/config.ini').write_bytes(b'owner Door choices')
    (root / 'data').mkdir(mode=0o700); (root / 'data/sfx.db').write_bytes(b'owner database'); (root / 'data/sfx.db').chmod(0o600)
    (root / 'config/board.key').write_bytes(b'owner private key'); (root / 'config/board.key').chmod(0o600)
    (root / 'docs/sysop/doors.html').write_bytes(b'owner help edit')
    (root / 'docs/sysop/unknown.html').write_bytes(b'owner unrecognized help'); (root / 'docs/sysop/unknown.html').chmod(0o600)
    keep = {p: p.read_bytes() for p in (root / 'display/main.ans', root / 'doors/game/config.ini', root / 'data/sfx.db', root / 'config/board.key')}
    names['docs/sysop/doors.html'] = (b'help-r2', 'always', '0644')
    names['docs/sysop/stock.html'] = (b'stock-help-r2', 'always', '0644')
    names['docs/sysop/unknown.html'] = (b'new-stock-help', 'always', '0644')
    names['bin/sfxweb'] = (b'program-r2', 'always', '0755'); payload('test-r2')
    module.install(package, root, True)
    check((root / 'bin/sfxweb').read_bytes() == b'program-r2', 'upgrade replaces program')
    check(all(p.read_bytes() == data for p, data in keep.items()), 'upgrade preserves database, key, artwork and Door settings')
    check((root / 'docs/sysop/doors.html').read_bytes() == b'owner help edit' and
          (root / 'docs/sysop/unknown.html').read_bytes() == b'owner unrecognized help',
          'same-layout repair preserves edited and unrecognized help')
    receipt = json.loads((root / '.sfx-installed.json').read_text())
    check(receipt['layout_schema'] == module.LAYOUT_SCHEMA and receipt['stock_docs']['docs/sysop/doors.html'] ==
          hashlib.sha256(b'help-r1').hexdigest() and 'docs/sysop/unknown.html' not in receipt['stock_docs'],
          'receipt records schema and never adopts owner help edits as stock')
    check((root / 'docs/sysop/stock.html').read_bytes() == b'stock-help-r2', 'unchanged recognized lowercase stock help updates normally')
    check(not list(root.glob('.sfx-update-*')), 'verified successful update retires only its transaction')
    fd = os.open(str(root / '.sfx-runtime.lock'), os.O_RDWR); fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
    refused = False
    try:
        module.install(package, root, True)
    except RuntimeError as error:
        refused = '--stop-management' in str(error) and 'Closing the browser is not enough' in str(error)
        command = str(error).split('then run ', 1)[1].split(' before updating.', 1)[0]
        check(shlex.split(command) == [str(root / 'spitfire-x'), '--stop-management'], 'busy guidance preserves spaced folder and separate option')
    finally:
        os.close(fd)
    check(refused, 'actual held management lock refuses update')
    old = {name: (root / name).read_bytes() for name in ('bin/sfxweb', 'spitfire-x', 'web/app.js', '.sfx-installed.json')}
    names['bin/sfxweb'] = (b'program-r3', 'always', '0755'); names['web/app.js'] = (b'web-r3', 'always', '0644'); payload('test-r3')
    real_replace = module.os.replace; failed = [False]
    def interrupted(src, dest):
        if not failed[0] and '/new/web/' in src:
            failed[0] = True; raise OSError('injected publication failure')
        return real_replace(src, dest)
    module.os.replace = interrupted
    try:
        module.install(package, root, True)
        raise AssertionError('failure injection was not reached')
    except OSError as error:
        check('injected publication failure' in str(error), 'publication failure surfaced')
    finally:
        module.os.replace = real_replace
    check(all((root / name).read_bytes() == data for name, data in old.items()), 'failed update restores exact old programs and manifest')
    check(all(p.read_bytes() == data for p, data in keep.items()), 'failed update preserves board content')
    real_write = module.write_new
    def stage_failed(path, data, mode=0o600):
        if '/new/web/' in str(path):
            raise OSError('injected staging failure')
        return real_write(path, data, mode)
    module.write_new = stage_failed
    try:
        module.install(package, root, True)
        raise AssertionError('staging failure not reached')
    except OSError:
        pass
    finally:
        module.write_new = real_write
    pending = list(root.glob('.sfx-update-*')); check(len(pending) == 1, 'interrupted staging retains exact journal')
    journal = pending[0] / 'journal.json'; journal_bytes = journal.read_bytes()
    plan = json.loads(journal_bytes); check(plan['layout_schema'] == module.LAYOUT_SCHEMA and not plan['fresh_install'], 'update journal records explicit current layout')
    plan.pop('layout_schema'); journal.write_text(json.dumps(plan)); root.chmod(0o775)
    unchanged_refusal(root, 'untagged recovery journal refuses before chmod', True, pending[0].name)
    journal.write_bytes(journal_bytes)
    module.install(package, root, True, pending[0].name)
    check(all((root / name).read_bytes() == data for name, data in old.items()), 'explicit rollback restores pre-update state without replaying install')

    # Receipt publication can fail after its old bytes move into the transaction.
    failed = [False]
    def receipt_interrupted(src, dest):
        if not failed[0] and '/new/' in src and dest.endswith('/.sfx-installed.json'):
            failed[0] = True; raise OSError('injected receipt publication failure')
        return real_replace(src, dest)
    module.os.replace = receipt_interrupted
    try:
        module.install(package, root, True)
        raise AssertionError('receipt failure injection was not reached')
    except OSError as error:
        check('injected receipt publication failure' in str(error), 'receipt publication failure surfaced')
    finally:
        module.os.replace = real_replace
    check(all((root / name).read_bytes() == data for name, data in old.items()), 'retained old receipt permits exact rollback while current receipt is temporarily absent')

    fresh_interrupted = case / 'fresh interrupted staging'
    module.write_new = stage_failed
    try:
        module.install(package, fresh_interrupted)
        raise AssertionError('fresh staging injection was not reached')
    except OSError as error:
        check('injected staging failure' in str(error), 'fresh staging failure surfaced')
    finally:
        module.write_new = real_write
    fresh_pending = list(fresh_interrupted.glob('.sfx-update-*'))
    check(len(fresh_pending) == 1 and not (fresh_interrupted / module.LAYOUT_MARKER).exists(), 'partial fresh journal exists before marker publication')
    unexpected = fresh_interrupted / 'config/sfx.conf'; unexpected.write_bytes(b'unjournaled board state'); unexpected.chmod(0o600)
    unchanged_refusal(fresh_interrupted, 'partial fresh rollback refuses unjournaled board state', recover=fresh_pending[0].name)
    unexpected.unlink()
    mixed_role = fresh_interrupted / 'Config'; mixed_role.mkdir(mode=0o700)
    unchanged_refusal(fresh_interrupted, 'partial fresh rollback refuses legacy mixed role', recover=fresh_pending[0].name)
    mixed_role.rmdir()
    module.install(case / 'unavailable package', fresh_interrupted, recover=fresh_pending[0].name)
    check(not list(fresh_interrupted.glob('.sfx-update-*')) and not (fresh_interrupted / module.LAYOUT_MARKER).exists(),
          'tagged partial fresh rollback needs neither marker nor original package')

    for premarked in (False, True):
        partial = case / ('fresh publication ' + str(premarked))
        if premarked:
            (partial / 'config').mkdir(parents=True, mode=0o700)
            (partial / module.LAYOUT_MARKER).write_bytes(module.LAYOUT_BYTES); (partial / module.LAYOUT_MARKER).chmod(0o600)
            unrelated = partial / 'config/unrelated.txt'
            unrelated.write_bytes(b'keep this unrelated file')
            unchanged_refusal(partial, 'fresh retry with valid marker refuses unrelated files')
            unrelated.unlink()
        failed = [False]; module.os.replace = interrupted
        try:
            module.install(package, partial)
            raise AssertionError('fresh publication injection was not reached')
        except OSError as error:
            check('injected publication failure' in str(error), 'fresh publication failure surfaced: ' + str(premarked))
        finally:
            module.os.replace = real_replace
        check(not list(partial.glob('.sfx-update-*')) and not (partial / 'bin/sfxweb').exists() and
              (partial / module.LAYOUT_MARKER).exists() == premarked,
              'fresh publication rollback restores original marker presence: ' + str(premarked))
        module.install(package, partial)
        check((partial / module.LAYOUT_MARKER).read_bytes() == module.LAYOUT_BYTES, 'fresh install can resume from safely rolled-back empty tree: ' + str(premarked))

    hard_target = current_copy('hard-linked target')
    (hard_target / 'web/app.js').unlink(); os.link(str(absent_root / 'web/app.js'), str(hard_target / 'web/app.js'))
    unchanged_refusal(hard_target, 'hard-linked target refuses before chmod', True)
    (hard_target / 'web/app.js').unlink()
    hard_source = case / 'hard-linked package source'; os.link(str(package / 'payload/web/app.js'), str(hard_source))
    unchanged_refusal(case / 'hard-linked package destination', 'hard-linked package source creates no destination')
    hard_source.unlink()
    (package / 'payload/web/app.js').write_bytes(b'tampered')
    refused = False
    try:
        module.install(package, root, True)
    except RuntimeError as error:
        refused = 'checksum' in str(error)
    check(refused and not list(root.glob('.sfx-update-*')), 'bad package checksum refused before publication')
    payload('test-r3'); (root / 'web/app.js').rename(root / 'web/kept.js'); (root / 'web/app.js').symlink_to(root / 'web/kept.js')
    refused = False
    try:
        module.install(package, root, True)
    except RuntimeError:
        refused = True
    check(refused and (root / 'web/kept.js').read_bytes() == old['web/app.js'], 'linked destination refused and link target retained')
    (case / 'checks.json').write_text(json.dumps(checks, indent=2)); print('PASS', len(checks), 'Linux installer checks')


if __name__ == '__main__':
    main()
