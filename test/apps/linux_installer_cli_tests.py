"""Focused prompt/CLI checks; no SFX program or browser is started."""
import contextlib
import importlib.util
import io
from pathlib import Path
import sys
from unittest import mock


def main():
    source = Path(sys.argv[1])
    spec = importlib.util.spec_from_file_location('installer_cli', source)
    module = importlib.util.module_from_spec(spec); spec.loader.exec_module(module)
    checks = []

    def check(value, text):
        assert value, text
        checks.append(text)

    def choose(answers, *, tty=True):
        with mock.patch.object(module.sys.stdin, 'isatty', return_value=tty), \
                mock.patch('builtins.input', side_effect=answers), \
                contextlib.redirect_stdout(io.StringIO()):
            return module.choose_prefix(None)

    check(choose(['', '']) == Path.home() / 'sfx', 'default folder accepted after confirmation')
    check(choose(['/tmp/SFX test board', 'yes']) == Path('/tmp/SFX test board'), 'custom absolute folder including spaces')
    check(choose(['~/sfx-other', 'y']) == Path.home() / 'sfx-other', 'explicit tilde expansion')
    check(choose(['/tmp/no-write', 'n']) is None, 'No cancels before install')
    check(choose(['/tmp/no-write', 'cancel']) is None, 'unknown confirmation cancels')
    check(choose([EOFError()]) is None, 'EOF cancels before install')
    check(choose([KeyboardInterrupt()]) is None, 'Ctrl-C cancels before install')
    try:
        choose(['relative/path'])
        raise AssertionError('relative path accepted')
    except ValueError:
        checks.append('relative path refused before confirmation')
    try:
        choose([], tty=False)
        raise AssertionError('noninteractive implicit default accepted')
    except ValueError as error:
        check('--prefix' in str(error), 'noninteractive missing prefix explains explicit option')
    with mock.patch('builtins.input', side_effect=AssertionError('prompted with explicit prefix')):
        check(module.choose_prefix('/tmp/explicit') == Path('/tmp/explicit'), '--prefix does not prompt')
    with mock.patch.object(module, 'choose_prefix', return_value=None), \
            mock.patch.object(module, 'install') as install, mock.patch.object(sys, 'argv', [str(source)]):
        check(module.main() == 0 and not install.called, 'main never installs after cancellation')
    with mock.patch.object(module, 'install', return_value=Path('/nonexistent/sfx-test')) as install, \
            mock.patch.object(sys, 'argv', [str(source), '--prefix', '/tmp/explicit', '--upgrade']), \
            contextlib.redirect_stdout(io.StringIO()):
        check(module.main() == 0, 'explicit-prefix main succeeds')
        check(install.call_args.args[1:] == (Path('/tmp/explicit'), True, None), 'upgrade arguments preserved')
    print('PASS ' + str(len(checks)) + ' installer prompt/CLI checks')


if __name__ == '__main__':
    main()
