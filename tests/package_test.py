#!/usr/bin/env python3
"""Inspect distributions and exercise preflight without sudo or installation."""
import hashlib
import plistlib
import subprocess
import tempfile
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
VERSION = (ROOT / 'VERSION').read_text().strip()
NAME = f'Audio8DJ-Mac-Driver-{VERSION}'


def run(*args, cwd=None, ok=True):
    result = subprocess.run([str(a) for a in args], cwd=cwd, capture_output=True, text=True)
    if ok and result.returncode:
        raise AssertionError(result.stdout + result.stderr)
    if not ok and not result.returncode:
        raise AssertionError('Expected rejection')
    return result


def files(base):
    return {p.relative_to(base).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
            for p in base.rglob('*') if p.is_file()}


with tempfile.TemporaryDirectory(prefix='a8dj-package-test-') as tmp:
    tmp = Path(tmp)
    # Deliberately extract under a directory with spaces, as users commonly do.
    extracted = tmp / 'Downloaded release with spaces'
    with zipfile.ZipFile(ROOT / 'dist' / f'{NAME}.zip') as archive:
        archive.extractall(extracted)
    release = extracted / NAME
    run('bash', 'install.command', '--dry-run', cwd=release)
    # Missing files must fail before authentication or any system operation.
    agent = release / 'payload/usr/local/bin/a8dj-midid'
    saved = agent.read_bytes()
    agent.unlink()
    run('bash', 'install.command', '--dry-run', cwd=release, ok=False)
    agent.write_bytes(saved + b'corrupt')
    run('bash', 'install.command', '--dry-run', cwd=release, ok=False)
    agent.write_bytes(saved)
    # Scripts must parse; pkg hooks must reject non-startup target volumes.
    for source in (ROOT / 'packaging').rglob('*'):
        if source.is_file() and (source.suffix in ('.sh', '.command') or source.name in ('preinstall', 'postinstall')):
            run('bash', '-n', source)
    run('bash', ROOT / 'packaging/scripts/preinstall', 'test.pkg', '/', '/Volumes/Other', ok=False)
    expanded = tmp / 'expanded-pkg'
    run('pkgutil', '--expand-full', ROOT / 'dist' / f'{NAME}-unsigned.pkg', expanded)
    payloads = list(expanded.glob('*.pkg/Payload'))
    assert len(payloads) == 1, payloads
    assert files(payloads[0]) == files(release / 'payload'), 'ZIP/PKG payload mismatch'
    payload = payloads[0]
    expected = {
        'Library/Audio/Plug-Ins/HAL/Audio8DJ.driver/Contents/Info.plist',
        'Library/Audio/Plug-Ins/HAL/Audio8DJ.driver/Contents/MacOS/Audio8DJ',
        'Library/Audio/Plug-Ins/HAL/Audio8DJ.driver/Contents/_CodeSignature/CodeResources',
        'Library/LaunchAgents/com.sawada.audio8dj.midid.plist',
        'Library/Application Support/Audio8DJ/manage.sh',
        'Library/Application Support/Audio8DJ/README.txt',
        'usr/local/bin/a8dj-midid',
    }
    assert set(files(payload)) == expected
    for binary in ('Library/Audio/Plug-Ins/HAL/Audio8DJ.driver/Contents/MacOS/Audio8DJ',
                   'usr/local/bin/a8dj-midid'):
        run('lipo', payload / binary, '-verify_arch', 'arm64', 'x86_64')
        run('codesign', '--verify', '--strict', payload / binary)
    config = plistlib.loads((payload / 'Library/LaunchAgents/com.sawada.audio8dj.midid.plist').read_bytes())
    assert config['ProgramArguments'] == ['/usr/local/bin/a8dj-midid']
    assert config['LimitLoadToSessionType'] == 'Aqua'
    assert 'UserName' not in config  # user LaunchAgent, not root daemon
    print('PASS: ZIP preflight, missing/corrupt rejection, paths with spaces, pkg target guard,')
    print('      identical ZIP/PKG payloads, expected paths, Universal binaries and signatures')
