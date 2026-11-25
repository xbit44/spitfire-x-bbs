"""Read-only path preflight tests; never launch DOS, download or change a board."""
import ast
from pathlib import Path
import tempfile


ROOT = Path(__file__).resolve().parents[2]
source = ast.parse((ROOT / 'scripts/prepare_linux_dos.py').read_text())
selected = [node for node in source.body if isinstance(node, ast.FunctionDef)
            and node.name in ('require', 'refuse_mixed_owned_roots')]
assert len(selected) == 2
scope = {}
exec(compile(ast.Module(body=selected, type_ignores=[]), 'actual_dos_layout_preflight', 'exec'), scope)
check = scope['refuse_mixed_owned_roots']
count = 0
with tempfile.TemporaryDirectory(prefix='sfx-beta11-path-only-') as temporary:
    root = Path(temporary)
    for name in ('bin', 'service', 'OwnerLibrary', 'A Game With CAPS'):
        (root / name).mkdir()
    before = sorted(p.name for p in root.iterdir())
    check(root)
    assert sorted(p.name for p in root.iterdir()) == before
    count += 1
    for name in ('Config', 'Data', 'Files', 'Management', 'Service'):
        case = root / ('case-' + name.lower())
        case.mkdir()
        (case / name).mkdir()
        before = sorted(p.name for p in case.iterdir())
        try:
            check(case)
        except RuntimeError as error:
            assert 'nothing was changed' in str(error)
        else:
            raise AssertionError('Known mixed owned role accepted: ' + name)
        assert sorted(p.name for p in case.iterdir()) == before
        count += 1
print('PASS', count, 'actual DOS helper read-only lowercase/owner-path preflight cases; no DOS execution')
